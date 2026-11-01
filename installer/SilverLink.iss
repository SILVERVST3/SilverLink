; SilverLink - Inno Setup script
; Compile: ISCC.exe /DBuildDir=<pasta com os .exe/.dll> installer\SilverLink.iss

#ifndef BuildDir
  #define BuildDir "..\build\Release"
#endif
#define AppVer "1.0.0"

[Setup]
AppId={{6E3B2C71-9D1A-4B7E-8F21-51C0A9D4E6B3}
AppName=SilverLink
AppVersion={#AppVer}
AppPublisher=Jorge Prata Alves Junior
DefaultDirName={autopf}\SilverLink
DefaultGroupName=SilverLink
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=Output
OutputBaseFilename=SilverLink_Setup_{#AppVer}
SetupIconFile=..\res\silverlink.ico
UninstallDisplayIcon={app}\SilverLink_Controller.exe
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
LicenseFile=..\LICENSE
CloseApplications=yes

[Languages]
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "autostart"; Description: "Iniciar o SilverLink Engine com o Windows"; GroupDescription: "Opções:"
Name: "desktopicon"; Description: "Atalho do SilverLink Controller na área de trabalho"; GroupDescription: "Opções:"

[Files]
Source: "{#BuildDir}\SilverLink.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\SilverLink_Controller.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\SilverLink_ASIO.dll"; DestDir: "{app}"; Flags: ignoreversion regserver
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{group}\SilverLink Controller"; Filename: "{app}\SilverLink_Controller.exe"
Name: "{group}\SilverLink Engine"; Filename: "{app}\SilverLink.exe"
Name: "{group}\Desinstalar SilverLink"; Filename: "{uninstallexe}"
Name: "{autodesktop}\SilverLink Controller"; Filename: "{app}\SilverLink_Controller.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "SilverLink Engine"; ValueData: """{app}\SilverLink.exe"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SilverLink Engine"""; Flags: runhidden; StatusMsg: "Configurando o firewall..."
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SilverLink Controller"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""SilverLink Engine"" dir=in action=allow program=""{app}\SilverLink.exe"" protocol=UDP profile=any enable=yes"; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""SilverLink Controller"" dir=in action=allow program=""{app}\SilverLink_Controller.exe"" protocol=UDP profile=any enable=yes"; Flags: runhidden
Filename: "{app}\SilverLink.exe"; Description: "Iniciar o SilverLink Engine agora"; Flags: nowait postinstall runasoriginaluser
Filename: "{app}\SilverLink_Controller.exe"; Description: "Abrir o SilverLink Controller"; Flags: nowait postinstall runasoriginaluser unchecked

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM SilverLink.exe"; Flags: runhidden; RunOnceId: "KillEngine"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM SilverLink_Controller.exe"; Flags: runhidden; RunOnceId: "KillCtl"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SilverLink Engine"""; Flags: runhidden; RunOnceId: "FwEngine"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""SilverLink Controller"""; Flags: runhidden; RunOnceId: "FwCtl"

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  rc: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM SilverLink.exe', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM SilverLink_Controller.exe', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Result := '';
end;
