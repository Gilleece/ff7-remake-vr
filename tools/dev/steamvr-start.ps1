<#
.SYNOPSIS
  Take the SteamVR lock and start SteamVR, waiting until its server is up.

.DESCRIPTION
  Starts <SteamVR>\bin\win64\vrstartup.exe and waits for vrserver and
  vrcompositor. Takes .locks\steamvr first (waits up to -WaitLockSeconds for it).
  Stop with steamvr-stop.ps1, which also releases the lock.
  Note that an OpenXR app pointed at SteamVR starts it on its own as well; this
  script is for runs where SteamVR should be up before the app.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\steamvr-start.ps1 -Owner dev
#>
param(
    [string]$Owner = '',
    [int]$TimeoutSeconds = 60,
    [int]$WaitLockSeconds = 0
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\steamvr-common.ps1"
if (-not $Owner) { $Owner = Get-DefaultSvOwner }

$vrRoot = Get-SteamVrRoot
if (-not $vrRoot) { Write-Sv 'SteamVR is not installed'; exit 1 }
if (-not (Lock-SteamVr -owner $Owner -waitSeconds $WaitLockSeconds)) { exit 1 }

if (-not (Test-SteamVrRunning)) {
    $exe = Join-Path $vrRoot 'bin\win64\vrstartup.exe'
    Write-Sv "Starting $exe"
    Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe -Parent) | Out-Null
}
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $deadline) {
    $names = @(Get-SteamVrProcesses | ForEach-Object { $_.ProcessName })
    if (($names -contains 'vrserver') -and ($names -contains 'vrcompositor')) {
        Write-Sv "SteamVR is up ($($names -join ', '))"
        exit 0
    }
    Start-Sleep -Milliseconds 500
}
Write-Sv "SteamVR did not come up within $TimeoutSeconds s (running: $((Get-SteamVrProcesses | ForEach-Object { $_.ProcessName }) -join ', '))"
exit 1
