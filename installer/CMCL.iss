#ifndef StageDir
  #define StageDir "stage"
#endif

#define MyAppName "CMCL"
#define MyAppVersion "0.5.1"
#define MyAppPublisher "CMCL"
#define MyAppExeName "cmcl-ui.exe"

[Setup]
AppId={{A7C2E5D9-3B4F-4C8A-9E21-6F5D8B1C0A73}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\CMCL
DefaultGroupName=CMCL
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\dist
OutputBaseFilename=CMCL-Setup-{#MyAppVersion}
SetupIconFile=..\src\ui\app.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
AllowNoIcons=yes
AppMutex=CMCL_UI_Singleton
CloseApplications=yes
RestartApplications=no
VersionInfoVersion=0.5.1.0
VersionInfoCompany=CMCL
VersionInfoDescription=CMCL - C++ Minecraft Launcher Setup
VersionInfoProductName=CMCL
VersionInfoProductVersion=0.5.1.0
VersionInfoCopyright=CMCL

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#StageDir}\cmcl-ui.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\msvcp140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\msvcp140_atomic_wait.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\vcruntime140.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\CMCL"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\CMCL"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,CMCL}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\logs"
Type: filesandordirs; Name: "{app}\web"
