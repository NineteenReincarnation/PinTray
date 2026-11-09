#define AppVersion "0.6.5"
#define AppName "PinTray"
#define AppExe "PinTray.exe"

[Setup]
AppId={{48ACB672-BA3A-4825-B303-43AB5479A095}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=NineteenReincarnation
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
WizardStyle=modern
DisableProgramGroupPage=yes
OutputDir=..\artifacts\installer
OutputBaseFilename=PinTray-Setup-{#AppVersion}-win-x64
Compression=lzma2
SolidCompression=yes
UninstallDisplayIcon={app}\{#AppExe}
CloseApplications=yes

[Files]
Source: "..\artifacts\publish\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\PinTray"; Filename: "{app}\{#AppExe}"

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch PinTray"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\reg.exe"; Parameters: "delete ""HKCU\Software\Microsoft\Windows\CurrentVersion\Run"" /v PinTray /f"; Flags: runhidden; RunOnceId: "PinTrayRemoveAutostart"
