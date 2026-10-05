<#
.SYNOPSIS
  Copy the built mod (xinput1_3.dll, ff7vr.ini) into the game's End\Binaries\Win64.

.DESCRIPTION
  Every file copied is recorded with its SHA-256 in
  End\Binaries\Win64\ff7vr.deploy-manifest.json so undeploy.ps1 removes exactly
  those files. Refuses to overwrite a file it did not deploy itself, and
  refuses while the game is running. Does not take the game lock; launch.ps1
  does that around deploy/undeploy.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\deploy.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\deploy.ps1 -BuildDir build\debug -Ini my-test.ini
#>
param(
    [string]$BuildDir = '',
    [string]$Ini = '',
    [string]$GameDir = ''
)

$ErrorActionPreference = 'Stop'
if ($GameDir) { $env:FF7VR_GAME_DIR = $GameDir }
. "$PSScriptRoot\common.ps1"

$files = Invoke-Deploy -buildDir $BuildDir -iniPath $Ini
$files | ForEach-Object { Write-Output ("  {0,-14} {1,9} bytes  {2}" -f $_.name, $_.size, $_.sha256.Substring(0, 16)) }
exit 0
