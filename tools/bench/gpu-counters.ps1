<#
.SYNOPSIS
  Log which process keeps which GPU engine busy, and who holds video memory, once per interval.

.DESCRIPTION
  Reads the Windows performance counters that Task Manager uses (no elevation needed):
    \GPU Engine(*)\Utilization Percentage      per process and engine (3D, copy, video encode, ...)
    \GPU Process Memory(*)\Local Usage         per process, in the card's memory
    \GPU Process Memory(*)\Shared Usage        per process, in shared system memory
    \GPU Process Memory(*)\Dedicated Usage     per process, allocations whose home is the card
    \GPU Process Memory(*)\Total Committed     per process, everything committed (card and system memory)
    \GPU Adapter Memory(*)\Dedicated Usage     whole card
    \GPU Adapter Memory(*)\Shared Usage        whole card, shared system memory
  and writes one CSV row per busy engine and per process holding memory:
    time,kind,pid,process,item,value
  kind = engine (value in percent, item = engine type and index, for example copy#11),
         local / shared / dedicated / committed (MB per process), adapter (MB, item dedicated / shared).
  Process id 4 is the System process: the video memory manager's own transfers
  (moving allocations between system memory and the card) are counted there.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\gpu-counters.ps1 -OutCsv C:\temp\gpu.csv -Seconds 300
#>
param(
    [Parameter(Mandatory = $true)][string]$OutCsv,
    [int]$IntervalSeconds = 1,
    [int]$Seconds = 0,             # 0 = until stopped
    [double]$MinEnginePercent = 0.5,
    [double]$MinProcessMb = 32,
    [int]$ChunkSamples = 10
)

$ErrorActionPreference = 'Stop'
$counters = @(
    '\GPU Engine(*)\Utilization Percentage',
    '\GPU Process Memory(*)\Local Usage',
    '\GPU Process Memory(*)\Shared Usage',
    '\GPU Process Memory(*)\Dedicated Usage',
    '\GPU Process Memory(*)\Total Committed',
    '\GPU Adapter Memory(*)\Dedicated Usage',
    '\GPU Adapter Memory(*)\Shared Usage'
)
$dir = Split-Path -Parent $OutCsv
if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
$w = New-Object System.IO.StreamWriter($OutCsv, $true, (New-Object System.Text.UTF8Encoding($false)))
$w.WriteLine('time,kind,pid,process,item,value')
$w.Flush()

$inv = [Globalization.CultureInfo]::InvariantCulture
$names = @{}
function Get-Name([int]$procId) {
    if (-not $names.ContainsKey($procId)) {
        $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
        $names[$procId] = if ($procId -eq 4) { 'System' } elseif ($p) { $p.ProcessName } else { '?' }
    }
    return $names[$procId]
}

# Get-Counter expands the (*) instances once per call, so processes started later would be
# missed: it runs in chunks of $ChunkSamples samples and expands them again for each chunk.
$total = if ($Seconds -gt 0) { [Math]::Max(1, [int]($Seconds / $IntervalSeconds)) } else { [int]::MaxValue }
$done = 0
try {
  while ($done -lt $total) {
    $n = [Math]::Min($ChunkSamples, $total - $done)
    $done += $n
    Get-Counter -Counter $counters -SampleInterval $IntervalSeconds -MaxSamples $n -ErrorAction SilentlyContinue | ForEach-Object {
        $t = $_.Timestamp.ToString('HH:mm:ss.fff')
        $mem = @{}
        foreach ($s in $_.CounterSamples) {
            $path = $s.Path
            $inst = $s.InstanceName
            $v = $s.CookedValue
            if ($path -like '*\gpu engine(*)\utilization percentage') {
                if ($v -lt $MinEnginePercent) { continue }
                if ($inst -match '^pid_(\d+)_.*_eng_(\d+)_engtype_(.*)$') {
                    $procId = [int]$Matches[1]
                    $w.WriteLine([string]::Format($inv, '{0},engine,{1},{2},{3}#{4},{5:F1}', $t, $procId, (Get-Name $procId), $Matches[3].ToLowerInvariant(), $Matches[2], $v))
                }
            } elseif ($path -like '*\gpu process memory(*)\*') {
                if ($inst -match '^pid_(\d+)_') {
                    $procId = [int]$Matches[1]
                    $kind = if ($path -like '*\local usage') { 'local' } elseif ($path -like '*\shared usage') { 'shared' }
                            elseif ($path -like '*\dedicated usage') { 'dedicated' } else { 'committed' }
                    $key = "$procId|$kind"
                    $mem[$key] = ($(if ($mem.ContainsKey($key)) { $mem[$key] } else { 0 })) + $v
                }
            } elseif ($path -like '*\gpu adapter memory(*)\*') {
                if ($v -le 0) { continue }  # other adapters (integrated graphics, software adapter)
                $item = if ($path -like '*\dedicated usage') { 'dedicated' } else { 'shared' }
                $w.WriteLine([string]::Format($inv, '{0},adapter,,,{1},{2:F0}', $t, $item, ($v / 1MB)))
            }
        }
        foreach ($k in $mem.Keys) {
            $mb = $mem[$k] / 1MB
            if ($mb -lt $MinProcessMb) { continue }
            $procId, $kind = $k -split '\|'
            $w.WriteLine([string]::Format($inv, '{0},{1},{2},{3},,{4:F0}', $t, $kind, $procId, (Get-Name ([int]$procId)), $mb))
        }
        $w.Flush()
    }
  }
} finally {
    $w.Dispose()
}
