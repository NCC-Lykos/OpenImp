; Inno Setup script for OpenImp badge sign-in. Build with scripts\build-installer.ps1,
; which builds the Release binaries first. Output: build\installer\OpenImpSetup-<version>.exe
;
; Silent install:    OpenImpSetup-<version>.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART
; Silent uninstall:  "C:\Program Files\OpenImp\unins000.exe" /VERYSILENT

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#define Clsid "{{e241fbb7-0ad7-461d-b4c4-4a0a374d5f14}"
#define BuildDir "..\build\Release"

[Setup]
AppId={{6F1C7A2E-3B8D-4C55-9E0A-2D7B4F81C3A9}
AppName=OpenImp Badge Sign-in
AppVersion={#AppVersion}
AppPublisher=OpenImp
DefaultDirName={autopf}\OpenImp
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\build\installer
OutputBaseFilename=OpenImpSetup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=OpenImp Badge Sign-in
UninstallDisplayIcon={app}\OpenImpManager.exe
CloseApplications=yes

[Files]
; The sign-in tile. LogonUI keeps it loaded, so a locked copy is replaced at the next reboot.
Source: "{#BuildDir}\OpenImpAuth.dll"; DestDir: "{sys}"; Flags: restartreplace uninsrestartdelete
Source: "{#BuildDir}\pcProxAPI.dll";   DestDir: "{sys}"; Flags: restartreplace uninsrestartdelete
; Enrollment tools.
Source: "{#BuildDir}\OpenImpManager.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\openimp.exe";        DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\pcProxAPI.dll";      DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\OpenImpAuth.dll";    DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md";                   DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Register the credential provider (the badge tile on the sign-in screen).
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{#Clsid}"; ValueType: string; ValueData: "OpenImpAuth"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{#Clsid}\InprocServer32"; ValueType: string; ValueData: "OpenImpAuth.dll"
Root: HKLM; Subkey: "SOFTWARE\Classes\CLSID\{#Clsid}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers\{#Clsid}"; ValueType: string; ValueData: "OpenImpAuth"; Flags: uninsdeletekey
; Enrolled badges hold encrypted passwords; remove them with the program.
Root: HKLM; Subkey: "SOFTWARE\OpenImp"; Flags: uninsdeletekey

[Icons]
Name: "{autoprograms}\OpenImp Badge Sign-in"; Filename: "{app}\OpenImpManager.exe"

[Run]
Filename: "{app}\OpenImpManager.exe"; Description: "Enroll a badge now"; Flags: postinstall nowait skipifsilent runascurrentuser
