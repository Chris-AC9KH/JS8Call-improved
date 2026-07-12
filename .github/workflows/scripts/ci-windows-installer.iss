#define MyAppName "JS8Call"
#ifndef MyAppVersion
  #define MyAppVersion "0.0.0"
#endif
#pragma message "AppVersion is: " + MyAppVersion
#define MyAppPublisher "JS8Call-improved"
#define MyAppURL "https://www.js8call.com/"
#define MyAppExeName "JS8Call.exe"

[Setup]
AppId={{B5281957-28FD-4BAE-8D06-FC59898D850E}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
LicenseFile=D:\a\JS8Call-improved\JS8Call-improved\build\JS8Call\LICENSE
DefaultDirName={autopf}\{#MyAppName}
UsePreviousAppDir=no
; UninstallDisplayIcon={app}\{#MyAppExeName}
; "ArchitecturesAllowed=x64compatible" specifies that Setup cannot run
; on anything but x64 and Windows 11 on Arm.
ArchitecturesAllowed=x64compatible
; "ArchitecturesInstallIn64BitMode=x64compatible" requests that the
; install be done in "64-bit mode" on x64 or Windows 11 on Arm,
; meaning it should use the native 64-bit Program Files directory and
; the 64-bit view of the registry.
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
; Uncomment the following line to run in non administrative install mode (install for current user only).
;PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=D:\a\JS8Call-improved\JS8Call-improved\build\JS8Call
; This can be changed from the ci-windows.yml for release builds
OutputBaseFilename=JS8Call-installer
SetupIconFile=D:\a\JS8Call-improved\JS8Call-improved\icons\windows-icons\js8call.ico
UninstallDisplayIcon=D:\a\JS8Call-improved\JS8Call-improved\icons\windows-icons\js8call.ico
SolidCompression=yes
; WizardStyle introduced in 6.6.0, Github has 6.5.4
; WizardStyle=modern dynamic

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "D:\a\JS8Call-improved\JS8Call-improved\build\JS8Call\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "D:\a\JS8Call-improved\JS8Call-improved\build\JS8Call\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "D:\a\JS8Call-improved\JS8Call-improved\icons\windows-icons\js8call.ico"; DestDir: "{app}"; Flags: ignoreversion
; NOTE: Don't use "Flags: ignoreversion" on any shared system files

[Icons]
Name: "{autoprograms}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\js8call.ico"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\js8call.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

