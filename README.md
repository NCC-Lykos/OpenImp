# OpenImp
![OpenImp sign-in tile](OpenImpAuth.png)

Sign in to Windows by tapping a badge on an Imprivata/RFIDeas reader, without Imprivata.

OpenImp adds a "Badge sign-in" tile to the Windows sign-in and unlock screens. When you tap an
enrolled badge, it signs you in to the account linked to that badge. The normal password tile
stays, so you can always sign in with your password.

While you're signed in, tap your badge again to lock the PC. If someone else taps their enrolled
badge, Windows switches to their account; your session keeps running in the background (like
"Switch user"), and your badge switches back. A different user's badge on the lock screen switches
users too. Badges that aren't enrolled are ignored. Switching needs Fast User Switching, which is
on unless a `HideFastUserSwitching` policy turns it off; without it, the tap just locks the PC.

## Branded sign-in screen

OpenImp Manager > **Sign-in screen...** turns on a full-screen branded screen over the Windows
sign-in and lock screens: your background image, logo, headline, message and colors, a "tap your
badge" status line, and username/password boxes. The background goes on the main monitor; any
other monitors are black. While it's on, Windows' own lock screen (the clock picture) is turned
off with the "Do not display the lock screen" policy (`NoLockScreen`), so locking the PC goes
straight to the branded screen; turning the branded screen off or uninstalling puts it back, unless
the policy was already set before OpenImp. **Cancel and use Windows sign-in** (or Esc)
closes it and shows the normal Windows screen until the next lock or sign-out. **Preview** shows
it on your desktop first (Esc closes the preview).

Settings live in `HKLM\SOFTWARE\OpenImp\Branding`, and the images are copied into
`%ProgramFiles%\OpenImp\Branding`; only administrators can change either. The screen is drawn by
the sign-in tile itself, on top of LogonUI. Windows doesn't officially support that, so a Windows
update could change how it behaves. If it ever gets in the way, press Esc. In Safe Mode it never
appears, and setting `Enabled` to 0 under the Branding key turns it off.

It works with local accounts (and Microsoft accounts that still have a password). Tested reader:
Imprivata HDW-IMP-80 (RFIDeas RDR-805x2AxU, USB `0C27:3BFA`) with 26-bit HID Prox badges.

## How it works

- `OpenImpAuth` is a credential provider DLL loaded by the sign-in screen. While the screen is up,
  one background thread polls the reader through the RFIDeas pcProx SDK (`shared/dlls/pcProxAPI.dll`).
- `OpenImpService.exe` is a Windows service ("OpenImp Badge Service") that polls the reader the rest
  of the time: only while someone is signed in at the console and the PC is unlocked. Your own
  badge locks the session. Another user's badge disconnects it and passes the badge to the sign-in
  tile through `HKLM\SOFTWARE\OpenImp\Handoff`, which is SYSTEM/Administrators-only and expires
  after 15 seconds; the tile then signs that user in. While OpenImp Manager or `openimp enroll` is
  open, the service leaves the reader alone, so enrolling a badge doesn't lock the PC.
- Both write a diagnostic log (taps, sign-in steps, lock/switch decisions; never passwords) to
  `C:\Windows\System32\LogFiles\OpenImp\openimp.log`. Any user can read it; only SYSTEM and
  Administrators can write to it.
- Badges are stored under `HKLM\SOFTWARE\OpenImp\Badges\<badge-id>`: the account name, and its
  password encrypted with machine-scope DPAPI. Only SYSTEM and Administrators can read that key.
- `OpenImpManager.exe` is a small window app: install or uninstall the tile, tap a badge, pick
  a Windows user, type their password, and save. It also lists and removes enrolled badges.
- `openimp.exe` (the `OpenImpConsole` project) does the same from the command line.

### Code layout

| Folder | What's in it |
| --- | --- |
| `OpenImpAuth` | The credential provider: `COpenImpProvider` (reader thread, handoff, branded screen) and `COpenImpCredential` (the tile and its sign-in buffer). `helpers.cpp` packs the logon buffer (from Microsoft's sample). |
| `OpenImpService` | The badge service. |
| `OpenImpManager` | OpenImp Manager. |
| `OpenImpConsole` | `openimp.exe`. |
| `shared/src` | Code the projects share: `BadgeReader` (reader, tap filter, reader pause), `BadgeStore` (enrolled badges, handoff), `Session`, `Account` (account names, password check), `Branding` and `Overlay` (branded screen), `Registry`, `Log`, `Util`. |

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

The setup file installs the sign-in tile, the badge service, OpenImp Manager and `openimp.exe`, adds a Start menu
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

Lock the PC (Windows+L) and tap your badge. Once you're signed in, tap it again to lock.

Other commands: `openimp list`, `openimp remove <badge-id>`, `openimp enroll <account>`.

## Uninstall

```
.\scripts\uninstall.ps1            # keeps enrolled badges
.\scripts\uninstall.ps1 -RemoveBadges
```
