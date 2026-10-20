; RefMatch installer (Inno Setup 6)
; Build everything with:  scripts\build-installer.bat   -> dist\RefMatch-Setup-<version>.exe
; (compiles a static-runtime Release build in build-release, copies it to installer\payload, then runs ISCC on this file)
; Payload: installer\payload\{VST3\RefMatch.vst3, CLAP\RefMatch.clap, Standalone\RefMatch.exe}, built with the static
;          MSVC runtime (-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded), so no VC++ redistributable is needed.
; Testing: ISCC /DTestRoot=<folder> installer\RefMatch.iss builds a per-user variant that installs everything below
;          <folder> instead of Program Files (no admin rights, never touches a real installation).

#define AppName "RefMatch"
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#define AppPublisher "Kujar3"

#ifdef TestRoot
  #define PluginRoot TestRoot + "\CommonFiles"
  #define AppRoot TestRoot + "\App"
#else
  #define PluginRoot "{commoncf64}"
  #define AppRoot "{autopf}\" + AppName
#endif

[Setup]
#ifdef TestRoot
; Own AppId, never the previous install's folder: a test install can never take over or move the real one.
AppId={{0C2E7A55-3B1F-4D7E-9A60-5E2B8F4C1D93}
UsePreviousAppDir=no
#else
AppId={{5DFF9D10-7669-46F8-B581-741A88E22341}
#endif
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={#AppRoot}
DisableDirPage=yes
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
#ifdef TestRoot
PrivilegesRequired=lowest
OutputBaseFilename=RefMatch-Setup-{#AppVersion}-TEST
#else
PrivilegesRequired=admin
OutputBaseFilename=RefMatch-Setup-{#AppVersion}
#endif
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
SetupIconFile=refmatch.ico
UninstallDisplayIcon={app}\refmatch.ico
UninstallDisplayName={#AppName} {#AppVersion}
InfoAfterFile=QuickStart.txt
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
CloseApplications=yes
; The default filter (*.exe,*.dll,*.chm) would not notice a DAW that has the plugin binaries loaded.
CloseApplicationsFilter=*.exe,*.dll,*.chm,*.vst3,*.clap

[Messages]
WelcomeLabel2=This will install [name/ver] - a reference-track matcher for FL Studio and other DAWs: compare your mix with a song you love, at the same loudness, and see what to change.%n%nPlease close FL Studio before continuing.

[Types]
Name: "full"; Description: "Recommended (VST3 + CLAP)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "vst3"; Description: "VST3 plugin (use this one in FL Studio)"; Types: full custom; Flags: fixed
Name: "clap"; Description: "CLAP plugin"; Types: full
Name: "standalone"; Description: "Standalone app (listen to and inspect reference tracks without a DAW)"

[Files]
Source: "payload\VST3\RefMatch.vst3\*"; DestDir: "{#PluginRoot}\VST3\RefMatch.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "payload\CLAP\RefMatch.clap"; DestDir: "{#PluginRoot}\CLAP"; Components: clap; Flags: ignoreversion
Source: "payload\Standalone\RefMatch.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
Source: "QuickStart.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "refmatch.ico"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; A clean bundle every time (an older build may have had other files inside it).
Type: filesandordirs; Name: "{#PluginRoot}\VST3\RefMatch.vst3"

[UninstallDelete]
Type: dirifempty; Name: "{#PluginRoot}\VST3\RefMatch.vst3"

[Icons]
Name: "{group}\RefMatch"; Filename: "{app}\RefMatch.exe"; Components: standalone
Name: "{group}\RefMatch Quick Start"; Filename: "{app}\QuickStart.txt"
Name: "{group}\Uninstall RefMatch"; Filename: "{uninstallexe}"
