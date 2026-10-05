<#
.SYNOPSIS
  Switch SteamVR to its null (virtual headset) driver for headless OpenXR tests.

.DESCRIPTION
  Edits <Steam>\config\steamvr.vrsettings (found through openvrpaths.vrpath or the
  Steam registry key) after backing it up once to steamvr.vrsettings.ff7vr-backup
  next to it. The backup is only written if none exists, so running this twice
  keeps the original. Undo with steamvr-null-restore.ps1.

  Settings written:
    steamvr.forcedDriver = "null"        use the null driver as the headset
    steamvr.requireHmd   = true          (the null driver provides one)
    steamvr.enableHomeApp = false        no SteamVR Home
    driver_null.enable   = true          the null driver is disabled by default
    driver_null.renderWidth/Height, displayFrequency, windowWidth/Height  (only if given)

  SteamVR must not be running (it rewrites the file on exit); use -StopSteamVr to
  stop it first. Nothing outside steamvr.vrsettings is changed, and the system
  default OpenXR runtime is not touched.

  Takes the SteamVR lock (.locks\steamvr) for -Owner first, waiting up to
  -WaitLockSeconds, so it cannot change the settings under someone else's run
  or race their restore. steamvr-start.ps1 / steamvr-stop.ps1 with the same
  owner continue with that lock; steamvr-stop.ps1 releases it.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\steamvr-null-enable.ps1
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\steamvr-null-enable.ps1 -RenderWidth 2064 -RenderHeight 2208 -RefreshHz 90
#>
param(
    [int]$RenderWidth = 0,
    [int]$RenderHeight = 0,
    [double]$RefreshHz = 0,
    [int]$WindowWidth = 0,
    [int]$WindowHeight = 0,
    [switch]$StopSteamVr,
    [string]$Owner = '',
    [int]$WaitLockSeconds = 900
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\steamvr-common.ps1"

$vrRoot = Get-SteamVrRoot
if (-not $vrRoot) { Write-Sv 'SteamVR is not installed (steamxr_win64.json not found)'; exit 1 }
$nullDefaults = Join-Path $vrRoot 'drivers\null\resources\settings\default.vrsettings'
if (-not (Test-Path -LiteralPath (Join-Path $vrRoot 'drivers\null\bin\win64\driver_null.dll'))) {
    Write-Sv "SteamVR's null driver is missing under $vrRoot\drivers\null"; exit 1
}
Write-Sv "SteamVR: $vrRoot"
Write-Sv "Null driver defaults (read only, not modified): $nullDefaults"

if (-not $Owner) { $Owner = Get-DefaultSvOwner }
if (-not (Lock-SteamVr -owner $Owner -waitSeconds $WaitLockSeconds)) { Write-Sv 'Not changing the settings while someone else uses SteamVR'; exit 1 }

if (Test-SteamVrRunning) {
    if (-not $StopSteamVr) { Write-Sv 'SteamVR is running. Close it first or pass -StopSteamVr.'; exit 1 }
    if (-not (Stop-SteamVrProcesses)) { exit 1 }
}

$settings = Get-VrSettingsPath
$backup = Get-VrSettingsBackupPath
if (Test-Path -LiteralPath $backup) {
    Write-Sv "Backup already exists, keeping it: $backup"
} elseif (Test-Path -LiteralPath $settings) {
    Copy-Item -LiteralPath $settings -Destination $backup
    Write-Sv "Backed up $settings -> $backup"
} else {
    # No settings file yet: remember that, so restore deletes ours.
    Set-Content -LiteralPath $backup -Value '{"ff7vr_backup_note": "steamvr.vrsettings did not exist"}' -Encoding ASCII
    Write-Sv "No $settings yet; restore will remove the file"
}

$json = [pscustomobject]@{}
if (Test-Path -LiteralPath $settings) {
    $raw = Get-Content -LiteralPath $settings -Raw
    if ($raw.Trim()) { $json = $raw | ConvertFrom-Json }
}
Set-VrSetting $json 'steamvr' 'forcedDriver' 'null'
Set-VrSetting $json 'steamvr' 'requireHmd' $true
Set-VrSetting $json 'steamvr' 'enableHomeApp' $false
Set-VrSetting $json 'driver_null' 'enable' $true
if ($RenderWidth -gt 0) { Set-VrSetting $json 'driver_null' 'renderWidth' $RenderWidth }
if ($RenderHeight -gt 0) { Set-VrSetting $json 'driver_null' 'renderHeight' $RenderHeight }
if ($RefreshHz -gt 0) { Set-VrSetting $json 'driver_null' 'displayFrequency' $RefreshHz }
if ($WindowWidth -gt 0) { Set-VrSetting $json 'driver_null' 'windowWidth' $WindowWidth }
if ($WindowHeight -gt 0) { Set-VrSetting $json 'driver_null' 'windowHeight' $WindowHeight }

Write-JsonNoBom $settings ($json | ConvertTo-Json -Depth 32)
Write-Sv "Null driver enabled in $settings"
Write-Sv 'Undo with tools\dev\steamvr-null-restore.ps1'
exit 0
