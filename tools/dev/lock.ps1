<#
.SYNOPSIS
  Take, release or show the game lock (.locks\game) or the SteamVR lock (.locks\steamvr).

.DESCRIPTION
  Only one person or script may run the game (or SteamVR) on this machine at a
  time. The lock is a directory: creating it is atomic, so whoever creates it
  owns it. owner.txt inside records "<owner> <time> pid=<pid>".

  A lock is stale when it is older than 20 minutes AND no ff7remake_ process
  runs; a stale lock is removed automatically by the next -Acquire.
  launch.ps1 and stop.ps1 take and release the game lock themselves; use this
  script when you run the game by other means (debugger, manual launch).

  The owner name defaults to env FF7VR_DEV_NAME (or 'dev').

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\lock.ps1 -Status
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\lock.ps1 -Acquire -WaitSeconds 600
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\lock.ps1 -Release
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\lock.ps1 -Acquire -Name steamvr
#>
param(
    [switch]$Acquire,
    [switch]$Release,
    [switch]$Status,
    [ValidateSet('game', 'steamvr')]
    [string]$Name = 'game',
    [string]$Owner = '',
    [int]$WaitSeconds = 0,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

if (-not $Owner) { $Owner = Get-DefaultOwner }
$path = Join-Path $script:LockDir $Name

if ($Acquire) {
    if (Lock-Game -owner $Owner -waitSeconds $WaitSeconds -lockPath $path) { exit 0 }
    exit 1
}
if ($Release) {
    if (Unlock-Game -owner $Owner -force:$Force -lockPath $path) { exit 0 }
    exit 1
}
$info = Get-LockInfo $path
if ($info) {
    Write-Output ("{0}: held by '{1}' for {2} min ({3})" -f $Name, $info.Owner, $info.AgeMinutes, $info.Text)
} else {
    Write-Output "${Name}: free"
}
exit 0
