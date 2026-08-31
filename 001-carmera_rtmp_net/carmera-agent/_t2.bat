@echo off
setlocal EnableExtensions EnableDelayedExpansion
set VSWHERE="%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set VSPATH=
if exist %VSWHERE% (
  echo EXISTS
) else (
  echo NOTEXIST
)
echo VSWHERE=%VSWHERE%
