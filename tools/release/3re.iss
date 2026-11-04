; Inno Setup script for the 3re Windows installer (Inno Setup 6). Installs the staged release folder for the
; current user only (no admin rights), so 3re can update itself in place (docs/UPDATES.md).
;   iscc /DAppVersion=2.10.0-alpha /DStageDir=out\3re /DOutDir=out tools\release\3re.iss
; The installer is not code-signed (an owner decision, see the release notes).

#ifndef AppVersion
  #error Pass /DAppVersion=<version>
#endif
#ifndef StageDir
  #error Pass /DStageDir=<staged release folder>
#endif
#ifndef OutDir
  #define OutDir "."
#endif

[Setup]
AppId={{6E1A3C52-3E5D-4D8B-9C3A-3E0A7B5C2F11}
AppName=3re
AppVersion={#AppVersion}
AppPublisher=The 3re authors
AppPublisherURL=https://github.com/3reusa7/3re
DefaultDirName={localappdata}\Programs\3re
DefaultGroupName=3re
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutDir}
OutputBaseFilename=3re-{#AppVersion}-windows-x64-setup
; Relative paths are relative to this script's folder (tools\release).
SetupIconFile=..\..\apps\gui\resources\3re.ico
UninstallDisplayIcon={app}\3re.exe
LicenseFile={#StageDir}\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{group}\3re"; Filename: "{app}\3re.exe"
Name: "{userdesktop}\3re"; Filename: "{app}\3re.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\3re.exe"; Description: "{cm:LaunchProgram,3re}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; Downloaded models and files added by updates are not in the installer's list.
Type: filesandordirs; Name: "{app}"
