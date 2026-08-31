@echo off
setlocal EnableExtensions EnableDelayedExpansion
REM One-click launcher for the Camera Agent demo.
REM
REM External tools (mediamtx, ffplay, ffmpeg) are expected on PATH.
REM Only the project tool (camera-agent.exe) is referenced by relative path,
REM so this script is portable across machines.
REM All runtime logs are written under tests\finished\.
REM
REM Flow: (1) pre-check tools + binary
REM       (2) stop running related programs
REM       (3) start MediaMTX -> camera-agent -> ffplay in order
REM       (4) verify each component and print startup info / access method
REM
REM Note: if a tool is "not found", it means it is NOT on the PATH that this
REM script inherits. Double-clicking a .bat uses the system/user PATH from the
REM registry, NOT the PATH of an already-open terminal. Add the tools to the
REM system/user environment (or run from a shell where they are on PATH).

set ROOT=%~dp0
set FINISHED=%ROOT%tests\finished
if not exist "%FINISHED%" mkdir "%FINISHED%"

REM Run from the project root so mediamtx finds mediamtx.yml and
REM camera-agent finds config/camera-agent.yaml regardless of where the
REM .bat is launched from.
cd /d "%ROOT%"

REM ---- parse args ----
set NOPAUSE=0
for %%a in (%*) do (
  if /i "%%a"=="--no-pause" set NOPAUSE=1
)

REM ---- (1) pre-check: project binary ----
set EXE=%ROOT%build-msvc\src\camera-agent.exe
if not exist "%EXE%" (
  echo [ERROR] camera-agent.exe not found at:
  echo         %EXE%
  echo         Build it first:  build_oneclick.bat   (or: build_oneclick.bat sim)
  goto :fail
)
echo [OK]    camera-agent.exe

REM ---- (1) pre-check: external tools on PATH ----
call :need mediamtx || goto :fail
call :need ffplay     || goto :fail

REM ---- (2) stop running related programs ----
echo [1/3] Stopping running programs: mediamtx / camera-agent / ffplay
taskkill /F /IM mediamtx.exe >nul 2>&1
taskkill /F /IM camera-agent.exe >nul 2>&1
taskkill /F /IM ffplay.exe >nul 2>&1
timeout /t 1 >nul

REM ---- (3) start programs in order ----
echo [2/3] Starting MediaMTX (RTSP server) ...
start "MediaMTX" cmd /c "mediamtx > "%FINISHED%\mediamtx.log" 2>&1"

REM wait until mediamtx is ready (poll its log)
set MT_READY=0
for /l %%i in (1,1,15) do (
  timeout /t 1 >nul
  findstr /i /c:"listener" /c:"8554" "%FINISHED%\mediamtx.log" >nul 2>&1 && ( set MT_READY=1 & goto :mt_ok )
)
:mt_ok
if "%MT_READY%"=="1" ( echo [OK]    mediamtx ready ) else ( echo [WARN]  mediamtx not ready - see tests\finished\mediamtx.log )

echo [2/3] Starting camera-agent (push stream) ...
start "camera-agent" cmd /c ""%EXE%" --camera 0 --width 240 --height 240 --fps 8 --stream camera01 --log-level info > "%FINISHED%\agent.log" 2>&1"

echo [2/3] Starting ffplay (viewer) ...
start "ffplay" ffplay -rtsp_transport tcp -fflags nobuffer -flags low_delay rtsp://127.0.0.1:8554/camera01

REM ---- (4) verify camera-agent status ----
set ST_OK=0
set ST_ERR=0
for /l %%i in (1,1,15) do (
  timeout /t 1 >nul
  findstr /i /c:"STREAMING" "%FINISHED%\agent.log" >nul 2>&1 && ( set ST_OK=1 & goto :st_done )
  findstr /i /c:"error" /c:"failed" /c:"not found" /c:"not installed" /c:"unable" "%FINISHED%\agent.log" >nul 2>&1 && ( set ST_ERR=1 & goto :st_done )
)
:st_done
if "%ST_OK%"=="1" ( echo [OK]    camera-agent STREAMING ) else (
  if "%ST_ERR%"=="1" (
    echo [WARN]  camera-agent reported an error (no camera / GStreamer issue?):
    echo         --- last lines of agent.log ---
    for /f "tokens=*" %%l in ('findstr /i /c:"error" /c:"failed" /c:"not found" /c:"not installed" /c:"unable" "%FINISHED%\agent.log"') do echo         %%l
  ) else (
    echo [WARN]  camera-agent status unknown - see tests\finished\agent.log
  )
)

REM ---- (4) print startup info and access method ----
echo.
echo ============================================================
echo  Camera Agent demo started
echo  RTSP stream : rtsp://127.0.0.1:8554/camera01
echo  Viewer      : ffplay window opened automatically
echo  Logs        : tests\finished\  (agent.log / mediamtx.log)
echo  Stop        : run this script again, or close the 3 windows
echo ============================================================
echo.
goto :done

:need
set "_t=%~1"
where %_t% >nul 2>&1
if errorlevel 1 (
  echo [ERROR] '%_t%' was not found on PATH.
  echo         This script inherits the system/user PATH from the registry.
  echo         Add '%_t%' to PATH (System Properties - Environment Variables)
  echo         and reopen the terminal, or run from a shell where it is available.
  exit /b 1
)
echo [OK]    %_t%
exit /b 0

:fail
echo.
echo Aborted - fix the error above, then run this script again.
echo.
if "%NOPAUSE%"=="0" pause
exit /b 1

:done
if "%NOPAUSE%"=="0" pause
exit /b 0
