@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM ============================================================================
REM  One-click demo launcher for the SOC camera / RTSP project.
REM
REM  Flow:
REM    (0) STOP all related processes first (clean start, no port conflicts)
REM    (1) pre-check tools
REM    (2) start Video Server  (spawns MediaMTX + Web UI on :8080)
REM    (3) start camera publisher (camera-agent if GStreamer, else ffmpeg)
REM    (4) verify: health + camera registration + decoded frame
REM
REM  On any critical command failure the script PAUSEs so the reason is visible,
REM  then exits. (Double-click still works: the spawned windows stay open.)
REM
REM  Publisher selection (automatic):
REM    * If GStreamer (gst-launch-1.0) is on PATH, camera-agent is the publisher.
REM    * Otherwise ffmpeg is used as an equivalent RTSP publisher (synthetic
REM      test pattern by default). Set CAM=REAL to use the real UVC camera.
REM ============================================================================

set ROOT=%~dp0
if "%ROOT:~-1%"=="\" set ROOT=%ROOT:~0,-1%
set VS_DIR=%ROOT%\video-server
set CA_DIR=%ROOT%\carmera-agent
set LOGS=%ROOT%\logs
if not exist "%LOGS%" mkdir "%LOGS%"

echo ============================================================
echo  SOC camera / RTSP one-click demo
echo  root : %ROOT%
echo ============================================================

REM ---- (0) STOP all related processes FIRST ----
echo [*] Stopping all related processes first ...
taskkill /F /IM video-server.exe >nul 2>&1
taskkill /F /IM mediamtx.exe    >nul 2>&1
taskkill /F /IM camera-agent.exe >nul 2>&1
taskkill /F /IM ffmpeg.exe      >nul 2>&1
C:\Windows\System32\timeout.exe /t 1 >nul 2>&1
echo [OK]    related processes stopped

REM ---- auto-detect external tools so a double-click works without them on PATH ----
call :add_path "D:\data\agent-tools\mediamtx_v1.20.1_windows_amd64"
call :add_path "D:\data\agent-tools\ffmpeg-master-latest-win64-gpl-shared\bin"

REM ---- (1) pre-check: video-server binary ----
set VS_EXE=%VS_DIR%\video-server.exe
if not exist "%VS_EXE%" (
  echo [ERROR] video-server.exe not found at:
  echo         %VS_EXE%
  echo         Build it first: cd video-server ^&^& scripts\build.bat
  goto :fail
)
echo [OK]    video-server.exe

REM ---- (1) pre-check: mediamtx binary (read from video-server config) ----
for /f "tokens=*" %%l in ('findstr /i "binary:" "%VS_DIR%\config\config.yaml"') do set "MT_LINE=%%l"
for /f "tokens=2 delims=:" %%b in ("%MT_LINE%") do set "MT_BIN=%%b"
set "MT_BIN=%MT_BIN: =%"
if not exist "%MT_BIN%" (
  echo [ERROR] mediamtx binary not found (config says: %MT_BIN%)
  echo         Install MediaMTX or fix mediamtx.binary in video-server/config/config.yaml
  goto :fail
)
echo [OK]    mediamtx binary: %MT_BIN%

REM ---- (1) pre-check: ffmpeg (publisher fallback / verification) ----
where ffmpeg >nul 2>&1
if errorlevel 1 (
  echo [ERROR] ffmpeg not found on PATH (needed as the RTSP publisher here).
  echo         Add ffmpeg bin to PATH or to the :add_path probe list above.
  goto :fail
)
echo [OK]    ffmpeg

REM ---- (2) start Video Server (it spawns MediaMTX + Web UI) ----
echo [*] Starting Video Server (MediaMTX + Web UI on :8080) ...
start "video-server" /D "%VS_DIR%" cmd /c "video-server.exe config/config.yaml > "%LOGS%\video-server.log" 2>&1"

REM ---- wait for health (fail -> pause so the reason is visible) ----
set READY=0
for /l %%i in (1,1,40) do (
  if "!READY!"=="0" (
    C:\Windows\System32\timeout.exe /t 1 >nul 2>&1
    curl -s -o nul -w "%%{http_code}" http://localhost:8080/api/health 2>nul | findstr "200" >nul && set READY=1
  )
)
if "%READY%"=="1" ( echo [OK]    video-server health OK ) else (
  echo [ERROR] video-server did not become ready - see %LOGS%\video-server.log
  goto :fail
)

REM ---- (3) start the camera publisher ----
set CAM=%CAM%
if "%CAM%"=="" set CAM=TEST
where gst-launch-1.0 >nul 2>&1
if not errorlevel 1 (
  echo [*] Starting camera-agent (gstreamer backend) as publisher ...
  set CA_EXE=%CA_DIR%\build-msvc\src\camera-agent.exe
  if not exist "%CA_EXE%" (
    echo [ERROR] camera-agent.exe not found at %CA_EXE% (build it: build_oneclick.bat)
    goto :fail
  )
  start "camera-agent" /D "%CA_DIR%" cmd /c "%CA_EXE% --camera 0 --stream camera01 --log-level info > "%LOGS%\agent.log" 2>&1"
) else (
  if /i "%CAM%"=="REAL" (
    echo [*] Starting ffmpeg publisher (real UVC camera) ...
    start "camera-feed" cmd /c "ffmpeg -hide_banner -loglevel error -re -f dshow -i video=^"UVC Control^" -pix_fmt yuv420p -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -rtsp_transport tcp -f rtsp rtsp://127.0.0.1:8554/camera01 > "%LOGS%\feed.log" 2>&1"
  ) else (
    echo [*] Starting ffmpeg publisher (synthetic test pattern) ...
    start "camera-feed" cmd /c "ffmpeg -hide_banner -loglevel error -re -f lavfi -i testsrc=size=320x240:rate=15 -pix_fmt yuv420p -c:v libx264 -preset ultrafast -tune zerolatency -g 30 -keyint_min 30 -rtsp_transport tcp -f rtsp rtsp://127.0.0.1:8554/camera01 > "%LOGS%\feed.log" 2>&1"
  )
)

REM ---- wait for camera auto-registration (fail -> pause) ----
set CONN=0
for /l %%i in (1,1,30) do (
  if "!CONN!"=="0" (
    C:\Windows\System32\timeout.exe /t 2 >nul 2>&1
    curl -s http://localhost:8080/api/cameras 2>nul | findstr /i "camera01" >nul && set CONN=1
  )
)
if "%CONN%"=="1" ( echo [OK]    camera01 registered / online ) else (
  echo [ERROR] camera01 not registered - see %LOGS%\feed.log and %LOGS%\video-server.log
  goto :fail
)

REM ---- (4) verify the image path: pull one frame from the RTSP stream ----
echo [*] Verifying decodable image (RTSP pull of 1 frame) ...
ffmpeg -hide_banner -loglevel error -rtsp_transport tcp -i rtsp://127.0.0.1:8554/camera01 -frames:v 1 -y "%LOGS%\snapshot.jpg"
if exist "%LOGS%\snapshot.jpg" (
  for %%F in ("%LOGS%\snapshot.jpg") do echo [OK]    snapshot captured: %%~zF bytes -> %LOGS%\snapshot.jpg
) else (
  echo [WARN]  snapshot not captured (stream may not be up yet)
)

REM ---- print access info ----
echo.
echo ============================================================
echo  Demo is running
echo  Web UI      : http://localhost:8080
echo  REST API    : http://localhost:8080/api/cameras
echo  RTSP stream : rtsp://127.0.0.1:8554/camera01
echo  Snapshot    : %LOGS%\snapshot.jpg  (proof the image path decodes)
echo  Logs        : %LOGS%\
echo  Stop        : run this script again, or close the windows
echo ============================================================
echo.
goto :done

:add_path
set "_cand=%~1"
if exist "%_cand%" (
  echo [AUTO]   added to PATH: %_cand%
  set "PATH=%_cand%;%PATH%"
)
exit /b 0

:fail
echo.
echo [ABORTED] A critical step failed - fix the error above, then run again.
echo.
pause
exit /b 1

:done
exit /b 0
