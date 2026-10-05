<#
.SYNOPSIS
  Stop the game and put everything back: kill ff7remake_, undeploy the mod,
  restore Luma/ReShade's dxgi.dll, release the game lock.

.DESCRIPTION
  Idempotent and safe at any time: after a crash, after launch.ps1 -KeepRunning,
  or when nothing is running. The mod's log and crash dumps are moved to
  captures\runs\<time>\ by the undeploy step.

  If the game lock is held by another owner (env FF7VR_DEV_NAME differs), the
  script does nothing and exits 1, so it cannot kill someone else's run.
  -Force overrides that.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\stop.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\stop.ps1 -Force
#>
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

$ok = Stop-GameRun -force:$Force
$left = @(Get-GameProcesses).Count + @(Get-GameHelperProcesses).Count
Write-Step ("State: game processes {0}, mod deployed {1}, Luma dxgi.dll {2}, lock {3}" -f $left,
    ($null -ne (Read-DeployManifest)), (Test-Path -LiteralPath (Get-LumaDll)), (Test-Path -LiteralPath $script:GameLock))
if ($ok) { exit 0 }
exit 1
