# Three views from the start of the latest save (where the save starts; an alley after
# walking forward for 3.5 s; an open street after turning right for 5 s), each with the
# per-eye reflections off and on in turns (before / after), then turning in both settings.
# Where the walk ends depends on the save; check the captures.
function AB([string]$scene, [int]$rounds = 2) {
    foreach ($round in 1..$rounds) {
        P 'ssr off' | Out-Null; Start-Sleep 1
        Measure-Perf "$scene-before-$round" 5 | Out-Null
        P 'ssr on' | Out-Null; Start-Sleep 1
        Measure-Perf "$scene-after-$round" 5 | Out-Null
    }
}
Start-Sleep 3
AB 'start'
P ("capture $out\start") | Out-Null
Send-Walk 3500
Start-Sleep 2
AB 'alley'
P ("capture $out\alley") | Out-Null
$pan = Start-Pan 6
Start-Sleep -Milliseconds 5000
$null = $pan.Stop()
Start-Sleep 2
AB 'street'
P ("capture $out\street") | Out-Null
foreach ($round in 1..2) {
    P 'ssr off' | Out-Null
    $pan = Start-Pan -6
    Measure-Perf "turn-before-$round" 5 | Out-Null
    $null = $pan.Stop()
    P 'ssr on' | Out-Null
    $pan = Start-Pan 6
    Measure-Perf "turn-after-$round" 5 | Out-Null
    $null = $pan.Stop()
}
P 'ssr;dynres;status' | Out-Null
