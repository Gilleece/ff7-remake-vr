# Turning in the street (after walking out of the start of the latest save): frame logs
# (`stereo framelog`, thread cycle counts) for the defaults, r.HZBOcclusion 1, the per-eye fixes
# off and foveation off, each a left turn then a right turn of 5 s (the same path every time), two
# rounds; then a check of hzb_skip's guard on r.HZBOcclusion. docs/engine-module.md, "Turning:
# where the slow frames come from". fl_*.csv columns: point, tid, qpc, cpu_ms, gpu_ms, gpu_samples.
function Turn([string]$label) {
    P 'stereo framelog start' | Out-Null
    $null = Send-DevCommand @('fov timing')
    $null = Send-DevCommand @("mark perf-$label", 'stereo frametime 10')
    $pan = Start-Pan -6
    Start-Sleep -Milliseconds 5000
    $null = $pan.Stop()
    $pan = Start-Pan 6
    Start-Sleep -Milliseconds 5000
    $null = $pan.Stop()
    Start-Sleep -Milliseconds 900
    P "stereo framelog stop $out\fl_$label.csv" | Out-Null
    $fov = @(Send-DevCommand @('fov timing'))[0]
    $pattern = 'frame time: (\d+) frames, avg ([\d.]+) ms, median ([\d.]+), p95 ([\d.]+), max ([\d.]+)'
    $ft = Get-LogAfterMark "perf-$label" | Where-Object { $_ -match $pattern } | Select-Object -First 1
    Write-Host "RESULT $label $ft | $fov"
}
Start-Sleep 3
Send-Walk 3500
Start-Sleep 2
$pan = Start-Pan 6
Start-Sleep -Milliseconds 5000
$null = $pan.Stop()
Start-Sleep 2
P "capture $out\street" | Out-Null
Measure-Perf 'still' 5 | Out-Null
P 'cvar get r.HZBOcclusion;cvar get r.AllowOcclusionQueries;cvar get r.Streaming.PoolSize;cvar get r.TextureStreaming' | Out-Null
P 'stereo framelog start' | Out-Null
Start-Sleep 5
P "stereo framelog stop $out\fl_still.csv" | Out-Null
foreach ($round in 1..2) {
    Turn "base-$round"
    P 'cvar set r.HZBOcclusion 1' | Out-Null; Start-Sleep 1
    Turn "hzbocc-$round"
    P 'cvar set r.HZBOcclusion 0' | Out-Null; Start-Sleep 1
    P 'ssr off;ssr fix 0;aofix 0;stereo lightfix 0' | Out-Null; Start-Sleep 1
    Turn "modfixesoff-$round"
    P 'ssr on;ssr fix 1;aofix 1;stereo lightfix 1' | Out-Null; Start-Sleep 1
    P 'fov off' | Out-Null; Start-Sleep 1
    Turn "fovoff-$round"
    P 'fov on' | Out-Null; Start-Sleep 1
}
P 'stereo framelog start' | Out-Null
Start-Sleep 5
P "stereo framelog stop $out\fl_still2.csv" | Out-Null
P 'status' | Out-File "$out\threads.txt"
# The HZB option and its guard on r.HZBOcclusion.
P 'hzb 1' | Out-Null; Start-Sleep 5
P 'hzb' | Out-Null
P 'cvar set r.HZBOcclusion 1' | Out-Null; Start-Sleep 5
P 'hzb' | Out-Null; Start-Sleep 2
P 'hzb' | Out-Null
P 'cvar set r.HZBOcclusion 0' | Out-Null; Start-Sleep 5
P 'hzb' | Out-Null; Start-Sleep 2
P 'hzb;hzb 0' | Out-Null

