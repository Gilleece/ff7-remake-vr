<#
.SYNOPSIS
  Stop every SteamVR process and release the SteamVR lock.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\steamvr-stop.ps1 -Owner dev
#>
param(
    [string]$Owner = '',
    [switch]$Force  # release the lock even if someone else holds it
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\steamvr-common.ps1"
if (-not $Owner) { $Owner = Get-DefaultSvOwner }

$holder = Get-SteamVrLockOwner
if ($holder -and $holder -ne $Owner -and -not $Force) {
    Write-Sv "SteamVR lock is held by '$holder'; not stopping their run (use -Force)."
    exit 1
}
$stopped = Stop-SteamVrProcesses
if (-not (Unlock-SteamVr -owner $Owner -force:$Force)) { exit 1 }
if ($stopped) { exit 0 } else { exit 1 }
