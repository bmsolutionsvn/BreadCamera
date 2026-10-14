#define AppName "BreadCamera"
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build\release"
#endif

[Setup]
AppId={{9C1F2B7A-3E44-4A39-9F0B-6A1D2C3B4E50}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=BM Solutions
AppPublisherURL=https://bmsolutions.vn
DefaultDirName={autopf}\BreadMedia\BreadCamera
DefaultGroupName=BreadCamera
OutputDir=..\dist
OutputBaseFilename=BreadCamera-Setup-{#AppVersion}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0.19041
AppMutex=Local\BreadCameraShell,Global\BreadCameraShell
CloseApplications=yes
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
LicenseFile=THIRD_PARTY_NOTICES.md

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#BuildDir}\BreadCamera.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\breadcamera-engine.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\ui\*"; DestDir: "{app}\ui"; Flags: ignoreversion recursesubdirs
Source: "{#BuildDir}\models\*"; DestDir: "{app}\models"; Flags: ignoreversion recursesubdirs skipifsourcedoesntexist
Source: "{#BuildDir}\vcam\breadcamera-vcam.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#VcRedist}"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "THIRD_PARTY_NOTICES.md"; DestDir: "{app}"
Source: "LGPL-source-offer.txt"; DestDir: "{app}"

[Icons]
Name: "{group}\BreadCamera"; Filename: "{app}\BreadCamera.exe"
Name: "{autodesktop}\BreadCamera"; Filename: "{app}\BreadCamera.exe"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing the Visual C++ runtime..."
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\breadcamera-vcam.dll"""; Check: IsWin11 and FileExists(ExpandConstant('{app}\breadcamera-vcam.dll')); StatusMsg: "Registering the virtual camera..."
Filename: "{app}\BreadCamera.exe"; Description: "Start BreadCamera"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\breadcamera-vcam.dll"""; RunOnceId: "UnregVcam"; Check: FileExists(ExpandConstant('{app}\breadcamera-vcam.dll'))
Filename: "{sys}\taskkill.exe"; Parameters: "/im breadcamera-engine.exe /f"; RunOnceId: "StopEngine"; Flags: runhidden

[Code]
function IsWin11: Boolean;
var V: TWindowsVersion;
begin
  GetWindowsVersionEx(V);
  Result := (V.Major = 10) and (V.Build >= 22000);
end;

function InitializeSetup: Boolean;
var Code: Integer;
begin
  { The engine survives the shell (spec §2); stop it so its files can be replaced. }
  if CheckForMutexes('Local\BreadCameraEngine') then
  begin
    if MsgBox('BreadCamera is running and its outputs are live. Stop it and continue the installation?', mbConfirmation, MB_YESNO) = IDNO then
    begin
      Result := False;
      exit;
    end;
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/im breadcamera-engine.exe /f', '', SW_HIDE, ewWaitUntilTerminated, Code);
  end;
  Result := True;
end;