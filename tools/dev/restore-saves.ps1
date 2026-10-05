<#
.SYNOPSIS
  Restore the game's save folder from a backup made by backup-saves.ps1.

.DESCRIPTION
  Overwrites your current saves. Only run it when you really want to go back
  to the backed-up state. It requires -Yes and refuses while the game runs.

  1. Verifies the backup against its manifest (SHA-256 of every file).
  2. Takes a safety backup of the current state first
     (<BackupRoot>\pre-restore-<time>\, same format, also verified).
  3. Inside the backed-up folders (Steam\, Saved\Config\), removes files that
     are not in the backup and copies every backed-up file back.
  4. Verifies the restored files by hash.

  -SaveRoot restores into another folder (for example a scratch copy to try
  the script); -BackupDir picks a backup (default: the newest complete one).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\restore-saves.ps1 -List
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\restore-saves.ps1 -Yes
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\restore-saves.ps1 -BackupDir "$env:USERPROFILE\ff7-remake-vr-backups\saves-20260101-120000" -SaveRoot D:\scratch\saves -Yes
#>
param(
    [string]$BackupDir = '',
    [string]$SaveRoot = '',
    [string]$BackupRoot = '',
    [switch]$List,
    [switch]$Yes
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\common.ps1"

if (-not $BackupRoot) { $BackupRoot = Get-BackupRoot }
if (-not $SaveRoot) { $SaveRoot = Get-SaveRoot }

$all = @(Get-SaveBackups $BackupRoot)
if ($List) {
    if ($all.Count -eq 0) { Write-Output "No backups in $BackupRoot" }
    foreach ($b in $all) { $m = Read-BackupManifest $b; Write-Output ("{0}  {1} files  {2:N1} MB  from {3}" -f $b, $m.count, ($m.bytes / 1MB), $m.source) }
    exit 0
}

if (-not $BackupDir) {
    if ($all.Count -eq 0) { throw "No complete backup in $BackupRoot" }
    $BackupDir = $all[0]
}
if (-not $Yes) {
    Write-Output "This would overwrite the saves in $SaveRoot with $BackupDir. Re-run with -Yes to do it."
    exit 1
}
if (@(Get-GameProcesses).Count -gt 0) { throw 'The game is running; close it first.' }

Write-Step "Verifying $BackupDir"
$problems = @(Test-SaveBackup $BackupDir)
if ($problems.Count -gt 0) { $problems | ForEach-Object { Write-Host "  $_" }; throw 'Backup is not intact; nothing restored' }
$m = Read-BackupManifest $BackupDir

# Safety copy of the current state (skipped if there is nothing to save).
if (@(Get-ScopeFiles $SaveRoot).Count -gt 0) {
    $safety = Backup-Saves -saveRoot $SaveRoot -backupRoot $BackupRoot -prefix 'pre-restore'
    Write-Step "Current state saved to $safety"
}

$wanted = @{}
foreach ($e in @($m.files)) { $wanted[$e.path.ToLowerInvariant()] = $e }

foreach ($f in @(Get-ScopeFiles $SaveRoot)) {
    $rel = (Get-RelativePath $SaveRoot $f.FullName).ToLowerInvariant()
    if (-not $wanted.ContainsKey($rel)) {
        Write-Step "Removing $rel (not in the backup)"
        Remove-Item -LiteralPath $f.FullName -Force
    }
}
foreach ($e in @($m.files)) {
    $src = Join-Path $BackupDir $e.path
    $dst = Join-Path $SaveRoot $e.path
    Ensure-Dir (Split-Path $dst -Parent)
    Copy-Item -LiteralPath $src -Destination $dst -Force
    if ((Get-FileSha256 $dst) -ne $e.sha256) { throw "Restored file does not match the backup: $($e.path)" }
}
Write-Step "Restored $($m.count) files into $SaveRoot from $BackupDir (verified)"
exit 0
