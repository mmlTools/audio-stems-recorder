; Inno Setup script for the Windows installer.
; Built by .github/scripts/Package-Windows.ps1, which passes:
;   /DProductName /DDisplayName /DProductVersion /DAuthor /DWebsite /DSourceDir /DOutputDir /DOutputName
;
; Installs into the OBS Studio 33+ plugin location:
;   C:\ProgramData\obs-studio\plugins\<ProductName>\<ProductName>.dll
;   C:\ProgramData\obs-studio\plugins\<ProductName>\data\...

[Setup]
AppId={{6F1B0C52-3C1E-4C8E-9C0B-6E2E8B6A9D41}
AppName={#DisplayName} for OBS Studio
AppVersion={#ProductVersion}
AppVerName={#DisplayName} {#ProductVersion}
AppPublisher={#Author}
AppPublisherURL={#Website}
DefaultDirName={commonappdata}\obs-studio\plugins\{#ProductName}
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=no
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Keep the uninstaller out of the plugin folder OBS scans.
UninstallFilesDir={commonappdata}\obs-studio\plugin-uninstallers\{#ProductName}
UninstallDisplayName={#DisplayName} for OBS Studio
OutputDir={#OutputDir}
OutputBaseFilename={#OutputName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Messages]
WelcomeLabel2=This will install [name/ver] into the OBS Studio 33+ plugin folder:%n%n  C:\ProgramData\obs-studio\plugins\{#ProductName}%n%nPlease close OBS Studio before continuing.

[InstallDelete]
; Legacy layouts written by 1.x builds (deprecated in OBS 33, ignored from OBS 34).
Type: filesandordirs; Name: "{app}\bin"
Type: files; Name: "{commonpf64}\obs-studio\obs-plugins\64bit\{#ProductName}.dll"
Type: files; Name: "{commonpf64}\obs-studio\obs-plugins\64bit\{#ProductName}.pdb"
Type: filesandordirs; Name: "{commonpf64}\obs-studio\data\obs-plugins\{#ProductName}"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[UninstallDelete]
Type: dirifempty; Name: "{app}"
