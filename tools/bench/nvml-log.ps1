<#
.SYNOPSIS
  Log the NVIDIA card's state every few hundred milliseconds through NVML (nvml.dll, installed
  with the NVIDIA driver; no elevation needed).

.DESCRIPTION
  One CSV row per sample, written and flushed at once (nvidia-smi's own -f file is buffered and
  loses its tail when the logger is stopped):
    time        local time, ISO 8601
    pstate      performance state (0 = P0, full clocks; 8 = P8, idle)
    gen, width  current PCIe link generation and width
    mem_mb      memory used on the card (all processes)
    watts       board power
    util_gpu, util_mem   NVML utilisation percentages (time busy, not work done)
    clk_sm, clk_mem      current clocks in MHz
    pcie_tx_kbs, pcie_rx_kbs   PCIe throughput over a 20 ms window (KB/s; rx = towards the card)
    replay      PCIe replay counter (link-level retransmissions since boot)
    reasons     clock event reasons (hex bit mask, NVML's nvmlClocksEventReason*)
  Stops after -Seconds (0 = until killed).

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\nvml-log.ps1 -OutCsv C:\temp\nvml.csv -IntervalMs 200
#>
param(
    [Parameter(Mandatory = $true)][string]$OutCsv,
    [int]$IntervalMs = 200,
    [int]$Seconds = 0,
    [int]$Index = 0,
    [switch]$NoPcieThroughput
)

$ErrorActionPreference = 'Stop'
if (-not ('Ff7vrBench.Nvml' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace Ff7vrBench {
public static class Nvml {
    [StructLayout(LayoutKind.Sequential)] public struct Util { public uint Gpu; public uint Memory; }
    [StructLayout(LayoutKind.Sequential)] public struct Mem { public ulong Total; public ulong Free; public ulong Used; }
    [DllImport("nvml.dll")] public static extern int nvmlInit_v2();
    [DllImport("nvml.dll")] public static extern int nvmlShutdown();
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetHandleByIndex_v2(uint index, out IntPtr dev);
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetPerformanceState(IntPtr dev, out int p);
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetClockInfo(IntPtr dev, int type, out uint mhz);
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetCurrPcieLinkGeneration(IntPtr dev, out uint gen);
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetCurrPcieLinkWidth(IntPtr dev, out uint width);
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetPowerUsage(IntPtr dev, out uint mw);
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetUtilizationRates(IntPtr dev, out Util u);
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetMemoryInfo(IntPtr dev, out Mem m);
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetPcieReplayCounter(IntPtr dev, out uint n);
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetPcieThroughput(IntPtr dev, int counter, out uint kbs);
    [DllImport("nvml.dll")] public static extern int nvmlDeviceGetCurrentClocksThrottleReasons(IntPtr dev, out ulong reasons);
}
}
'@
}
$N = [Ff7vrBench.Nvml]
$rc = $N::nvmlInit_v2()
if ($rc -ne 0) { throw "nvmlInit_v2 failed ($rc)" }
$dev = [IntPtr]::Zero
$rc = $N::nvmlDeviceGetHandleByIndex_v2([uint32]$Index, [ref]$dev)
if ($rc -ne 0) { throw "nvmlDeviceGetHandleByIndex_v2 failed ($rc)" }

$dir = Split-Path -Parent $OutCsv
if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
$w = New-Object System.IO.StreamWriter($OutCsv, $true, (New-Object System.Text.UTF8Encoding($false)))
$w.WriteLine('time,pstate,gen,width,mem_mb,watts,util_gpu,util_mem,clk_sm,clk_mem,pcie_tx_kbs,pcie_rx_kbs,replay,reasons')
$w.Flush()
$inv = [Globalization.CultureInfo]::InvariantCulture
$end = if ($Seconds -gt 0) { (Get-Date).AddSeconds($Seconds) } else { [datetime]::MaxValue }
try {
    while ((Get-Date) -lt $end) {
        $t = Get-Date
        $p = 0; $gen = 0; $wid = 0; $mw = 0; $sm = 0; $mc = 0; $rep = 0; $rs = [uint64]0; $tx = 0; $rx = 0
        $u = New-Object Ff7vrBench.Nvml+Util
        $m = New-Object Ff7vrBench.Nvml+Mem
        [void]$N::nvmlDeviceGetPerformanceState($dev, [ref]$p)
        [void]$N::nvmlDeviceGetCurrPcieLinkGeneration($dev, [ref]$gen)
        [void]$N::nvmlDeviceGetCurrPcieLinkWidth($dev, [ref]$wid)
        [void]$N::nvmlDeviceGetMemoryInfo($dev, [ref]$m)
        [void]$N::nvmlDeviceGetPowerUsage($dev, [ref]$mw)
        [void]$N::nvmlDeviceGetUtilizationRates($dev, [ref]$u)
        [void]$N::nvmlDeviceGetClockInfo($dev, 1, [ref]$sm)
        [void]$N::nvmlDeviceGetClockInfo($dev, 2, [ref]$mc)
        if (-not $NoPcieThroughput) {
            [void]$N::nvmlDeviceGetPcieThroughput($dev, 0, [ref]$tx)
            [void]$N::nvmlDeviceGetPcieThroughput($dev, 1, [ref]$rx)
        }
        [void]$N::nvmlDeviceGetPcieReplayCounter($dev, [ref]$rep)
        [void]$N::nvmlDeviceGetCurrentClocksThrottleReasons($dev, [ref]$rs)
        $w.WriteLine([string]::Format($inv, '{0},{1},{2},{3},{4},{5:F1},{6},{7},{8},{9},{10},{11},{12},0x{13:X}',
            $t.ToString('o'), $p, $gen, $wid, [int]($m.Used / 1MB), $mw / 1000.0, $u.Gpu, $u.Memory, $sm, $mc, $tx, $rx, $rep, $rs))
        $w.Flush()
        $left = $IntervalMs - [int]((Get-Date) - $t).TotalMilliseconds
        if ($left -gt 0) { Start-Sleep -Milliseconds $left }
    }
} finally {
    $w.Dispose()
    [void]$N::nvmlShutdown()
}
