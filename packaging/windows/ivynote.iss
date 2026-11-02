; Inno Setup script for the Windows installer.
;
;   iscc /DAppVersion=1.2.3 /DAppVersionNumeric=1.2.3 /DSourceDir=<staged app dir>
;        /DOutputDir=<dir> /DZhIsl=<ChineseSimplified.isl> packaging\windows\ivynote.iss
;
; Installs per user by default (no admin prompt); the wizard offers
; "install for all users" for people who want it.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppVersionNumeric
  #define AppVersionNumeric "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\stage"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\build"
#endif

[Setup]
; Never change AppId: upgrades and uninstall find the old install by it.
AppId={{6F3B7E2A-9C41-4D8E-B5A2-1E7C0D9F4A63}
AppName=Ivynote
AppVerName=Ivynote {#AppVersion}
AppVersion={#AppVersion}
VersionInfoVersion={#AppVersionNumeric}
AppPublisher=Ivynote
AppPublisherURL=https://github.com/ivynote/ivynote-qt
AppSupportURL=https://github.com/ivynote/ivynote-qt/issues
AppUpdatesURL=https://github.com/ivynote/ivynote-qt/releases
DefaultDirName={autopf}\Ivynote
DefaultGroupName=Ivynote
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=Ivynote-{#AppVersion}-windows-x64-setup
SetupIconFile=..\..\resources\icons\app.ico
UninstallDisplayIcon={app}\ivynote.exe
UninstallDisplayName=Ivynote
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Upgrading while the app runs: ask to close it, reopen afterwards.
CloseApplications=yes
RestartApplications=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
#ifdef ZhIsl
Name: "zh"; MessagesFile: "{#ZhIsl}"
#endif

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Ivynote"; Filename: "{app}\ivynote.exe"
Name: "{autodesktop}\Ivynote"; Filename: "{app}\ivynote.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\ivynote.exe"; Description: "{cm:LaunchProgram,Ivynote}"; Flags: nowait postinstall skipifsilent
