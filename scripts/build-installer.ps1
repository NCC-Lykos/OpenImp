# Builds the Release binaries and the OpenImpSetup installer.
#   .\scripts\build-installer.ps1 [-Version 1.0.0]
# Needs Visual Studio with the C++ workload, and Inno Setup 6
# (winget install --id JRSoftware.InnoSetup -e).

param([string]$Version = '1.0.0')

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found. Install Visual Studio with the "Desktop development with C++" workload.' }

& $msbuild (Join-Path $root 'OpenImp.sln') /p:Configuration=Release /p:Platform=x64 /m /v:minimal
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }

$iscc = @(
    (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found. Install it with: winget install --id JRSoftware.InnoSetup -e' }

& $iscc "/DAppVersion=$Version" (Join-Path $root 'installer\OpenImp.iss')
if ($LASTEXITCODE -ne 0) { throw 'Installer build failed.' }

Write-Host "Installer: $(Join-Path $root "build\installer\OpenImpSetup-$Version.exe")"
