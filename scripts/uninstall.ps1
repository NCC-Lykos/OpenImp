# Removes the OpenImp badge sign-in tile. Run from an administrator PowerShell:
#   .\scripts\uninstall.ps1 [-RemoveBadges]
# Enrolled badges are kept unless -RemoveBadges is given.

#Requires -RunAsAdministrator
param([switch]$RemoveBadges)

$clsid = '{e241fbb7-0ad7-461d-b4c4-4a0a374d5f14}'
$system32 = Join-Path $env:SystemRoot 'System32'

# Unregister first: once these keys are gone LogonUI no longer loads the tile.
Remove-Item "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\$clsid" -Recurse -ErrorAction SilentlyContinue
Remove-Item "HKLM:\SOFTWARE\Classes\CLSID\$clsid" -Recurse -ErrorAction SilentlyContinue

foreach ($file in 'OpenImpAuth.dll', 'pcProxAPI.dll') {
    Get-ChildItem $system32 -Filter "$file*" | ForEach-Object {
        # A copy LogonUI still has loaded can't be deleted until sign-out or reboot.
        Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue
        if (Test-Path $_.FullName) { Write-Warning "$($_.Name) is in use; delete it after a reboot." }
    }
}

Remove-Item (Join-Path $env:ProgramFiles 'OpenImp') -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\OpenImp Badge Sign-in.lnk') -ErrorAction SilentlyContinue

if ($RemoveBadges) {
    Remove-Item 'HKLM:\SOFTWARE\OpenImp' -Recurse -ErrorAction SilentlyContinue
}

Write-Host 'OpenImp badge sign-in removed.'
