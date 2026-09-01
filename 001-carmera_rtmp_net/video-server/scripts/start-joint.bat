@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  Joint launcher:  video-server  +  carmera-agent
REM
REM  Topology:
REM    UVC camera -> camera-agent.exe (GStreamer) --RTSP push--> MediaMTX
REM              -> video-server.exe monitor auto-registers the path
REM              -> Web UI / WebRTC / HLS on http://localhost:<http_port>
REM
REM  ONE MediaMTX ONLY - this is the whole point of this script.
REM    video-server launches MediaMTX itself, and that instance is the one with
REM    the control API on :9997 that its monitor polls every 3s to discover
REM    cameras. carmera-agent has its own mediamtx.yml / start-camera-agent.bat,
REM    but running those would start a SECOND MediaMTX fighting over :8554 -
REM    and it would be the wrong instance anyway, since it has no API for the
REM    server to poll. So we launch the agent binary directly and never start
REM    another MediaMTX.
REM
REM  External tools are called by bare name and must be on PATH.
REM  Only the two project binaries are referenced relative to this script.
REM
REM  Environment overrides (argument > env var > default):
REM    CAMERA_ID       camera index, see: camera-agent --list   (default: auto)
REM    STREAM_ID       RTSP path name                           (default camera01)
REM    CAMERA_SOURCE   force a GStreamer source element         (default: auto)
REM    NO_BROWSER      1 = do not open the browser              (default 0)
REM
REM  Examples:
REM    scripts\start-joint.bat
REM    scripts\start-joint.bat --camera 0 --stream cam01
REM    scripts\start-joint.bat --no-browser
REM
REM  NOTE: --auto is always passed to the agent. The UVC camera here only does
REM        240x240@8fps natively, so forcing the 1280x720 from camera-agent.yaml
REM        makes caps negotiation fail and the agent loops on reconnect forever.
REM ============================================================================

REM ---- paths: this script lives in <video-server>\scripts ----
set SCRIPT_DIR=%~dp0
if "%SCRIPT_DIR:~-1%"=="\" set SCRIPT_DIR=%SCRIPT_DIR:~0,-1%
for %%i in ("%SCRIPT_DIR%\..") do set "ROOT=%%~fi"
for %%i in ("%ROOT%\..") do set "PARENT=%%~fi"
set AGENT_DIR=%PARENT%\carmera-agent
set LOGS=%ROOT%\logs
if not exist "%LOGS%" mkdir "%LOGS%"

REM Use the joint config by default: it moves HTTP off 8080, which the
REM "ApplicationWebServer" service on this machine grabs, so the demo does not
REM randomly fail to bind. Override with: set VS_CONFIG=config.yaml
set CFG_NAME=config.joint.yaml
if defined VS_CONFIG set CFG_NAME=%VS_CONFIG%
set CFG=%ROOT%\config\%CFG_NAME%
REM Prefer the canonical binary; fall back to the alternate name build.bat
REM produces when video-server.exe is locked by a still-running instance.
set VS_EXE=%ROOT%\video-server.exe
if not exist "%VS_EXE%" if exist "%ROOT%\video-server.new.exe" set VS_EXE=%ROOT%\video-server.new.exe
set CA_EXE=%AGENT_DIR%\build-msvc\src\camera-agent.exe

echo ============================================================
echo  Joint run: video-server + carmera-agent
echo  server root: %ROOT%
echo  agent  root: %AGENT_DIR%
echo ============================================================

REM ---- defaults ----
if not defined STREAM_ID   set STREAM_ID=camera01
if not defined NO_BROWSER  set NO_BROWSER=0
set "CAMERA_ARG="
set "SOURCE_ARG="

REM ---- parse args ----
set "_ctx="
for %%a in (%*) do (
  if defined _ctx (
    if "!_ctx!"=="camera" set "CAMERA_ID=%%~a"
    if "!_ctx!"=="stream" set "STREAM_ID=%%~a"
    if "!_ctx!"=="source" set "CAMERA_SOURCE=%%~a"
    set "_ctx="
  ) else (
    if /i "%%a"=="--no-browser" set NO_BROWSER=1
    if /i "%%a"=="--camera"     set "_ctx=camera"
    if /i "%%a"=="--stream"     set "_ctx=stream"
    if /i "%%a"=="--source"     set "_ctx=source"
  )
)
if defined CAMERA_ID   set "CAMERA_ARG=--camera %CAMERA_ID%"
if defined CAMERA_SOURCE set "SOURCE_ARG=--source %CAMERA_SOURCE%"

REM ---- (1) pre-check project binaries ----
if not exist "%VS_EXE%" (
  echo [ERROR] video-server.exe not found: %VS_EXE%
  echo         Build it: scripts\build.bat
  goto :fail
)
echo [OK]    video-server.exe

if not exist "%CA_EXE%" (
  echo [ERROR] camera-agent.exe not found: %CA_EXE%
  echo         Build it: cd %AGENT_DIR% ^&^& build_oneclick.bat
  goto :fail
)
echo [OK]    camera-agent.exe

if not exist "%CFG%" (
  echo [ERROR] server config not found: %CFG%
  goto :fail
)
echo [OK]    config\%CFG_NAME%

REM ---- (2) read ports out of the server config -------------------------
REM Ports live in one place only - the YAML - so read them back instead of
REM duplicating the numbers here. Matches on "key: value" and trims spaces.
REM The first bare "port:" in the file is rtsp.port; server uses http_port.
set HTTP_PORT=8080
set RTSP_PORT=8554
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"http_port:" "%CFG%"`) do (
  for /f "tokens=1" %%v in ("%%b") do set "HTTP_PORT=%%v"
)
for /f "usebackq tokens=1,2 delims=:" %%a in (`findstr /r /c:"^  port:" "%CFG%"`) do (
  if "!RTSP_PORT_SET!"=="" (
    for /f "tokens=1" %%v in ("%%b") do set "RTSP_PORT=%%v"
    set RTSP_PORT_SET=1
  )
)
echo [OK]    ports: http=%HTTP_PORT% rtsp=%RTSP_PORT%

REM ---- (3) stop leftovers from a previous run --------------------------
echo [*] Stopping leftovers: video-server / mediamtx / camera-agent
taskkill /F /IM video-server.exe >nul 2>&1
taskkill /F /IM camera-agent.exe >nul 2>&1
taskkill /F /IM mediamtx.exe    >nul 2>&1
C:\Windows\System32\timeout.exe /t 2 >nul 2>&1
echo [OK]    stopped

REM ---- (4) start video-server (it spawns MediaMTX) ---------------------
echo [1/3] Starting video-server ...
start "video-server" /D "%ROOT%" cmd /c "video-server.exe config\%CFG_NAME% > "%LOGS%\video-server.log" 2>&1"

set READY=0
for /l %%i in (1,1,40) do (
  if "!READY!"=="0" (
    C:\Windows\System32\timeout.exe /t 1 >nul 2>&1
    curl -s -o nul -w "%%{http_code}" http://localhost:%HTTP_PORT%/api/health 2>nul | findstr "200" >nul && set READY=1
  )
)
if "%READY%"=="0" (
  echo [ERROR] video-server did not become ready - see %LOGS%\video-server.log
  goto :fail
)
echo [OK]    video-server healthy on :%HTTP_PORT%

REM ---- (5) pick a camera index that actually exists --------------------
REM A wrong index makes GStreamer fail to open the device and the agent exits
REM immediately, which looks like "nothing happened". Ask the binary.
if not defined CAMERA_ID (
  "%CA_EXE%" --list > "%LOGS%\cameras.txt" 2>nul
  set "AVAIL_IDS="
  if exist "%LOGS%\cameras.txt" (
    for /f "tokens=2" %%i in ('findstr /b /c:"Camera " "%LOGS%\cameras.txt"') do set "AVAIL_IDS=!AVAIL_IDS! %%i"
    del "%LOGS%\cameras.txt" >nul 2>&1
  )
  if defined AVAIL_IDS (
    for /f "tokens=1" %%i in ("!AVAIL_IDS!") do set "CAMERA_ID=%%i"
    echo [AUTO]   camera index auto-picked: !CAMERA_ID! - available:!AVAIL_IDS!
  ) else (
    set CAMERA_ID=0
    echo [note]   could not enumerate cameras - falling back to index 0
  )
  set "CAMERA_ARG=--camera !CAMERA_ID!"
)

REM ---- (6) start camera-agent, pushing at the SAME MediaMTX ------------
echo [2/3] Starting camera-agent --auto --stream %STREAM_ID% ...
set "AGENT_ARGS=%CAMERA_ARG% --stream %STREAM_ID% --server 127.0.0.1 --port %RTSP_PORT% --auto %SOURCE_ARG%"
echo        args: %AGENT_ARGS%
start "camera-agent" /D "%AGENT_DIR%" cmd /c ""%CA_EXE%" %AGENT_ARGS% --log-level info > "%LOGS%\agent.log" 2>&1"

REM ---- (7) wait for the server to auto-register the stream -------------
echo [3/3] Waiting for video-server to auto-register %STREAM_ID% ...
set CONN=0
for /l %%i in (1,1,30) do (
  if "!CONN!"=="0" (
    C:\Windows\System32\timeout.exe /t 2 >nul 2>&1
    curl -s http://localhost:%HTTP_PORT%/api/cameras 2>nul | findstr /i /c:"%STREAM_ID%" >nul && set CONN=1
  )
)
if "%CONN%"=="1" (
  echo [OK]    %STREAM_ID% registered
) else (
  echo [WARN]  %STREAM_ID% not registered within 60s - see %LOGS%\agent.log
)

REM ---- (8) open the Web UI ----
if "%NO_BROWSER%"=="0" start "" http://localhost:%HTTP_PORT%/

REM ---- summary ----
REM The server binds 0.0.0.0, so the same port is served on every local
REM address. Showing the LAN address here saves the usual "which IP do I use"
REM guesswork - the server prints the full list in logs\video-server.log and
REM exposes it as GET /api/net/addresses.
set "LAN_IP="
for /f "usebackq tokens=*" %%i in (`powershell -NoProfile -Command "(Get-NetIPAddress -AddressFamily IPv4 ^| Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } ^| Sort-Object -Property InterfaceMetric ^| Select-Object -First 1).IPAddress"`) do set "LAN_IP=%%i"

echo.
echo ============================================================
echo  Joint run started
echo  Web UI      : http://localhost:%HTTP_PORT%/
echo  REST API    : http://localhost:%HTTP_PORT%/api/cameras
echo  RTSP stream : rtsp://127.0.0.1:%RTSP_PORT%/%STREAM_ID%
if defined LAN_IP (
  echo  --- from another machine on the LAN ---
  echo  Web UI      : http://%LAN_IP%:%HTTP_PORT%/
  echo  RTSP stream : rtsp://%LAN_IP%:%RTSP_PORT%/%STREAM_ID%
  echo  If it times out, open the firewall first:
  echo     scripts\firewall-add.bat %HTTP_PORT% %RTSP_PORT%
)
echo  Camera      : index %CAMERA_ID%  - negotiated natively via --auto
echo  Logs        : %LOGS%\  (video-server.log / agent.log)
echo  Verify      : python scripts\verify_joint.py
echo  Stop        : scripts\stop-joint.bat
echo ============================================================
echo.
goto :done

:fail
echo.
echo [ABORTED] Fix the error above, then run this script again.
echo.
pause
exit /b 1

:done
exit /b 0
