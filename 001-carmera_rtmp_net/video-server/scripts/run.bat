@echo off
setlocal
cd /d "%~dp0\.."

REM Prefer the canonical binary; fall back to the name build.bat uses when
REM video-server.exe is locked by a still-running instance.
set "VS_BIN=video-server.exe"
if not exist "%VS_BIN%" (
  if exist "video-server.new.exe" (
    set "VS_BIN=video-server.new.exe"
  ) else (
    echo ERROR: neither video-server.exe nor video-server.new.exe exists.
    echo        Build it first: scripts\build.bat
    exit /b 1
  )
)

REM Optional overrides:
REM   run.bat                      -> config\config.yaml, bind from the config
REM   run.bat config\config.joint.yaml
"%VS_BIN%" %*
endlocal
