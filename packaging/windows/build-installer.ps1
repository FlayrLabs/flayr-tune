# Builds dist\FlayrTune-<version>-Windows-Setup.exe from build\FlayrTune_artefacts\Release\VST3
# with a pinned Inno Setup, never whatever the machine has: the official 6.7.1 installer,
# checked against its SHA-256 (the same bytes Chocolatey's innosetup 6.7.1 package embeds),
# installed into a private directory. Used by release.yml, and by CI to keep it working.
#   pwsh packaging/windows/build-installer.ps1 -Version 1.3.0
param([Parameter(Mandatory = $true)][string] $Version)
$ErrorActionPreference = 'Stop'

$tmp = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }
$installer = Join-Path $tmp "innosetup-6.7.1.exe"
Invoke-WebRequest -Uri "https://github.com/jrsoftware/issrc/releases/download/is-6_7_1/innosetup-6.7.1.exe" -OutFile $installer
$hash = (Get-FileHash -Algorithm SHA256 $installer).Hash.ToLowerInvariant()
if ($hash -ne "4d11e8050b6185e0d49bd9e8cc661a7a59f44959a621d31d11033124c4e8a7b0") { throw "Inno Setup installer hash mismatch: $hash" }

$inno = Join-Path $tmp "inno-6.7.1"
$p = Start-Process -FilePath $installer -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-", "/CURRENTUSER", "/DIR=`"$inno`"" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "Inno Setup install failed: $($p.ExitCode)" }
$iscc = Join-Path $inno "ISCC.exe"
if (-not (Test-Path $iscc)) { throw "ISCC.exe missing after install" }

& $iscc "/DAppVersion=$Version" "/DSourceDir=$PWD\build\FlayrTune_artefacts\Release\VST3" packaging\windows\FlayrTune.iss
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
