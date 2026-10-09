<#
.SYNOPSIS
  Restart cycles: start the game to gameplay, measure it, stop it, wait, start again. For the
  slow state after a quick restart (docs\benchmarking.md, "Video memory and slow phases", state 3).

.DESCRIPTION
  Each item of -Runs is "<kind>-<gap>":
    kind  mod    the mod from -BuildDir, Null backend (-Set), ini from -Ini or the build's template
          dlss   the mod from -DlssBuildDir (a build with DLSS), -Set plus -DlssSet
          nomod  the game without the mod, flat, in a -NoModWidth x -NoModHeight window
    gap   seconds from the previous game's exit (the moment its process was gone) to the start of
          the next game process. The first run starts at once.
  A run: launch.ps1 to gameplay (-NoIdleWait), -Seconds of walking (W/A/S/D) with a record every
  10 s (mod: `stereo status` frame times and the `vram` line), optionally upload_probe inside the
  run, then the game is killed and the mod undeployed (the lock stays held for the whole series).
  -ProbeAfterExit runs upload_probe at the given seconds after each exit, before the next start.
  -OnSlow names a script that is dot-sourced after the exit of a slow run (tests of the state with
  no game running); it can call Invoke-Probe and Note.

  Two loggers run for the whole series: tools\bench\nvml-log.ps1 (every -SmiMs ms: P-state,
  PCIe link, throughput and replays, memory, power, utilisation, clocks) and
  tools\bench\gpu-counters.ps1 (1 s: busy engines and memory per process, the whole card's
  memory). Everything goes to captures\restart\<stamp>-<tag>\:
    run.txt      the narrative with every measurement
    runs.csv     one row per run (written as the series goes)
    nvml.csv, gpu.csv, <run>\launch.txt, <run>\ff7vr.log
  tools\bench\restart-analyze.ps1 adds the per-run engine and memory figures from the loggers.

  A run counts as slow when (mod, dlss) the median of the 10-s frame-time medians from 30 s on is
  above 20 ms, or (all kinds) the card's average power from 20 s on is below -SlowWatts while the
  game runs: in the slow state the card reports full utilisation at 55-110 W.

.EXAMPLE
  $env:FF7VR_DEV_NAME = 'me'
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\restart-cycle.ps1 -Tag base `
      -Runs mod-9,mod-9,mod-9,mod-90 -ProbeAfterExit 3 -ProbeInRun
#>
param(
    [Parameter(Mandatory = $true)][string[]]$Runs,
    [string]$Tag = 'cycle',
    [int]$Seconds = 60,
    [string]$BuildDir = '',
    [string]$DlssBuildDir = '',
    [string]$Ini = '',
    [string]$Set = 'xr.backend=null;xr.null_refresh_hz=90;xr.null_motion=yaw;xr.eye_width=3072;xr.eye_height=3264;log.level=info;dev.pipe=1',
    [string]$DlssSet = 'dlss.enabled=1;dlss.mode=upscale;dlss.input_scale=0.65;dlss.output=runtime',
    [int]$NoModWidth = 6144,
    [int]$NoModHeight = 3264,
    [double[]]$ProbeAfterExit = @(),
    [switch]$ProbeInRun,
    [string]$Probe = '',
    [string]$OnSlow = '',
    [switch]$StopAfterSlow,
    [double]$SlowWatts = 150,
    [int]$SmiMs = 200,
    [double]$FirstGap = 120,
    [double]$LaunchLead = 2.5,
    [string]$OutRoot = ''
)

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\..\dev\common.ps1"

$owner = Get-DefaultOwner
$inv = [Globalization.CultureInfo]::InvariantCulture
$BuildDir = Resolve-BuildDir $BuildDir
if (-not $Probe) { $Probe = Join-Path $BuildDir 'tools\upload_probe\upload_probe.exe' }
if (-not $OutRoot) { $OutRoot = Join-Path $script:CapturesDir 'restart' }
$sdir = Join-Path $OutRoot ((Get-Date).ToString('yyyyMMdd-HHmmss') + '-' + $Tag)
Ensure-Dir $sdir
$script:RunDir = $sdir

function Note([string]$msg) {
    $line = '{0} {1}' -f (Get-Date).ToString('HH:mm:ss.fff'), $msg
    Write-Host $line
    # Shared opening and a few tries: a viewer that has the file open must not end the series.
    for ($try = 0; $try -lt 5; $try++) {
        try {
            $fs = New-Object System.IO.FileStream((Join-Path $sdir 'run.txt'), [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write, [System.IO.FileShare]::ReadWrite)
            try { $b = [System.Text.Encoding]::UTF8.GetBytes($line + "`r`n"); $fs.Write($b, 0, $b.Length) } finally { $fs.Dispose() }
            break
        } catch { Start-Sleep -Milliseconds 100 }
    }
}

function Get-Smi {
    return [string](& nvidia-smi '--query-gpu=pstate,pcie.link.gen.gpucurrent,pcie.link.width.current,memory.used,power.draw,utilization.gpu,clocks.sm,clocks.mem' '--format=csv,noheader,nounits' 2>$null)
}

function Get-Watts {
    $s = (Get-Smi) -split ','
    if ($s.Count -ge 5) { try { return [double]::Parse($s[4].Trim(), $inv) } catch { } }
    return -1
}

# Runs upload_probe with a hard deadline; returns its output joined on one line.
function Invoke-Probe([string]$probeArgs, [int]$deadlineMs = 15000) {
    if (-not (Test-Path -LiteralPath $Probe)) { return "no probe at $Probe" }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Probe
    $psi.Arguments = $probeArgs
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.CreateNoWindow = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEndAsync()
    if (-not $p.WaitForExit($deadlineMs)) {
        try { $p.Kill() } catch { }
        return "KILLED after $deadlineMs ms: " + ($out.Result -replace "`r?`n", ' | ')
    }
    $p.WaitForExit()
    return ('exit {0}: {1}' -f $p.ExitCode, (($out.Result.Trim()) -replace "`r?`n", ' | '))
}

function Test-Ours { $i = Get-LockInfo; return ($i -and $i.Owner -eq $owner) }
function Test-Alive { return (@(Get-GameProcesses).Count -gt 0 -and (Test-Ours)) }

function Send-Pipe([string[]]$cmds) {
    if (-not (Test-Alive)) { return @('err: no game or not our lock') }
    try { return @(Send-DevCommand $cmds 5000) } catch { return @("err: $_") }
}

# Kill, keep the log, undeploy, restore Luma; the lock stays held.
function Stop-Run([string]$runDir) {
    $gp = @(Get-GameProcesses)
    Stop-GameProcesses
    $exit = Get-Date
    foreach ($p in $gp) {
        try { Note ("pid {0}: exited {1}, exit code {2}, exit time {3}" -f $p.Id, $p.HasExited, $p.ExitCode, $p.ExitTime.ToString('HH:mm:ss.fff')) } catch { }
        try { $p.Dispose() } catch { }
    }
    try { Read-SharedText (Get-GameLogPath) | Out-File -LiteralPath (Join-Path $runDir 'ff7vr.log') -Encoding utf8 } catch { }
    try { [void](Invoke-Undeploy) } catch { Note "undeploy: $_" }
    try { [void](Restore-Luma) } catch { Note "Luma: $_" }
    return $exit
}

# Never take over someone else's game.
if (@(Get-GameProcesses).Count -gt 0) { Note 'ff7remake_ is running already; not starting'; exit 3 }
if (-not (Lock-Game -owner $owner -waitSeconds 1800)) { Note 'could not get the game lock'; exit 3 }

$smiProc = Start-Process -FilePath 'powershell' -WindowStyle Hidden -PassThru -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'nvml-log.ps1'), '-OutCsv', (Join-Path $sdir 'nvml.csv'), '-IntervalMs', "$SmiMs")
$ctrProc = Start-Process -FilePath 'powershell' -WindowStyle Hidden -PassThru -ArgumentList @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot 'gpu-counters.ps1'), '-OutCsv', (Join-Path $sdir 'gpu.csv'))
Note ("series {0}: runs {1}; build {2}; dlss build {3}; ini {4}; probe {5}" -f $Tag, ($Runs -join ' '), $BuildDir, $DlssBuildDir, $Ini, $Probe)
Note ("other GPU users: " + ((Get-Process | Where-Object { $_.ProcessName -match 'Broadcast|VirtualDesktop|vrserver|vrcompositor|obs|Afterburner|RTSS' } | ForEach-Object { $_.ProcessName } | Sort-Object -Unique) -join ', '))
$csv = Join-Path $sdir 'runs.csv'
Add-Content -LiteralPath $csv 'name,kind,gap_planned,gap_actual_s,pid,start,gameplay_s,window_start,window_end,exit,smi_at_start,p50s,median_p50_after30,watts_after20,probe_in_run_gbs,slow'

$prevExit = Get-GameExitTime
$items = @($Runs | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
$idx = 0
try {
    foreach ($item in $items) {
        $idx++
        $kind, $gapText = $item -split '-', 2
        $gap = [double]::Parse($gapText, $inv)
        $name = '{0:D2}-{1}-{2}' -f $idx, $kind, $gapText
        $rdir = Join-Path $sdir $name
        Ensure-Dir $rdir
        if (-not (Test-Ours)) { Note 'lost the game lock; stopping'; break }

        # Wait for the planned gap; launch.ps1 needs about -LaunchLead s to its Start-Process. The
        # first run waits -FirstGap s after the last exit any harness run recorded.
        if ($prevExit) {
            $g = if ($idx -gt 1) { $gap } else { [Math]::Max($gap, $FirstGap) }
            $wake = $prevExit.AddSeconds([Math]::Max(0, $g - $LaunchLead))
            if ($wake -gt (Get-Date)) { Note ("waiting until {0} ({1} s after the last exit)" -f $wake.ToString('HH:mm:ss'), $g) }
            while ((Get-Date) -lt $wake) { Start-Sleep -Milliseconds 100 }
        }
        $smiStart = Get-Smi
        Note "=== $name (gap $gap s) smi at start: $smiStart"
        $tl = Get-Date
        if ($kind -eq 'nomod') {
            $out = & powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\..\dev\launch.ps1" -NoMod -Width $NoModWidth -Height $NoModHeight -KeepRunning -Until gameplay -UntilTimeout 240 -NoIdleWait 2>&1
        } else {
            $bd = if ($kind -eq 'dlss') { $DlssBuildDir } else { $BuildDir }
            $s = if ($kind -eq 'dlss') { $Set + ';' + $DlssSet } else { $Set }
            $la = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$PSScriptRoot\..\dev\launch.ps1", '-BuildDir', $bd, '-Set', $s, '-KeepRunning', '-Until', 'gameplay', '-UntilTimeout', '240', '-NoIdleWait')
            if ($Ini) { $la += @('-Ini', $Ini) }
            $out = & powershell @la 2>&1
        }
        $out | Out-File -LiteralPath (Join-Path $rdir 'launch.txt') -Encoding utf8
        $gp = @(Get-GameProcesses)
        $gpid = if ($gp.Count) { $gp[0].Id } else { 0 }
        $gstart = if ($gp.Count) { $gp[0].StartTime } else { $null }
        $gapActual = if ($prevExit -and $gstart) { ($gstart - $prevExit).TotalSeconds } else { -1 }
        $toGame = ((Get-Date) - $tl).TotalSeconds
        Note ("launch exit {0}; pid {1}; started {2}; gap actual {3:N1} s; gameplay after {4:N0} s" -f $LASTEXITCODE, $gpid,
              $(if ($gstart) { $gstart.ToString('HH:mm:ss.fff') } else { '-' }), $gapActual, $toGame)

        $wins = @(); $watts = @(); $probeIn = ''
        $w0 = Get-Date; $w1 = $w0
        if (Test-Alive) {
            if ($kind -ne 'nomod') { [void](Send-Pipe @('stereo frametime 10')) }
            $end = $w0.AddSeconds($Seconds); $next = $w0.AddSeconds(10); $i = 0
            $probeAt = $w0.AddSeconds([Math]::Max(10, $Seconds - 10)); $probed = $false
            while ((Get-Date) -lt $end) {
                if (@(Get-GameProcesses).Count -eq 0) { Note 'the game exited during the run'; break }
                $k = @('w', 'w', 'a', 's', 's', 'd')[$i % 6]; $i++
                try { Send-GameKey $k 1200 } catch { }
                $el = ((Get-Date) - $w0).TotalSeconds
                $wv = Get-Watts
                if ($wv -ge 0) { $watts += [pscustomobject]@{ T = $el; W = $wv } }
                if ($ProbeInRun -and -not $probed -and (Get-Date) -ge $probeAt) {
                    $probed = $true
                    $probeIn = Invoke-Probe '--mb 64 --timeout-ms 4000 up'
                    Note "probe in run: $probeIn"
                }
                if ((Get-Date) -ge $next) {
                    $next = $next.AddSeconds(10)
                    $fm = '-'; $p50 = -1; $vr = ''
                    if ($kind -ne 'nomod') {
                        $st = [string](@(Send-Pipe @('stereo status'))[0])
                        if ($st -match 'frame_ms_avg=([\d.]+) p50=([\d.]+) p95=([\d.]+) max=([\d.]+) \(n=(\d+)') {
                            $p50 = [double]::Parse($Matches[2], $inv)
                            $fm = "avg $($Matches[1]) p50 $($Matches[2]) p95 $($Matches[3]) n $($Matches[5])"
                        }
                        $vr = [string](@(Send-Pipe @('vram'))[0])
                    }
                    Note ("t+{0:N0}s frame {1}; smi {2}; {3}" -f $el, $fm, (Get-Smi), $vr)
                    $wins += [pscustomobject]@{ T = $el; P50 = $p50 }
                }
            }
            $w1 = Get-Date
        }
        $prevExit = Stop-Run $rdir
        Note ("exit at {0}; smi {1}" -f $prevExit.ToString('HH:mm:ss.fff'), (Get-Smi))

        # Classify.
        $med = -1
        $late = @($wins | Where-Object { $_.T -ge 30 -and $_.P50 -gt 0 } | ForEach-Object { $_.P50 } | Sort-Object)
        if ($late.Count) { $med = $late[[int][Math]::Floor(($late.Count - 1) / 2)] }
        $wl = @($watts | Where-Object { $_.T -ge 20 })
        $wavg = if ($wl.Count) { ($wl | Measure-Object W -Average).Average } else { -1 }
        $slow = ($med -gt 20) -or ($wavg -ge 0 -and $wavg -lt $SlowWatts)
        $gbs = if ($probeIn -match 'up \d+ MB: gpu [\d.]+ ms \(([\d.]+) GB/s\)') { $Matches[1] } elseif ($probeIn) { 'timeout' } else { '' }
        Note ("RESULT {0}: frame median from 30 s {1} ms, power from 20 s {2:N0} W, probe {3} GB/s, slow={4}" -f $name, $med, $wavg, $gbs, $slow)
        Add-Content -LiteralPath $csv ([string]::Format($inv, '{0},{1},{2},{3:F1},{4},{5},{6:F0},{7},{8},{9},"{10}",{11},{12},{13:F0},{14},{15}',
            $name, $kind, $gap, $gapActual, $gpid, $(if ($gstart) { $gstart.ToString('o') } else { '' }), $toGame,
            $w0.ToString('o'), $w1.ToString('o'), $prevExit.ToString('o'), $smiStart,
            (($wins | ForEach-Object { '{0:F1}' -f $_.P50 }) -join ' '), $med, $wavg, $gbs, $slow))

        foreach ($at in $ProbeAfterExit) {
            while (((Get-Date) - $prevExit).TotalSeconds -lt $at) { Start-Sleep -Milliseconds 100 }
            Note ("probe +{0} s after the exit (smi {1}): {2}" -f $at, (Get-Smi), (Invoke-Probe '--mb 64 --timeout-ms 4000 info up'))
        }
        if ($slow -and $OnSlow) {
            $ExitTime = $prevExit
            Note "on-slow script $OnSlow"
            . $OnSlow
            Note 'on-slow script done'
            $prevExit = $ExitTime
        }
        if ($slow -and $StopAfterSlow) { Note 'stopping after the first slow run'; break }
    }
} catch {
    Note "ERROR: $_"
} finally {
    if (@(Get-GameProcesses).Count -gt 0 -and (Test-Ours)) { try { [void](Stop-Run $sdir) } catch { } }
    Start-Sleep -Seconds 5
    foreach ($p in @($smiProc, $ctrProc)) { if ($p) { try { Stop-Process -Id $p.Id -Force -ErrorAction Stop } catch { } } }
    if (Test-Ours) { [void](Unlock-Game -owner $owner) }
    Note ('series end; folder ' + $sdir)
}
