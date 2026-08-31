# Build Camera Agent with MSVC (cl.exe) + Ninja.
#
# Why not the "Visual Studio 17 2022" generator: MSBuild.exe on this machine
# crashes with an access violation as soon as it builds a real project, so the
# VS generator cannot produce binaries. The Ninja generator drives cl.exe
# directly and does not touch MSBuild at all.
#
# Why PowerShell and not bash: MSYS2 bash hands a colon-separated PATH to
# MSYS2-built cmake, which forwards it to cmd.exe; cmd.exe then fails to find
# rc.exe / mt.exe and linking dies. Running from PowerShell keeps PATH in
# native Windows form end to end.
#
# Usage:
#   .\scripts\build-msvc.ps1                 # gstreamer backend (default)
#   .\scripts\build-msvc.ps1 -Backend sim    # SIM backend
#   .\scripts\build-msvc.ps1 -Clean          # wipe the build dir first

param(
    [ValidateSet('gstreamer', 'sim', 'auto')]
    [string]$Backend  = 'auto',
    [string]$BuildDir = 'build-msvc',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot

# ---- Locate the Visual Studio installation -------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$VsPath = $null
if (Test-Path $vswhere) {
    $VsPath = & $vswhere -latest -products * `
              -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
              -property installationPath 2>$null | Select-Object -First 1
}
if (-not $VsPath -or -not (Test-Path $VsPath)) { $VsPath = 'D:\Software\vs' }
if (-not (Test-Path $VsPath)) { throw "Visual Studio not found (looked in '$VsPath')." }

# ---- Newest MSVC toolset --------------------------------------------------
$MsvcBase = Join-Path $VsPath 'VC\Tools\MSVC'
if (-not (Test-Path $MsvcBase)) { throw "MSVC toolset not found under '$MsvcBase'." }
$Toolset = Get-ChildItem $MsvcBase -Directory |
           Sort-Object { [version]$_.Name } | Select-Object -Last 1
$Vc = Join-Path $MsvcBase $Toolset.Name
$Cl = Join-Path $Vc 'bin\Hostx64\x64\cl.exe'
if (-not (Test-Path $Cl)) { throw "cl.exe not found at '$Cl'." }

# ---- Newest Windows SDK ---------------------------------------------------
$Kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
if (-not (Test-Path $Kits)) { throw "Windows 10 SDK not found at '$Kits'." }
$Sdk = Get-ChildItem (Join-Path $Kits 'Include') -Directory |
       Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
       Sort-Object { [version]$_.Name } | Select-Object -Last 1
$SdkVer = $Sdk.Name

"MSVC  : $($Toolset.Name)"
"SDK   : $SdkVer"
"VS    : $VsPath"

# ---- Compiler environment (native Windows form) ---------------------------
$env:INCLUDE = @(
    (Join-Path $Vc 'include'),
    "$Kits\Include\$SdkVer\ucrt",
    "$Kits\Include\$SdkVer\um",
    "$Kits\Include\$SdkVer\shared",
    "$Kits\Include\$SdkVer\winrt",
    "$Kits\Include\$SdkVer\cppwinrt"
) -join ';'

$env:LIB = @(
    (Join-Path $Vc 'lib\x64'),
    "$Kits\Lib\$SdkVer\ucrt\x64",
    "$Kits\Lib\$SdkVer\um\x64"
) -join ';'

# CMake/Ninja bundled with VS are native Windows binaries - using them keeps
# PATH untouched by any MSYS layer. rc.exe/mt.exe come from the SDK bin dir.
$VsCMake = Join-Path $VsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:PATH = @(
    (Join-Path $Vc 'bin\Hostx64\x64'),
    "$Kits\bin\$SdkVer\x64",
    "$VsCMake\CMake\bin",
    "$VsCMake\Ninja",
    $env:PATH
) -join ';'

# ---- Configure + build ----------------------------------------------------
$Build = Join-Path $Root $BuildDir
if ($Clean -and (Test-Path $Build)) { Remove-Item -Recurse -Force $Build }

$cmakeArgs = @(
    '-S', $Root,
    '-B', $Build,
    '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_CXX_COMPILER=$Cl",
    "-DCAMERA_AGENT_BACKEND=$Backend"
)

"--- configure ---"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)." }

"--- build ---"
& cmake --build $Build
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }

""
"Build OK: $Build"
"Next:"
"  & '$Build\camera-agent.exe' --list"
"  & ctest --test-dir '$Build' --output-on-failure"
