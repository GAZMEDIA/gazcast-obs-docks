; Windows installer for the GazCast docks OBS plugin (Inno Setup 6).
;
; CI builds it after packaging the plugin:
;   iscc /DAppVersion=<version> /DSourceDir=<release\RelWithDebInfo\gazcast-docks> ^
;        /DOutputDir=<release> installer\gazcast-docks.iss
;
; It copies the plugin to C:\ProgramData\obs-studio\plugins\gazcast-docks,
; the folder OBS 30+ loads third-party plugins from, and registers an
; uninstaller under Settings > Apps.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\release\RelWithDebInfo\gazcast-docks"
#endif
#ifndef OutputDir
  #define OutputDir "..\release"
#endif

[Setup]
AppId={{6C1E3E0B-4B7A-4E55-9A51-2F0D7C9B5A31}
AppName=GazCast docks for OBS Studio
AppVersion={#AppVersion}
AppVerName=GazCast docks for OBS Studio {#AppVersion}
AppPublisher=GazCast
AppPublisherURL=https://gazgoc.com.ng/gazcast/
AppSupportURL=https://github.com/GAZMEDIA/gazcast-obs-docks/issues
DefaultDirName={commonappdata}\obs-studio\plugins\gazcast-docks
UsePreviousAppDir=no
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallFilesDir={app}\uninstall
UninstallDisplayName=GazCast docks for OBS Studio
UninstallDisplayIcon={app}\uninstall\gazcast.ico
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile=gazcast.ico
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename=gazcast-docks-{#AppVersion}-windows-x64-setup

[Messages]
WelcomeLabel2=This adds the GazCast docks to OBS Studio, so you can play, stop and edit your GazCast overlays without leaving OBS.%n%nPlease close OBS Studio before you continue.
FinishedHeadingLabel=GazCast docks are installed
FinishedLabelNoIcons=Open OBS Studio, click Docks at the top of the window, and tick GazCast and GazCast Control.%n%nThe docks need the GazCast desktop app running: https://gazgoc.com.ng/gazcast/

[InstallDelete]
; drop files left by an older version
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: recursesubdirs ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "gazcast.ico"; DestDir: "{app}\uninstall"; Flags: ignoreversion

[Run]
Filename: "{code:ObsExe}"; WorkingDir: "{code:ObsBinDir}"; Description: "Open OBS Studio now"; Check: ObsFound; Flags: postinstall nowait skipifsilent

[Code]
function ObsInstallDir: String;
begin
  if not RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', Result) then
    Result := ExpandConstant('{commonpf64}\obs-studio');
end;

function ObsBinDir(Param: String): String;
begin
  Result := AddBackslash(ObsInstallDir) + 'bin\64bit';
end;

function ObsExe(Param: String): String;
begin
  Result := ObsBinDir('') + '\obs64.exe';
end;

function ObsFound: Boolean;
begin
  Result := FileExists(ObsExe(''));
end;

function ObsRunning: Boolean;
var
  Locator, Service, Procs: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Procs := Service.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''obs64.exe''');
    Result := Procs.Count > 0;
  except
    Result := CheckForMutexes('OBSStudioCore');
  end;
end;

function InitializeSetup: Boolean;
begin
  Result := True;
  if not ObsFound then
    if SuppressibleMsgBox('OBS Studio was not found on this computer.' + #13#10 + #13#10 +
        'Install OBS Studio 30 or newer from https://obsproject.com first.' + #13#10 + #13#10 +
        'Install the GazCast docks anyway?', mbConfirmation, MB_YESNO, IDYES) = IDNO then
    begin
      Result := False;
      exit;
    end;
  while ObsRunning do
    if SuppressibleMsgBox('OBS Studio is open. Please close OBS Studio, then click OK.',
        mbInformation, MB_OKCANCEL, IDOK) = IDCANCEL then
    begin
      Result := False;
      exit;
    end;
end;

function InitializeUninstall: Boolean;
begin
  Result := True;
  while ObsRunning do
    if SuppressibleMsgBox('OBS Studio is open. Please close OBS Studio, then click OK.',
        mbInformation, MB_OKCANCEL, IDOK) = IDCANCEL then
    begin
      Result := False;
      exit;
    end;
end;
