<#
.SYNOPSIS
  Run several benchmark configurations several times each and print a
  comparison table with the run-to-run spread.

.DESCRIPTION
  Runs bench-run.ps1 for every configuration, -Runs times, interleaved
  (A B C A B C ...) so slow drift (temperature, background activity) spreads
  over all configurations instead of biasing one. Each run is a fresh game
  start into the latest save. Results go to captures\bench\<time>-compare\:
  one folder per run with its result.json, plus summary.json and summary.md.

  The table shows, per configuration, the mean and the sample standard
  deviation over the runs of: average fps, frame time p50/p99, 1% low fps,
  GPU utilisation, GPU memory, game CPU (cores busy), and the per-eye /
  total resolution actually rendered. "spread" is (max - min) / mean of the
  average fps. A difference between two configurations is only called real
  when it is larger than both their spreads.

  Between runs the script pauses -PauseSeconds (35) with no lock held, so
  someone else waiting for the game (lock polling every 30 s) gets a turn.

  -Summarize <folder> re-prints the table of an earlier comparison without
  running anything.

.EXAMPLE
  # Baseline: plain game and UEVR, three runs each (about 20 minutes)
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\bench-compare.ps1 -Configs flat-720p,flat-2eye-2496,uevr-2496 -Runs 3

  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\bench-compare.ps1 -Summarize captures\bench\20261005-140000-compare
#>
param(
    [string[]]$Configs = @(),
    [int]$Runs = 3,
    [int]$WarmupSeconds = 20,
    [int]$RecordSeconds = 45,
    [ValidateSet('idle', 'pan')]
    [string]$Scene = 'idle',
    [string]$ConfigFile = '',
    [string]$BuildDir = '',
    [string]$UevrDir = '',
    [string]$Summarize = '',
    [int]$PauseSeconds = 35,
    [switch]$StopOnFailure
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\bench-common.ps1"

function Format-Num($v, [int]$digits = 1) {
    if ($null -eq $v) { return '-' }
    return ([double]$v).ToString("F$digits", [System.Globalization.CultureInfo]::InvariantCulture)
}

function Get-Summary([string]$dir) {
    $results = @()
    foreach ($f in @(Get-ChildItem -LiteralPath $dir -Recurse -Filter 'result.json' -File)) {
        try { $results += (Get-Content -LiteralPath $f.FullName -Raw | ConvertFrom-Json) } catch { }
    }
    # Rows are configuration + scene ("uevr-2496/pan"; the idle scene is not named).
    foreach ($r in $results) {
        $key = $r.config
        if ($r.PSObject.Properties.Name -contains 'scene' -and $r.scene -and $r.scene -ne 'idle') { $key = "$($r.config)/$($r.scene)" }
        $r | Add-Member -NotePropertyName rowKey -NotePropertyValue $key -Force
    }
    $order = @()
    foreach ($r in ($results | Sort-Object started)) { if ($order -notcontains $r.rowKey) { $order += $r.rowKey } }
    $rows = @()
    foreach ($c in $order) {
        $all = @($results | Where-Object { $_.rowKey -eq $c })
        $ok = @($all | Where-Object { @($_.failures).Count -eq 0 -and $_.PSObject.Properties.Name -contains 'frames' -and $_.frames })
        $fps = Get-MeanStd ([double[]]@($ok | ForEach-Object { $_.frames.avgFps }))
        $row = [ordered]@{
            config = $c
            description = $all[0].description
            runs = $ok.Count
            failed = $all.Count - $ok.Count
            avgFps = $fps
            spreadPct = if ($fps.mean -gt 0) { 100.0 * ($fps.max - $fps.min) / $fps.mean } else { 0 }
            p50Ms = Get-MeanStd ([double[]]@($ok | ForEach-Object { $_.frames.frameTimeMs.p50 }))
            p95Ms = Get-MeanStd ([double[]]@($ok | ForEach-Object { $_.frames.frameTimeMs.p95 }))
            p99Ms = Get-MeanStd ([double[]]@($ok | ForEach-Object { $_.frames.frameTimeMs.p99 }))
            low1Fps = Get-MeanStd ([double[]]@($ok | ForEach-Object { $_.frames.onePercentLowFps }))
            hitches = Get-MeanStd ([double[]]@($ok | Where-Object { $_.frames.PSObject.Properties.Name -contains 'hitchesOver50ms' } | ForEach-Object { $_.frames.hitchesOver50ms }))
            gpuPct = Get-MeanStd ([double[]]@($ok | Where-Object { $_.gpu.PSObject.Properties.Name -contains 'utilPct' } | ForEach-Object { $_.gpu.utilPct }))
            gpuMemMiB = Get-MeanStd ([double[]]@($ok | Where-Object { $_.gpu.PSObject.Properties.Name -contains 'memUsedMiB' } | ForEach-Object { $_.gpu.memUsedMiB }))
            gpuPowerW = Get-MeanStd ([double[]]@($ok | Where-Object { $_.gpu.PSObject.Properties.Name -contains 'powerW' } | ForEach-Object { $_.gpu.powerW }))
            cpuCores = Get-MeanStd ([double[]]@($ok | ForEach-Object { $_.cpu.coresBusy }))
            eye = (@($ok | ForEach-Object { $_.eyeResolution } | Where-Object { $_ } | Select-Object -Unique) -join '/')
            megapixels = Get-MeanStd ([double[]]@($ok | ForEach-Object { $_.renderedPixels / 1e6 }))
            backBuffer = (@($ok | ForEach-Object { $_.backBuffer } | Select-Object -Unique) -join '/')
            # SteamVR's own per-application statistics (whole session, VR configurations only)
            svAppGpuMs = Get-MeanStd ([double[]]@($ok | Where-Object { $_.PSObject.Properties.Name -contains 'steamvrAppStats' -and $_.steamvrAppStats -and $_.steamvrAppStats.PSObject.Properties.Name -contains 'appGpuMs' } | ForEach-Object { $_.steamvrAppStats.appGpuMs }))
            svAppCpuMs = Get-MeanStd ([double[]]@($ok | Where-Object { $_.PSObject.Properties.Name -contains 'steamvrAppStats' -and $_.steamvrAppStats -and $_.steamvrAppStats.PSObject.Properties.Name -contains 'appCpuMs' } | ForEach-Object { $_.steamvrAppStats.appCpuMs }))
        }
        $rows += [pscustomobject]$row
    }
    return $rows
}

function Write-Table($rows, [string]$mdPath) {
    $hdr = '| Configuration | Runs | Avg fps (mean +- sd) | Spread | p50 ms | p99 ms | 1% low fps | Hitches >50ms | GPU % | SteamVR app GPU ms | SteamVR app CPU ms | GPU mem MiB | GPU W | CPU cores | Eye | Rendered MP |'
    $sep = '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
    $lines = @($hdr, $sep)
    foreach ($r in $rows) {
        $lines += ('| {0} | {1}{2} | {3} +- {4} | {5}% | {6} | {7} | {8} | {15} | {9} | {16} | {17} | {10} | {11} | {12} | {13} | {14} |' -f
            $r.config, $r.runs, $(if ($r.failed) { " ($($r.failed) failed)" } else { '' }),
            (Format-Num $r.avgFps.mean 1), (Format-Num $r.avgFps.std 1), (Format-Num $r.spreadPct 1),
            ((Format-Num $r.p50Ms.mean 2) + ' +- ' + (Format-Num $r.p50Ms.std 2)), (Format-Num $r.p99Ms.mean 2), (Format-Num $r.low1Fps.mean 1),
            (Format-Num $r.gpuPct.mean 0), (Format-Num $r.gpuMemMiB.mean 0), (Format-Num $r.gpuPowerW.mean 0),
            (Format-Num $r.cpuCores.mean 2), $(if ($r.eye) { $r.eye } else { '-' }), (Format-Num $r.megapixels.mean 1), (Format-Num $r.hitches.mean 1),
            (Format-Num $r.svAppGpuMs.mean 2), (Format-Num $r.svAppCpuMs.mean 2))
    }
    $lines += ''
    foreach ($r in $rows) { $lines += "- **$($r.config)**: $($r.description) (back buffer $($r.backBuffer))" }
    $lines | ForEach-Object { Write-Host $_ }
    if ($mdPath) { [System.IO.File]::WriteAllLines($mdPath, [string[]]$lines, (New-Object System.Text.UTF8Encoding($false))) }
}

if ($Summarize) {
    $rows = Get-Summary $Summarize
    Write-Table $rows (Join-Path $Summarize 'summary.md')
    exit 0
}

if ($Configs.Count -eq 0) { Write-Step 'Give -Configs name1,name2,... (see tools\bench\configs.psd1)'; exit 1 }
$Configs = @($Configs | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $ConfigFile) { $ConfigFile = Join-Path $PSScriptRoot 'configs.psd1' }
$known = (Import-PowerShellDataFile -LiteralPath $ConfigFile).Configs
foreach ($c in $Configs) { if (-not $known.ContainsKey($c)) { Write-Step "Unknown configuration '$c'"; exit 1 } }

$root = Join-Path $script:BenchResultsDir ((Get-Date).ToString('yyyyMMdd-HHmmss') + '-compare')
Ensure-Dir $root
Write-Step "Comparison of $($Configs -join ', '), $Runs runs each, scene '$Scene', into $root"
$failed = 0
for ($i = 1; $i -le $Runs; $i++) {
    foreach ($c in $Configs) {
        $out = Join-Path $root ("{0}-run{1}" -f $c, $i)
        $args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'bench-run.ps1'),
                  '-Config', $c, '-ConfigFile', $ConfigFile, '-OutDir', $out, '-Label', "run$i",
                  '-WarmupSeconds', [string]$WarmupSeconds, '-RecordSeconds', [string]$RecordSeconds, '-Scene', $Scene)
        if ($BuildDir) { $args += @('-BuildDir', $BuildDir) }
        if ($UevrDir) { $args += @('-UevrDir', $UevrDir) }
        if ($i -gt 1 -or $c -ne $Configs[0]) {
            # Leave the locks free for a moment: others waiting for the game poll every 30 s.
            Start-Sleep -Seconds $PauseSeconds
        }
        Write-Step "=== $c, run $i of $Runs"
        & powershell @args | Where-Object { $_ -notmatch '^\[\d\d:\d\d:\d\d\]   screen:' } | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) {
            $failed++
            Write-Step "Run failed (exit $LASTEXITCODE): $c run $i"
            if ($StopOnFailure) { break }
        }
    }
}

$rows = Get-Summary $root
[System.IO.File]::WriteAllText((Join-Path $root 'summary.json'), ($rows | ConvertTo-Json -Depth 6), (New-Object System.Text.UTF8Encoding($false)))
Write-Host ''
Write-Table $rows (Join-Path $root 'summary.md')
Write-Host ''
Write-Step "Summary: $(Join-Path $root 'summary.md')"

# Resting-state check: nothing of ours may be left running or changed.
$problems = @()
if (@(Get-GameProcesses).Count -gt 0) { $problems += 'ff7remake_ still running' }
if (@(Get-SteamVrProcesses).Count -gt 0) { $problems += 'SteamVR still running' }
if (Test-Path -LiteralPath (Get-LumaDisabledDll)) { $problems += 'dxgi.dll.vr-disabled present (Luma not restored)' }
if (Test-Path -LiteralPath (Get-VrSettingsBackupPath)) { $problems += 'SteamVR settings backup still present (settings not restored)' }
if (Test-Path -LiteralPath $script:UevrStatePath) { $problems += 'UEVR profile not restored' }
$ub = @(Get-UevrProfileBackups)
if ($ub.Count -gt 0) {
    $m = Get-Content -LiteralPath (Join-Path $ub[0] 'backup-manifest.json') -Raw | ConvertFrom-Json
    $d = @(Compare-FolderToManifest (Get-UevrProfileDir) $m)
    if ($d.Count -gt 0) { $problems += "UEVR profile differs from its backup: $($d -join ', ')" }
}
$sd = @(Compare-SavesToBackup)
if ($sd.Count -gt 0) { $problems += "save games differ from the backup: $($sd -join ', ')" }
$owner = Get-DefaultOwner
$gl = Get-LockInfo
if ($gl -and $gl.Owner -eq $owner) { $problems += 'game lock still held' }
if ($problems.Count -gt 0) { $problems | ForEach-Object { Write-Step "NOT CLEAN: $_" }; exit 1 }
Write-Step 'Machine is back in its resting state (no game or SteamVR, Luma enabled, SteamVR settings and UEVR profile restored, save games identical to the backup, locks released)'
if ($failed -gt 0) { exit 1 }
exit 0
