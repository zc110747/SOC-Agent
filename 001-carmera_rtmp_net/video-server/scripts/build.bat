@echo off
setlocal
cd /d "%~dp0\.."

:: Locate Go: prefer PATH, fall back to the bundled local toolchain (.toolchain/go).
set GO_BIN=go
where go >nul 2>nul
if %errorlevel% neq 0 (
  if exist ".toolchain\go\go\bin\go.exe" (
    set "GO_BIN=%CD%\.toolchain\go\go\bin\go.exe"
    echo using bundled go: %GO_BIN%
  ) else (
    echo ERROR: go not found on PATH and .toolchain\go\go\bin\go.exe missing.
    echo Install Go (https://go.dev/dl) or run scripts\setup-go.bat to fetch it locally.
    exit /b 1
  )
)

echo [1/3] installing web dependencies
cd web
call npm install --no-audit --no-fund
if %errorlevel% neq 0 exit /b %errorlevel%

echo [2/3] building web ui
call npm run build
if %errorlevel% neq 0 exit /b %errorlevel%
cd ..

echo [3/3] building video-server
"%GO_BIN%" build -trimpath -o video-server.exe ./cmd/video-server
if %errorlevel% neq 0 exit /b %errorlevel%

echo build complete: video-server.exe
endlocal
