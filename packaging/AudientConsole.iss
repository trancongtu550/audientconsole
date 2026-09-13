; Audient Console 0.1.0 - per-user installer (Inno Setup 6.x)
; ---------------------------------------------------------------------------
; Release packaging for the frozen v0.1.0 feature set. Installs ONLY runtime
; files (Release executable + local WebView frontend assets + notices). Source,
; tests, dev artifacts, reverse-engineering files and Debug binaries are never
; packaged. Writable state stays under %LOCALAPPDATA%\Audient Console and is
; preserved across upgrade/uninstall.
;
; Build:  ISCC.exe AudientConsole.iss
; Depends on: build\configure-release\src\app\Release\{audient_console_daily.exe,ui\*}
;             build\configure-release\NOTICE.txt
;             packaging\redist\vc_redist.x64.exe  (gitignored; official MS redist)
; ---------------------------------------------------------------------------

#define AppName "Audient Console"
#define AppVersion "0.1.0"
#define AppPublisher "Audient Console (personal use)"
#define AppExeName "AudientConsole.exe"
#define AppSrc "..\build\configure-release\src\app\Release"
#define AppNotice "..\build\configure-release\NOTICE.txt"
; WebView2 Evergreen client GUID (Microsoft).
#define WebView2ClientGuid "{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"

[Setup]
AppId={{B7E7C2A1-5F3D-4E8B-9C21-0A6D4F2E9B33}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\Audient Console
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\{#AppExeName}
; Per-user install: no admin for normal installation. The only elevated step is
; the Microsoft VC++ Redistributable, and only when it is missing.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
OutputDir=..\dist
OutputBaseFilename=AudientConsole-{#AppVersion}-Setup
SetupIconFile=..\src\app\assets\audient_console.ico
VersionInfoVersion={#AppVersion}.0
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} {#AppVersion} Setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#AppSrc}\audient_console_daily.exe"; DestDir: "{app}"; DestName: "{#AppExeName}"; Flags: ignoreversion
Source: "{#AppSrc}\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#AppNotice}"; DestDir: "{app}"; DestName: "NOTICE.txt"; Flags: ignoreversion
Source: "redist\vc_redist.x64.exe"; Flags: dontcopy

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExeName}"
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; IconFilename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent

[Code]
// Windows SYSTEM/WebView: require the Microsoft Edge WebView2 Runtime
// (Evergreen strategy, option A: clear installer error instead of a white
// window). VC++ 2015-2022 x64 runtime is installed from the official
// redistributable only when missing.
function WebView2Installed: Boolean;
var
  pv: String;
begin
  Result :=
    RegQueryStringValue(HKCU, 'Software\Microsoft\EdgeUpdate\Clients\{#WebView2ClientGuid}', 'pv', pv) or
    RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{#WebView2ClientGuid}', 'pv', pv) or
    RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{#WebView2ClientGuid}', 'pv', pv);
end;

function VCRuntimeX64Installed: Boolean;
var
  v: Cardinal;
begin
  Result :=
    (RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', v) and (v = 1)) or
    (RegQueryDWordValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', v) and (v = 1));
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not WebView2Installed then
  begin
    MsgBox('{#AppName} requires the Microsoft Edge WebView2 Runtime, which was not detected on this PC.'
           + #13#10 + #13#10
           + 'Install it from:' + #13#10
           + 'https://developer.microsoft.com/microsoft-edge/webview2/' + #13#10 + #13#10
           + 'Then run this installer again.', mbCriticalError, MB_OK);
    Result := False;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  rc: Integer;
begin
  Result := '';
  if not VCRuntimeX64Installed then
  begin
    ExtractTemporaryFile('vc_redist.x64.exe');
    if not ShellExec('runas', ExpandConstant('{tmp}\vc_redist.x64.exe'),
                     '/install /quiet /norestart', '', SW_HIDE, ewWaitUntilTerminated, rc) then
    begin
      Result := 'The Microsoft Visual C++ 2015-2022 x64 Redistributable is required. '
                + 'Automatic installation could not start (elevation declined?). '
                + 'Install it from https://aka.ms/vs/17/release/vc_redist.x64.exe and run this installer again.';
    end
    else if (rc <> 0) and (rc <> 1638) and (rc <> 3010) then
    begin
      Result := 'The Visual C++ Redistributable installer failed (exit code ' + IntToStr(rc) + '). '
                + 'Install it from https://aka.ms/vs/17/release/vc_redist.x64.exe and run this installer again.';
    end;
  end;
end;
