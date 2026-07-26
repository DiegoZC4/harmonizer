#define MyAppName "Harmonizer"
#define MyAppVersion GetEnv("HARMONIZER_VERSION")
#define MySourceDir GetEnv("HARMONIZER_DIST_DIR")
#define MyOutputDir GetEnv("HARMONIZER_OUTPUT_DIR")

[Setup]
AppId={{E450BE20-DAB4-4A16-9A98-596439F6A7D5}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=DiegoZC4
AppPublisherURL=https://github.com/DiegoZC4/harmonizer
DefaultDirName={localappdata}\Programs\Harmonizer
DefaultGroupName=Harmonizer
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#MyOutputDir}
OutputBaseFilename=Harmonizer-Windows-x64-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\harmonizer_web.exe
LicenseFile={#MySourceDir}\LICENSE

[Files]
Source: "{#MySourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Harmonizer"; Filename: "{app}\Harmonizer.cmd"; WorkingDir: "{app}"
Name: "{autodesktop}\Harmonizer"; Filename: "{app}\Harmonizer.cmd"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Run]
Filename: "{app}\Harmonizer.cmd"; Description: "Launch Harmonizer"; Flags: nowait postinstall skipifsilent
