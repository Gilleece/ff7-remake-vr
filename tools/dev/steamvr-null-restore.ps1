<#
.SYNOPSIS
  Undo steamvr-null-enable.ps1: put the original steamvr.vrsettings back.

.DESCRIPTION
  Stops SteamVR if it is running (it rewrites the settings file on exit), copies
  steamvr.vrsettings.ff7vr-backup back over steamvr.vrsettings byte for byte,
  checks the copy and deletes the backup. If the settings file did not exist
  before, it is removed instead. Safe to run when nothing needs restoring.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\steamvr-null-restore.ps1
#>
param([switch]$KeepSteamVrRunning)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\steamvr-common.ps1"

$settings = Get-VrSettingsPath
$backup = Get-VrSettingsBackupPath
if (-not (Test-Path -LiteralPath $backup)) {
    Write-Sv "No backup at ${backup}: nothing to restore"
    exit 0
}
if (Test-SteamVrRunning) {
    if ($KeepSteamVrRunning) { Write-Sv 'SteamVR is running; restore needs it stopped.'; exit 1 }
    if (-not (Stop-SteamVrProcesses)) { exit 1 }
}

$content = Get-Content -LiteralPath $backup -Raw
if ($content -match 'ff7vr_backup_note') {
    if (Test-Path -LiteralPath $settings) { Remove-Item -LiteralPath $settings }
    Remove-Item -LiteralPath $backup
    Write-Sv "Removed $settings (it did not exist before)"
    exit 0
}

Copy-Item -LiteralPath $backup -Destination $settings -Force
$a = (Get-FileHash -LiteralPath $settings -Algorithm SHA256).Hash
$b = (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash
if ($a -ne $b) { Write-Sv "Restore check failed: $settings differs from $backup (backup kept)"; exit 1 }
Remove-Item -LiteralPath $backup
Write-Sv "Restored $settings (SHA256 $a)"
exit 0
