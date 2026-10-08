# The [graphics] profiles applied live (graphics profile <name>), standing, one capture each.
# "custom" is the ini as it is (no profile at start-up).
# The order quality, balanced, performance, balanced, quality also shows the drift
# and that a switch puts back what the previous profile held.
Start-Sleep 3
P 'graphics status;fov status;dynres' | Out-Null
Measure-Perf 'custom' 5 | Out-Null
P ("capture $out\custom") | Out-Null
foreach ($p in @('quality', 'balanced', 'performance', 'balanced', 'quality')) {
    P "graphics profile $p" | Out-Null
    Start-Sleep 3
    P 'cvar get r.VolumetricFog;cvar get r.Shadow.CSM.MaxCascades;cvar get r.StaticMeshLODDistanceScale;fov status;dynres' | Out-Null
    Measure-Perf "profile-$p" 5 | Out-Null
    P ("capture $out\$p") | Out-Null
}
