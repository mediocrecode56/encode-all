; Per-user installer (no admin). build.ps1 passes AppVersion, SourceDir (build\) and RootDir.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\build"
#endif
#ifndef RootDir
  #define RootDir ".."
#endif

[Setup]
AppId={{9C5D1E7A-3B2F-4E6A-9D11-0EA0E0C0DE01}
AppName=Encode All
AppVersion={#AppVersion}
AppVerName=Encode All {#AppVersion}
AppPublisher=Encode All
AppPublisherURL=https://github.com/mediocrecode56/encode-all
DefaultDirName={localappdata}\Programs\Encode All
DisableDirPage=auto
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=EncodeAll-Setup-{#AppVersion}
SetupIconFile={#RootDir}\assets\encodeall.ico
UninstallDisplayIcon={app}\EncodeAll.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
LicenseFile={#RootDir}\LICENSE

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#SourceDir}\EncodeAll.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RootDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#RootDir}\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Encode All"; Filename: "{app}\EncodeAll.exe"
Name: "{autodesktop}\Encode All"; Filename: "{app}\EncodeAll.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\EncodeAll.exe"; Description: "Start Encode All"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\EncodeAll.exe.old"
Type: files; Name: "{app}\EncodeAll.exe.bad"
