# OpenImp
![OpenImp sign-in tile](OpenImpAuth.png)

Sign in to Windows by tapping a badge on an Imprivata/RFIDeas reader, without Imprivata.

OpenImp adds a "Badge sign-in" tile to the Windows sign-in and unlock screens. When you tap an
enrolled badge, it signs you in to the account linked to that badge. The normal password tile
stays, so you can always sign in with your password.

It works with local accounts (and Microsoft accounts that still have a password). Tested reader:
Imprivata HDW-IMP-80 (RFIDeas RDR-805x2AxU, USB `0C27:3BFA`) with 26-bit HID Prox badges.

## How it works

- `OpenImpAuth` is a credential provider DLL loaded by the sign-in screen. While the screen is up,
  one background thread polls the reader through the RFIDeas pcProx SDK (`shared/dlls/pcProxAPI.dll`).
- Badges are stored under `HKLM\SOFTWARE\OpenImp\Badges\<badge-id>`: the account name, and its
  password encrypted with machine-scope DPAPI. Only SYSTEM and Administrators can read that key.
- `OpenImpManager.exe` is a small window app: install or uninstall the tile, tap a badge, pick
  a Windows user, type their password, and save. It also lists and removes enrolled badges.
- `openimp.exe` (the `OpenImpConsole` project) does the same from the command line.

Your Windows password is stored (encrypted) so the badge can sign you in. When you change your
password, enroll your badge again. Prox badges are easy to copy, so treat a badge like a key to
the PC.

## Build

Visual Studio 2022 or later with the "Desktop development with C++" workload. Open `OpenImp.sln`
and build `Release|x64`, or from a Developer PowerShell:

```
msbuild OpenImp.sln /p:Configuration=Release /p:Platform=x64
```

Output goes to `build\Release`.

## Installer

`.\scripts\build-installer.ps1 -Version 1.0.0` builds everything and writes
`build\installer\OpenImpSetup-1.0.0.exe` (needs Inno Setup 6: `winget install --id JRSoftware.InnoSetup -e`).

The setup file installs the sign-in tile, OpenImp Manager and `openimp.exe`, adds a Start menu
entry, and offers to open OpenImp Manager to enroll a badge. Uninstalling it from Apps & features
removes everything, including enrolled badges.

- Silent install: `OpenImpSetup-1.0.0.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART`
- Silent uninstall: `"C:\Program Files\OpenImp\unins000.exe" /VERYSILENT`
- Enroll from a script: `"C:\Program Files\OpenImp\openimp.exe" enroll <account>` (prompts for the password)

The binaries aren't code-signed, so SmartScreen may warn on other PCs until they are. Check the
RFIDeas SDK license before redistributing `pcProxAPI.dll`.

## Install and enroll (developer)

Easiest: run `build\Release\OpenImpManager.exe`, click **Install**, tap a badge, choose the
user, type the password and click **Save badge**.

Or from an administrator PowerShell in the repo folder:

```
.\scripts\install.ps1
& "$env:ProgramFiles\OpenImp\openimp.exe" read      # check the reader sees your badge
& "$env:ProgramFiles\OpenImp\openimp.exe" enroll    # tap your badge, then type your password
```

Lock the PC (Windows+L) and tap your badge.

Other commands: `openimp list`, `openimp remove <badge-id>`, `openimp enroll <account>`.

## Uninstall

```
.\scripts\uninstall.ps1            # keeps enrolled badges
.\scripts\uninstall.ps1 -RemoveBadges
```
