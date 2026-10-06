<#
.SYNOPSIS
  One measurement session of the mod's stereo rendering at a given eye size, without a headset.

.DESCRIPTION
  Starts the game with the player's ini (tools\package\ff7vr.ini) plus overrides for
  measuring: the XR Null backend at the given eye size without frame pacing, the dev pipe,
  bloom off while in stereo (as in the player's own ini), no periodic timing reports. Loads
  the latest save, lifts the game's frame cap (t.MaxFPS 0), runs a steps file (PowerShell
  dot-sourced, with the helpers of vr-perf-lib.ps1: Measure-Perf, P, Start-Pan), writes
  results.json / results.txt and the mod's log into the output folder, then stops the game
  and puts everything back (tools\dev\stop.ps1).

  Measure-Perf <label> <seconds> gives the engine's frame interval (avg, p50, p95, max) and
  the GPU time of the scene and of everything after it until Present (timestamp queries of
  the foveation module) over the same window. Frame times are uncapped: the GPU frame time,
  not a headset's paced rate. See docs/benchmarking.md, "Stereo at headset resolution".

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\vr-perf-session.ps1 -EyeWidth 3072 -EyeHeight 3264 -Steps tools\bench\vr-perf-steps\scenes.ps1
#>
param(
    [int]$EyeWidth = 3072,
    [int]$EyeHeight = 3264,
    [Parameter(Mandatory = $true)][string]$Steps,
    [string]$Tag = 'perf',
    [string]$BuildDir = '',
    [string]$OutRoot = '',
    [string[]]$ExtraSet = @(),
    [switch]$NoStop
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
. "$repo\tools\dev\common.ps1"
. "$repo\tools\bench\bench-common.ps1"
. "$PSScriptRoot\vr-perf-lib.ps1"
if (-not $OutRoot) { $OutRoot = Join-Path $repo 'captures\perf' }
$out = Join-Path $OutRoot ((Get-Date -Format 'yyyyMMdd-HHmmss') + "-$Tag-${EyeWidth}x$EyeHeight")
New-Item -ItemType Directory -Force $out | Out-Null
Write-Host "OUT $out"
$stepsPath = (Resolve-Path $Steps).Path

$set = @('xr.backend=null', 'dev.pipe=1', "xr.eye_width=$EyeWidth", "xr.eye_height=$EyeHeight", 'xr.null_pace=0', 'xr.null_refresh_hz=72',
    'stereo_cvars.r.BloomQuality=0', 'render.stats_interval=600') + $ExtraSet
$launchArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$repo\tools\dev\launch.ps1", '-Ini', "$repo\tools\package\ff7vr.ini",
    '-Until', 'gameplay', '-KeepRunning', '-LockWaitSeconds', '1200', '-Set', ($set -join ';'))
if ($BuildDir) { $launchArgs += @('-BuildDir', $BuildDir) }
# Exit code 3 of launch.ps1: no lock, or a game it did not start is running (someone else's run).
# That game must not be stopped here.
$ours = $true
try {
    & powershell @launchArgs
    if ($LASTEXITCODE -eq 3) { $ours = $false }
    if ($LASTEXITCODE -ne 0) { throw "launch failed ($LASTEXITCODE)" }
    P 'cvar set t.MaxFPS 0' | Out-Null
    Start-Sleep 4
    P 'stereo status;fov status;dynres;ssr;cvar get r.ScreenPercentage;cvar get r.BloomQuality' | Out-File "$out\status.txt"
    . $stepsPath
} finally {
    Save-PerfResults "$out\results.json"
    $script:PerfResults | Format-Table -AutoSize | Out-String -Width 200 | Tee-Object -FilePath "$out\results.txt"
    if (-not $ours) {
        Write-Host 'The game was not started by this session: left running, nothing restored.'
    } elseif (-not $NoStop) {
        & powershell -NoProfile -ExecutionPolicy Bypass -File "$repo\tools\dev\stop.ps1"
        $runs = Get-ChildItem "$repo\captures\runs" -Directory | Sort-Object LastWriteTime | Select-Object -Last 1
        if ($runs -and (Test-Path "$($runs.FullName)\ff7vr.log")) { Copy-Item "$($runs.FullName)\ff7vr.log" "$out\ff7vr.log" }
    }
}
