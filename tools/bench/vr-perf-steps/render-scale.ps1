# Fixed render scales, then the dynamic resolution at two targets, standing and turning.
Start-Sleep 3
Measure-Perf 'scale-1.00' 5 | Out-Null
foreach ($s in @('0.9', '0.8', '0.75')) {
    P "dynres scale $s" | Out-Null
    Start-Sleep 1
    Measure-Perf "scale-$s" 5 | Out-Null
}
P ("capture $out\scale075") | Out-Null
P 'dynres scale 1' | Out-Null
Start-Sleep 1
P 'dynres on' | Out-Null
Start-Sleep 3
Measure-Perf 'dyn-target0.85' 5 | Out-Null
P 'dynres' | Out-Null
P 'dynres target 0.70' | Out-Null
Start-Sleep 4
Measure-Perf 'dyn-target0.70' 5 | Out-Null
P 'dynres' | Out-Null
$pan = Start-Pan 6
Measure-Perf 'dyn-target0.70-turn' 5 | Out-Null
$null = $pan.Stop()
P 'dynres' | Out-Null
Start-Sleep 2
Measure-Perf 'dyn-target0.70-after-turn' 5 | Out-Null
P 'dynres' | Out-Null
P 'dynres off' | Out-Null
