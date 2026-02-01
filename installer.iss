; Inno Setup script for FastPlay
; This script is used by the GitHub Actions workflow to create an installer

#define MyAppName "FastPlay"
#define MyAppPublisher "Mew"
#define MyAppURL "https://github.com/masonasons/FastPlay"
#define MyAppExeName "FastPlay.exe"

; Version is passed via command line: /DMyAppVersion=x.x.x
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif

; SourceDir is passed via command line: /DSourceDir=path
#ifndef SourceDir
  #define SourceDir "."
#endif

; OutputDir is passed via command line: /DOutputDir=path
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{8A9B1C2D-3E4F-5A6B-7E8F-4A2B3C5D6E7F}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
; Output installer filename
OutputBaseFilename=FastPlayInstaller
OutputDir={#OutputDir}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Require admin rights to install to Program Files
PrivilegesRequired=admin
; Allow installation for current user only as alternative
PrivilegesRequiredOverridesAllowed=dialog
; Architecture
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Uninstaller settings
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "quicklaunchicon"; Description: "{cm:CreateQuickLaunchIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked; OnlyBelowVersion: 6.1; Check: not IsAdminInstallMode

[Files]
; Install main executable
Source: "{#SourceDir}\FastPlay.exe"; DestDir: "{app}"; Flags: ignoreversion
; Install config file if exists
Source: "{#SourceDir}\FastPlay.ini"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; Install lib folder with DLLs
Source: "{#SourceDir}\lib\*"; DestDir: "{app}\lib"; Flags: ignoreversion recursesubdirs createallsubdirs
; Create installed marker file
Source: "{#SourceDir}\FastPlay.exe"; DestDir: "{app}"; AfterInstall: CreateInstalledMarker; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{userappdata}\Microsoft\Internet Explorer\Quick Launch\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: quicklaunchicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{app}\installed.txt"

[Code]
// Create installed marker file after installation
procedure CreateInstalledMarker();
var
  MarkerFile: String;
begin
  MarkerFile := ExpandConstant('{app}\installed.txt');
  SaveStringToFile(MarkerFile, 'Installed via setup', False);
end;

// Check if the app is running before uninstall/upgrade
function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  // Try to find if FastPlay is running
  if Exec('tasklist', '/FI "IMAGENAME eq FastPlay.exe" /NH', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    // The app might be running - warn user
    if MsgBox('FastPlay may be running. Please close it before continuing.' + #13#10 + #13#10 + 'Continue anyway?', mbConfirmation, MB_YESNO) = IDNO then
    begin
      Result := False;
    end;
  end;
end;

// Check if app is running before install/upgrade
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  // Try to find if FastPlay is running
  if Exec('tasklist', '/FI "IMAGENAME eq FastPlay.exe" /NH', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    // The app might be running - warn user
    if MsgBox('FastPlay may be running. Please close it before continuing.' + #13#10 + #13#10 + 'Continue anyway?', mbConfirmation, MB_YESNO) = IDNO then
    begin
      Result := False;
    end;
  end;
end;
