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
; The badge service (tap to lock or switch users). Stopped first so it can be replaced.
Source: "{#BuildDir}\OpenImpService.exe"; DestDir: "{sys}"; Flags: ignoreversion restartreplace uninsrestartdelete; BeforeInstall: StopBadgeService
; Enrollment tools.
Source: "{#BuildDir}\OpenImpManager.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\openimp.exe";        DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\pcProxAPI.dll";      DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\OpenImpAuth.dll";    DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\OpenImpService.exe"; DestDir: "{app}"; Flags: ignoreversion
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
; Register (or update) and start the badge service. "create" fails harmlessly on upgrades; "config" covers them.
Filename: "{sys}\sc.exe"; Parameters: "create OpenImp binPath= ""{sys}\OpenImpService.exe"" start= auto DisplayName= ""OpenImp Badge Service"""; Flags: runhidden
Filename: "{sys}\sc.exe"; Parameters: "config OpenImp binPath= ""{sys}\OpenImpService.exe"" start= auto DisplayName= ""OpenImp Badge Service"""; Flags: runhidden
Filename: "{sys}\sc.exe"; Parameters: "description OpenImp ""Locks the PC or switches users when a badge is tapped while someone is signed in."""; Flags: runhidden
Filename: "{sys}\sc.exe"; Parameters: "start OpenImp"; Flags: runhidden
Filename: "{app}\OpenImpManager.exe"; Description: "Enroll a badge now"; Flags: postinstall nowait skipifsilent runascurrentuser

[UninstallDelete]
; Sign-in screen images that OpenImp Manager copied in.
Type: filesandordirs; Name: "{app}\Branding"

[UninstallRun]
Filename: "{sys}\net.exe"; Parameters: "stop OpenImp"; Flags: runhidden; RunOnceId: "StopBadgeService"
Filename: "{sys}\sc.exe"; Parameters: "delete OpenImp"; Flags: runhidden; RunOnceId: "DeleteBadgeService"

[Code]
// Stops the badge service before OpenImpService.exe is replaced; does nothing if it isn't installed.
procedure StopBadgeService();
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\net.exe'), 'stop OpenImp', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;
