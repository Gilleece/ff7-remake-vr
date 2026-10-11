<#
.SYNOPSIS
  One game run for the mid-session stall (docs\benchmarking.md, "Mid-session stall"): start to
  gameplay, send dev pipe commands on a schedule, watch the timing block and the card's upload
  rate, stop and clean up.

.DESCRIPTION
  -Seq is a list of "<seconds after gameplay>:<dev pipe command>" items, separated by '|' (or
  given as an array). The command 'probe' runs upload_probe (32 MB up and down) instead.
  -OnStall items have the same form, counted from the moment the stall test below first holds.

  For the whole run a background job times an 8 MB copy from system memory to the card with
  upload_probe every 3 s (probeloop.txt: time and GB/s), so the moment the card's uploads turn
  slow (the slow PCIe state) and the moment they recover are known to a few seconds; upload_probe
  also runs once before the start and 3 s after the exit. nvidia-smi logs P-state, clocks,
  power, utilisation, memory and the PCIe link every 2 s (gpu.csv).

  Stall test: the GPU scene time (foveation's timer, so foveation must be on for it) is above
  8 ms and above twice the lowest of the previous six stereo windows, in two windows in a row.
  The run stops -AfterStallSeconds after that unless -NoStallStop, and after -MaxSeconds of
  gameplay in any case. Keep every game run under 15 minutes.

  Everything goes to captures\stall\<stamp>-<tag>\: run.txt (the narrative), windows.csv (one
  row per timing window), probeloop.txt, probe.txt, gpu.csv, launch.txt, stop.txt, ff7vr.log.

.EXAMPLE
  $env:FF7VR_DEV_NAME = 'me'
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\stall-run.ps1 -Tag scale `
      -BuildDir build\me -Ini my.ini -Set "log.d3d11_census=1" `
      -Seq "60:dynres scale 0.6|180:dynres scale 1.0" -MaxSeconds 260 -NoStallStop
#>
param(
    [Parameter(Mandatory)] [string]$Tag,
    [string]$BuildDir = '',            # default: build\<FF7VR_DEV_NAME>
    [string]$Ini = '',
    [string]$Set = '',
    [string[]]$Seq = @(),          # "<seconds after gameplay>:<pipe command>"
    [string[]]$OnStall = @(),      # "<seconds after the stall was detected>:<pipe command>" ; command 'probe' runs upload_probe
    [int]$MaxSeconds = 480,        # after gameplay
    [int]$AfterStallSeconds = 60,
    [switch]$NoStallStop,
    [switch]$KeepLuma,
    [string]$ProbeBuildDir = '',   # the build that holds tools\upload_probe (default: -BuildDir)
    [int]$LockWaitSeconds = 2400
)
$ErrorActionPreference = 'Stop'
$Seq = @($Seq | ForEach-Object { $_ -split '\||,(?=\d+:)' } | Where-Object { $_ })
$OnStall = @($OnStall | ForEach-Object { $_ -split '\||,(?=\d+:)' } | Where-Object { $_ })
. "$PSScriptRoot\..\dev\common.ps1"
$wt = $script:RepoRoot
$owner = Get-DefaultOwner
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$out = Join-Path $script:CapturesDir "stall\$stamp-$Tag"
New-Item -ItemType Directory -Force $out | Out-Null
$runLog = Join-Path $out 'run.txt'
function Append([string]$path, [string]$line) { for ($k = 0; $k -lt 20; $k++) { try { [System.IO.File]::AppendAllText($path, $line + "`r`n"); return } catch { Start-Sleep -Milliseconds 100 } } }
function Note([string]$m) { $l = "[{0}] {1}" -f (Get-Date).ToString('HH:mm:ss'), $m; Append $runLog $l; Write-Host $l }
$probe = Join-Path (Resolve-BuildDir $(if ($ProbeBuildDir) { $ProbeBuildDir } else { $BuildDir })) 'tools\upload_probe\upload_probe.exe'
if (-not (Test-Path -LiteralPath $probe)) { Write-Host "upload_probe not found at $probe (build the target upload_probe)"; exit 2 }
function Probe([string]$what) {
    $po = & $probe --timeout-ms 10000 --mb 32 up down 2>&1 | Out-String
    $lines = @($po -split "`n" | Where-Object { $_ -match 'GB/s|TIMEOUT' } | ForEach-Object { $_.Trim() })
    Append (Join-Path $out 'probe.txt') "--- $what`n$po"; Note "probe $what : $($lines -join ' ; ')"
}

Note "tag $Tag build $BuildDir ini $Ini set '$Set' seq [$($Seq -join ' | ')] onstall [$($OnStall -join ' | ')] max $MaxSeconds"
if (-not (Lock-Game -owner $owner -waitSeconds $LockWaitSeconds -pollSeconds 15)) { Note 'no lock'; exit 3 }
Probe 'before the start (no game)'
$runStart = Get-Date
$loopFile = Join-Path $out 'probeloop.txt'
$loop = Start-Job -ArgumentList $probe, $loopFile, ((Get-Date).AddSeconds($MaxSeconds + 400)) -ScriptBlock {
    param($probe, $file, $end)
    while ((Get-Date) -lt $end -and -not (Test-Path ($file + '.stop'))) {
        $o = & $probe --timeout-ms 5000 --mb 8 up 2>&1 | Out-String
        $m = [regex]::Match($o, 'gpu ([\d.]+) ms \(([\d.]+) GB/s\)')
        $v = if ($m.Success) { $m.Groups[2].Value } elseif ($o -match 'TIMEOUT') { 'timeout' } else { 'err' }
        for ($k = 0; $k -lt 10; $k++) { try { [System.IO.File]::AppendAllText($file, ((Get-Date).ToString('HH:mm:ss') + ' ' + $v + "`r`n")); break } catch { Start-Sleep -Milliseconds 50 } }
        Start-Sleep -Milliseconds 2500
    }
}
$smi = Start-Process -FilePath 'nvidia-smi' -ArgumentList @('--query-gpu=timestamp,pstate,clocks.sm,clocks.mem,power.draw,utilization.gpu,utilization.memory,memory.used,clocks_event_reasons.active,pcie.link.gen.current,pcie.link.width.current', '--format=csv,noheader,nounits', '-lms', '2000', '-f', (Join-Path $out 'gpu.csv')) -WindowStyle Hidden -PassThru
$stallAt = $null; $result = 'no stall'
try {
    $largs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "$wt\tools\dev\launch.ps1", '-BuildDir', (Resolve-BuildDir $BuildDir), '-Until', 'gameplay', '-KeepRunning')
    if ($Ini) { $largs += @('-Ini', $Ini) }
    if ($Set) { $largs += @('-Set', $Set) }
    if ($KeepLuma) { $largs += '-KeepLuma' }
    & powershell @largs *> (Join-Path $out 'launch.txt')
    $lc = $LASTEXITCODE
    Note "launch exit $lc"
    if ($lc -ne 0 -or @(Get-GameProcesses).Count -eq 0) { $result = "launch failed ($lc)"; throw 'launch failed' }
    $t0 = Get-Date
    try { Send-DevCommand @('mark gameplay reached (stall run)') | Out-Null } catch {}
    $pending = @($Seq | ForEach-Object { $p = $_.IndexOf(':'); [pscustomobject]@{ T = [double]$_.Substring(0, $p); C = $_.Substring($p + 1); Done = $false } })
    $pendingStall = @($OnStall | ForEach-Object { $p = $_.IndexOf(':'); [pscustomobject]@{ T = [double]$_.Substring(0, $p); C = $_.Substring($p + 1); Done = $false } })
    $logPath = Get-GameLogPath
    $seen = 0; $win = @(); $lastSys = Get-Date; $high = 0
    Append (Join-Path $out 'windows.csv') 'time,elapsed,fps,mode,scene_avg,after_avg,interval_p50,game_mb,card_pct,sys_mb,copy_pct'
    while ($true) {
      try {
        Start-Sleep -Milliseconds 1000
        $el = ((Get-Date) - $t0).TotalSeconds
        if (@(Get-GameProcesses).Count -eq 0) { $result = 'game exited'; Note 'game process gone'; break }
        foreach ($p in $pending) {
            if (-not $p.Done -and $el -ge $p.T) {
                $p.Done = $true
                if ($p.C -like 'probe*') { Probe "in game at $([int]$el) s" }
                else { try { $r = Send-DevCommand @("mark seq: $($p.C)", $p.C) 20000; Note ("cmd '{0}' at {1:N0} s -> {2}" -f $p.C, $el, ($r -join ' / ')) } catch { Note "cmd '$($p.C)' failed: $_" } }
            }
        }
        if ($stallAt) {
            $es = ((Get-Date) - $stallAt).TotalSeconds
            foreach ($p in $pendingStall) {
                if (-not $p.Done -and $es -ge $p.T) {
                    $p.Done = $true
                    if ($p.C -like 'probe*') {
                        Probe "in state at +$([int]$es) s"
                    } else {
                        try { $r = Send-DevCommand @("mark onstall: $($p.C)", $p.C) 30000; Note ("onstall cmd '{0}' at +{1:N0} s -> {2}" -f $p.C, $es, ($r -join ' / ')) } catch { Note "onstall cmd '$($p.C)' failed: $_" }
                    }
                }
            }
        }
        # parse new timing windows
        $text = Read-SharedText $logPath
        $ms = [regex]::Matches($text, '(?m)^\S+ (\d\d:\d\d:\d\d)\.\d+ \[tid \d+\] INFO  timing: last [\d.]+ s, ([\d.]+) fps; [^;]*; mode (\S+);')
        for ($i = $seen; $i -lt $ms.Count; $i++) {
            $m = $ms[$i]
            $start = $m.Index
            $end = if ($i + 1 -lt $ms.Count) { $ms[$i + 1].Index } else { $text.Length }
            if ($i + 1 -ge $ms.Count -and -not ($text.Substring($start, $end - $start) -match 'video memory:')) { break }
            $blk = $text.Substring($start, $end - $start)
            $scene = if ($blk -match 'gpu scene \(foveation window\): avg ([\d.]+)') { [double]$Matches[1] } else { -1 }
            $after = if ($blk -match 'gpu after the scene until Present: avg ([\d.]+)') { [double]$Matches[1] } else { -1 }
            $p50 = if ($blk -match 'game frame interval: avg [\d.]+ p50 ([\d.]+)') { [double]$Matches[1] } else { -1 }
            $vm = if ($blk -match 'video memory: game (\d+) of budget \d+ MB \(\d+ %\); card \d+ of \d+ MB in use by all processes \((\d+) %\); game in system memory (\d+) MB; game copy engine (\d+) %') { @($Matches[1], $Matches[2], $Matches[3], $Matches[4]) } else { @(-1, -1, -1, -1) }
            $w = [pscustomobject]@{ Time = $m.Groups[1].Value; Fps = [double]$m.Groups[2].Value; Mode = $m.Groups[3].Value; Scene = $scene; After = $after; P50 = $p50 }
            $wel = [int](((Get-Date) - $t0).TotalSeconds)
            Append (Join-Path $out 'windows.csv') ("{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10}" -f $w.Time, $wel, $w.Fps, $w.Mode, $scene, $after, $p50, $vm[0], $vm[1], $vm[2], $vm[3])
            $win += $w
            $seen = $i + 1
            Note ("win {0} el {1} fps {2} {3} scene {4} after {5} game {6} card {7}% sys {8} ce {9}" -f $w.Time, $wel, $w.Fps, $w.Mode, $scene, $after, $vm[0], $vm[1], $vm[2], $vm[3])
            # stall test: scene time over 2.5x the lowest of the previous 6 stereo windows and over 15 ms, twice in a row
            if (-not $stallAt -and $w.Mode -eq 'stereo' -and $scene -gt 0) {
                $prev = @($win | Select-Object -SkipLast 1 | Where-Object { $_.Mode -eq 'stereo' -and $_.Scene -gt 0.5 } | Select-Object -Last 6)
                if ($prev.Count -ge 3) {
                    $base = ($prev | Measure-Object Scene -Minimum).Minimum
                    if ($scene -gt [math]::Max(8, 2.0 * $base)) { $high++ } else { $high = 0 }
                    if ($high -ge 2) { $stallAt = Get-Date; $result = ('STALL detected at {0:N0} s after gameplay (window {1}, scene {2} ms, base {3:N2})' -f $el, $w.Time, $scene, $base); Note $result }
                }
            }
        }
        if (((Get-Date) - $lastSys).TotalSeconds -ge 30) {
            $lastSys = Get-Date
            $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $runStart } -ErrorAction SilentlyContinue | Where-Object { $_.Id -in 153, 4101 -or $_.ProviderName -match 'nvlddmkm' })
            if ($ev.Count -gt 0) { $result = 'DRIVER EVENT: ' + (($ev | ForEach-Object { "$($_.TimeCreated) $($_.ProviderName) $($_.Id)" }) -join '; '); Note $result; break }
        }
        if ($el -ge $MaxSeconds) { Note 'max time reached'; break }
        if ($stallAt -and -not $NoStallStop) {
            $es = ((Get-Date) - $stallAt).TotalSeconds
            $left = @($pendingStall | Where-Object { -not $_.Done }).Count
            if ($es -ge $AfterStallSeconds -and $left -eq 0) { Note 'after-stall time done'; break }
        }
      } catch { Note "loop error: $_" }
    }
} catch {
    Note "error: $_"
} finally {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$wt\tools\dev\stop.ps1" *> (Join-Path $out 'stop.txt')
    Note "stop exit $LASTEXITCODE"
    Start-Sleep -Seconds 3
    Probe 'after exit +3 s'
    Start-Sleep -Seconds 12
    [System.IO.File]::WriteAllText($loopFile + '.stop', 'stop')
    Wait-Job $loop -Timeout 20 | Out-Null; Remove-Job $loop -Force -ErrorAction SilentlyContinue
    Note ('probe loop: ' + ((Get-Content $loopFile -ErrorAction SilentlyContinue | ForEach-Object { $_ }) -join ' | '))
    if ($smi -and -not $smi.HasExited) { Stop-Process -Id $smi.Id -Force -ErrorAction SilentlyContinue }
    $runsDir = Get-ChildItem (Join-Path $script:CapturesDir 'runs') -Directory | Where-Object { $_.LastWriteTime -ge $runStart -and (Test-Path (Join-Path $_.FullName 'ff7vr.log')) } | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($runsDir) { Copy-Item (Join-Path $runsDir.FullName 'ff7vr.log') (Join-Path $out 'ff7vr.log'); Note "log from $($runsDir.FullName)" }
    Note "RESULT: $result"
    $left = @(Get-GameProcesses).Count
    Note "game processes left: $left; lock: $(Test-Path -LiteralPath $script:GameLock)"
}
