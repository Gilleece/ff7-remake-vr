<#
.SYNOPSIS
  Per-run figures for a restart-cycle.ps1 series, from its runs.csv, smi.csv and gpu.csv.

.DESCRIPTION
  For every run (writes analysis.csv next to runs.csv and prints a table):
    copy_game     the game's copy engines, summed, averaged over the measured window (%)
    copy_system   the System process's copy engines (Windows' video memory manager), same window (%)
    d3_game       the game's 3D engine, same window (%)
    watts         the card's average power over the window (smi.csv)
    state_start   P-state, PCIe generation and memory clock at the game's start (smi.csv)
    clk_before    the last change of the card's memory clock (a P-state change) before the start
    clk_after     the first change after the start (the card waking up for the game)
    replays_start PCIe replays (link-level retransmissions) from the start to 20 s after it
    replays_run   PCIe replays from 20 s after the start to the end of the window
    pcie_rx_mbs   average PCIe traffic towards the card over the window (MB/s)
    card_mb_*     the whole card's used memory (gpu.csv, adapter dedicated): in the last second
                  before the kill, 2 s and 5 s after the exit, and right before the next start
    mem_gone_s    seconds after the exit until the killed game's process no longer appears with
                  memory in gpu.csv (the counters drop the process once its memory is released)
    procs_gap     processes holding 256 MB or more of the card's memory just before the next start

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\restart-analyze.ps1 -Dir captures\restart\20261009-222000-base
#>
param([Parameter(Mandatory = $true)][string]$Dir)

$ErrorActionPreference = 'Stop'
$inv = [Globalization.CultureInfo]::InvariantCulture
$runs = @(Import-Csv -LiteralPath (Join-Path $Dir 'runs.csv'))
if (-not $runs.Count) { throw 'runs.csv has no runs' }
$day0 = [datetime]::Parse($runs[0].window_start, $inv, [Globalization.DateTimeStyles]::RoundtripKind).Date

# gpu.csv holds times of day only: rebuild full times, counting midnight crossings.
$rows = New-Object System.Collections.Generic.List[object]
$lastT = $null; $day = $day0
foreach ($line in [System.IO.File]::ReadLines((Join-Path $Dir 'gpu.csv'))) {
    if ($line.StartsWith('time,')) { continue }
    $f = $line.Split(',')
    if ($f.Count -lt 6) { continue }
    $tod = [TimeSpan]::Parse($f[0], $inv)
    $t = $day + $tod
    if ($lastT -and $t -lt $lastT.AddHours(-1)) { $day = $day.AddDays(1); $t = $day + $tod }
    $lastT = $t
    $rows.Add([pscustomobject]@{ T = $t; Kind = $f[1]; Pid = $f[2]; Proc = $f[3]; Item = $f[4]; V = [double]::Parse($f[5], $inv) })
}
$smi = New-Object System.Collections.Generic.List[object]
foreach ($r in @(Import-Csv -LiteralPath (Join-Path $Dir 'nvml.csv'))) {
    try {
        $smi.Add([pscustomobject]@{
            T = [datetime]::Parse($r.time, $inv, [Globalization.DateTimeStyles]::RoundtripKind)
            P = 'P' + $r.pstate; Gen = $r.gen; MemClk = $r.clk_mem; Replay = [int64]$r.replay
            W = [double]::Parse($r.watts, $inv); Rx = [double]$r.pcie_rx_kbs
        })
    } catch { }
}

function Avg-Engine($from, $to, $procId, $type) {
    $sel = @($rows | Where-Object { $_.Kind -eq 'engine' -and $_.T -ge $from -and $_.T -le $to -and $_.Pid -eq $procId -and $_.Item -like "$type#*" })
    $ticks = @($rows | Where-Object { $_.Kind -eq 'adapter' -and $_.Item -eq 'dedicated' -and $_.T -ge $from -and $_.T -le $to }).Count
    if ($ticks -eq 0) { return -1 }
    return (($sel | Measure-Object V -Sum).Sum) / $ticks
}
function Card-Mb($at, [double]$tolerance = 1.5) {
    $c = @($rows | Where-Object { $_.Kind -eq 'adapter' -and $_.Item -eq 'dedicated' -and [Math]::Abs(($_.T - $at).TotalSeconds) -le $tolerance } | Sort-Object { [Math]::Abs(($_.T - $at).TotalSeconds) })
    if ($c.Count) { return $c[0].V } else { return -1 }
}

$out = @()
for ($i = 0; $i -lt $runs.Count; $i++) {
    $r = $runs[$i]
    if (-not $r.start) { continue }
    $start = [datetime]::Parse($r.start, $inv, [Globalization.DateTimeStyles]::RoundtripKind)
    $w0 = [datetime]::Parse($r.window_start, $inv, [Globalization.DateTimeStyles]::RoundtripKind)
    $w1 = [datetime]::Parse($r.window_end, $inv, [Globalization.DateTimeStyles]::RoundtripKind)
    $exit = [datetime]::Parse($r.exit, $inv, [Globalization.DateTimeStyles]::RoundtripKind)
    $next = if ($i + 1 -lt $runs.Count -and $runs[$i + 1].start) { [datetime]::Parse($runs[$i + 1].start, $inv, [Globalization.DateTimeStyles]::RoundtripKind) } else { $exit.AddSeconds(30) }
    $ws = @($smi | Where-Object { $_.T -ge $w0 -and $_.T -le $w1 })
    $s0 = @($smi | Where-Object { $_.T -le $start.AddSeconds(0.5) } | Select-Object -Last 1)
    # The last change of the memory clock (a P-state change) before the start, and the first after it.
    $pre = @($smi | Where-Object { $_.T -le $start -and $_.T -ge $start.AddSeconds(-120) })
    $chg = ''
    for ($k = $pre.Count - 1; $k -ge 1; $k--) {
        if ($pre[$k].MemClk -ne $pre[$k - 1].MemClk) { $chg = '{0:N1} s before: {1} -> {2}' -f ($start - $pre[$k].T).TotalSeconds, $pre[$k - 1].MemClk, $pre[$k].MemClk; break }
    }
    $post = @($smi | Where-Object { $_.T -gt $start -and $_.T -le $start.AddSeconds(60) })
    $up = ''
    $lastClk = if ($pre.Count) { $pre[-1].MemClk } else { '' }
    foreach ($s in $post) { if ($s.MemClk -ne $lastClk) { $up = '{0:N1} s after: {1} -> {2}' -f ($s.T - $start).TotalSeconds, $lastClk, $s.MemClk; break } }
    $rpS0 = @($smi | Where-Object { $_.T -le $start } | Select-Object -Last 1)
    $rpS1 = @($smi | Where-Object { $_.T -le $start.AddSeconds(20) } | Select-Object -Last 1)
    $rpW1 = @($smi | Where-Object { $_.T -le $w1 } | Select-Object -Last 1)
    $replStart = if ($rpS0.Count -and $rpS1.Count) { $rpS1[0].Replay - $rpS0[0].Replay } else { -1 }
    $replRun = if ($rpS1.Count -and $rpW1.Count) { $rpW1[0].Replay - $rpS1[0].Replay } else { -1 }
    $memRows = @($rows | Where-Object { $_.Pid -eq $r.pid -and $_.Kind -ne 'engine' -and $_.T -ge $exit.AddSeconds(-3) })
    $lastSeen = if ($memRows.Count) { ($memRows | Sort-Object T | Select-Object -Last 1).T } else { $null }
    $gone = if ($lastSeen) { [Math]::Round(($lastSeen - $exit).TotalSeconds, 1) } else { 'none' }
    $before = $next.AddSeconds(-1.5)
    $holders = @($rows | Where-Object { $_.Kind -eq 'dedicated' -and $_.V -ge 256 -and [Math]::Abs(($_.T - $before).TotalSeconds) -le 1.0 } |
                 ForEach-Object { '{0}={1}' -f $_.Proc, [int]$_.V } | Sort-Object -Unique)
    $out += [pscustomobject]@{
        run          = $r.name
        gap_s        = $r.gap_actual_s
        slow         = $r.slow
        p50_ms       = $r.median_p50_after30
        watts        = if ($ws.Count) { [Math]::Round(($ws | Measure-Object W -Average).Average) } else { -1 }
        copy_game    = [Math]::Round((Avg-Engine $w0 $w1 $r.pid 'copy'), 1)
        copy_system  = [Math]::Round((Avg-Engine $w0 $w1 '4' 'copy'), 1)
        d3_game      = [Math]::Round((Avg-Engine $w0 $w1 $r.pid '3d'), 1)
        probe_gbs    = $r.probe_in_run_gbs
        state_start  = if ($s0.Count) { '{0} gen{1} {2}' -f $s0[0].P, $s0[0].Gen, $s0[0].MemClk } else { '' }
        clk_before   = $chg
        clk_after    = $up
        replays_start = $replStart
        replays_run  = $replRun
        pcie_rx_mbs  = if ($ws.Count) { [Math]::Round(($ws | Measure-Object Rx -Average).Average / 1024) } else { -1 }
        card_mb_kill = Card-Mb $exit.AddSeconds(-1.5)
        card_mb_2s   = Card-Mb $exit.AddSeconds(2)
        card_mb_5s   = Card-Mb $exit.AddSeconds(5)
        card_mb_next = Card-Mb $before
        mem_gone_s   = $gone
        procs_gap    = ($holders -join ' ')
    }
}
$out | Export-Csv -LiteralPath (Join-Path $Dir 'analysis.csv') -NoTypeInformation -Encoding UTF8
$out | ConvertTo-Csv -NoTypeInformation
