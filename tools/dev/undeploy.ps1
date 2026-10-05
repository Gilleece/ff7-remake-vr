<#
.SYNOPSIS
  Remove what deploy.ps1 put into the game folder, and nothing else.

.DESCRIPTION
  Reads End\Binaries\Win64\ff7vr.deploy-manifest.json and deletes the files it
  lists. Files the mod writes at runtime (ff7vr.log, ff7vr-crash-*.dmp) are
  first moved to captures\runs\<time>\ in the repo. A deployed file whose hash
  changed since deployment is left in place unless -Force is given. Refuses
  while the game is running. Safe to run when nothing is deployed.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\undeploy.ps1
#>
param(
    [switch]$Force,
    [string]$GameDir = ''
)

$ErrorActionPreference = 'Stop'
if ($GameDir) { $env:FF7VR_GAME_DIR = $GameDir }
. "$PSScriptRoot\common.ps1"

if (Invoke-Undeploy -force:$Force) { exit 0 }
exit 1
