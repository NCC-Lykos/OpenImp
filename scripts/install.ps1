# Installs the OpenImp badge sign-in tile. Run from an administrator PowerShell
# after building the solution:  .\scripts\install.ps1 [-Configuration Debug]
#
# The normal Windows password tile is left in place, so password sign-in keeps
# working if anything goes wrong. scripts\uninstall.ps1 removes everything.

#Requires -RunAsAdministrator
param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$clsid = '{e241fbb7-0ad7-461d-b4c4-4a0a374d5f14}'
$buildDir = Join-Path $PSScriptRoot "..\build\$Configuration"
$system32 = Join-Path $env:SystemRoot 'System32'
$toolDir = Join-Path $env:ProgramFiles 'OpenImp'

foreach ($file in 'OpenImpAuth.dll', 'pcProxAPI.dll', 'openimp.exe', 'OpenImpManager.exe') {
    if (-not (Test-Path (Join-Path $buildDir $file))) {
        throw "$file not found in $buildDir. Build the $Configuration configuration first."
    }
}

# LogonUI keeps the DLLs loaded, so an in-use copy can't be overwritten. It can
# be renamed, though; the old copy is unloaded at the next sign-out or reboot.
function Copy-InUse($source, $destDir) {
    $dest = Join-Path $destDir (Split-Path $source -Leaf)
    if (Test-Path $dest) {
        $old = "$dest.old"
        Remove-Item $old -Force -ErrorAction SilentlyContinue
        if (Test-Path $old) { $old = "$dest.$(Get-Date -Format yyyyMMddHHmmss).old" }
        Rename-Item $dest (Split-Path $old -Leaf)
    }
    Copy-Item $source $dest
}

Copy-InUse (Join-Path $buildDir 'OpenImpAuth.dll') $system32
Copy-InUse (Join-Path $buildDir 'pcProxAPI.dll') $system32

New-Item $toolDir -ItemType Directory -Force | Out-Null
foreach ($file in 'openimp.exe', 'OpenImpManager.exe', 'OpenImpAuth.dll', 'pcProxAPI.dll') {
    Copy-Item (Join-Path $buildDir $file) $toolDir -Force
}

$shortcut = (New-Object -ComObject WScript.Shell).CreateShortcut(
    (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\OpenImp Badge Sign-in.lnk'))
$shortcut.TargetPath = Join-Path $toolDir 'OpenImpManager.exe'
$shortcut.Save()

$com = "HKLM:\SOFTWARE\Classes\CLSID\$clsid"
New-Item "$com\InprocServer32" -Force | Out-Null
Set-Item $com -Value 'OpenImpAuth'
Set-Item "$com\InprocServer32" -Value 'OpenImpAuth.dll'
Set-ItemProperty "$com\InprocServer32" -Name ThreadingModel -Value 'Apartment'

$cp = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\$clsid"
New-Item $cp -Force | Out-Null
Set-Item $cp -Value 'OpenImpAuth'

Write-Host "Installed. Enroll your badge with 'OpenImp Badge Sign-in' in the Start menu, or:"
Write-Host "  & '$toolDir\openimp.exe' enroll"
Write-Host "Then lock the PC (Windows+L) and tap your badge."
