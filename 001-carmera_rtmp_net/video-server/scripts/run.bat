@echo off
setlocal
cd /d "%~dp0\.."
video-server.exe config/config.yaml
endlocal
