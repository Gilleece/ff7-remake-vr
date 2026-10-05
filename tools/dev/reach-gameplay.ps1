<#
.SYNOPSIS
  Take an already running game from the title screen into the latest save.

.DESCRIPTION
  Same sequence as launch.ps1 -Until gameplay, for a game that is already
  running (for example started with launch.ps1 -KeepRunning):
  title -> Enter -> Continue -> Enter -> "Resume playing...?" Yes -> Enter ->
  loading -> gameplay. It only loads; it never picks New Game and never saves.
  Each key press happens only after a screenshot shows the expected screen.
  A PNG of every screen change goes to captures\runs\<time>-steps\.
  Exit code 0 when gameplay is reached.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\launch.ps1 -Until title -KeepRunning
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\reach-gameplay.ps1
#>
param(
    [int]$TimeoutSeconds = 240
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

if (@(Get-GameProcesses).Count -eq 0) { Write-Step 'The game is not running'; exit 1 }
$shotDir = Join-Path $script:CapturesDir ('runs\' + (New-RunFolderName 'steps'))
if (Invoke-ReachGameplay -timeoutSeconds $TimeoutSeconds -shotDir $shotDir) {
    Write-Step "Gameplay reached (step captures in $shotDir)"
    exit 0
}
Write-Step "Gameplay NOT reached (step captures in $shotDir)"
exit 1
