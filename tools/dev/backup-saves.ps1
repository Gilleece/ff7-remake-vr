<#
.SYNOPSIS
  Back up the game's save folder once, verified by SHA-256.

.DESCRIPTION
  Copies <SaveRoot>\Steam\ (every account folder) and <SaveRoot>\Saved\Config\
  to <BackupRoot>\saves-<yyyyMMdd-HHmmss>\ and writes backup-manifest.json with
  the hash of every file. Each copy is compared with its source before the
  manifest is written. If a complete backup already exists nothing is copied,
  unless -New is given. launch.ps1 calls this automatically before every run.

  SaveRoot defaults to %USERPROFILE%\Documents\My Games\FINAL FANTASY VII REMAKE
  (env FF7VR_SAVE_ROOT overrides). BackupRoot defaults to
  %USERPROFILE%\ff7-remake-vr-backups (env FF7VR_BACKUP_DIR overrides).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\backup-saves.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\backup-saves.ps1 -Verify
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\backup-saves.ps1 -New
#>
param(
    [string]$SaveRoot = '',
    [string]$BackupRoot = '',
    [switch]$New,
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

if (-not $SaveRoot) { $SaveRoot = Get-SaveRoot }
if (-not $BackupRoot) { $BackupRoot = Get-BackupRoot }

if ($Verify) {
    $all = @(Get-SaveBackups $BackupRoot)
    if ($all.Count -eq 0) { Write-Step "No backup in $BackupRoot"; exit 1 }
    $problems = @(Test-SaveBackup $all[0])
    if ($problems.Count -gt 0) {
        Write-Step "Backup $($all[0]) is NOT intact:"
        $problems | ForEach-Object { Write-Host "  $_" }
        exit 1
    }
    $m = Read-BackupManifest $all[0]
    Write-Step "Backup $($all[0]) intact ($($m.count) files, created $($m.created))"
    exit 0
}

$dir = Backup-Saves -saveRoot $SaveRoot -backupRoot $BackupRoot -ifMissing:(-not $New)
Write-Output $dir
exit 0
