; Inno Setup script for the Windows VST3 installer.
; iscc /DAppVersion=1.2.0 /DSourceDir=<folder containing "Flayr Tune.vst3"> FlayrTune.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\FlayrTune_artefacts\Release\VST3"
#endif

[Setup]
AppId={{6B1C3E2A-7F4D-4E58-9A51-2C8D3F0E9B47}
AppName=Flayr Tune
AppVersion={#AppVersion}
AppPublisher=Flayr Labs
AppPublisherURL=https://github.com/FlayrLabs/flayr-tune
AppSupportURL=https://github.com/FlayrLabs/flayr-tune/issues
DefaultDirName={commoncf64}\VST3
DisableDirPage=yes
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir=..\..\dist
OutputBaseFilename=FlayrTune-{#AppVersion}-Windows-Setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=Flayr Tune (VST3)

[Files]
Source: "{#SourceDir}\Flayr Tune.vst3\*"; DestDir: "{commoncf64}\VST3\Flayr Tune.vst3"; Flags: recursesubdirs createallsubdirs ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\VST3\Flayr Tune.vst3"

[Messages]
FinishedLabel=Flayr Tune is installed in Common Files\VST3. Restart your DAW or rescan plug-ins to use it.
