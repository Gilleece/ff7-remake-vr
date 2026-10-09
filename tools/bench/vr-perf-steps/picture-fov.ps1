# Picture quality, part 3 (third person, the view where the latest save starts): the same
# start as picture-aniso.ps1 and picture-vignette.ps1 (so a session of those with a start-up
# setting compares with this one), temporal AA samples, a one-frame GPU trace with read-backs
# of the full-screen passes (what the tonemapper does to the picture), translucency at full
# rate under foveated rendering (fov translucency; traces of the scene's render target
# bindings with and without it), texture streaming mip bias and the video memory reading.
# Captures and A/B frame times.
. "$PSScriptRoot\picture-lib.ps1"
Start-Sleep 3
P 'cvar get r.MaxAnisotropy' | Out-Null
P 'fp third' | Out-Null
Start-Sleep 4
Measure-Perf 'start' 30 | Out-Null
Capture-View 'start' -Turning
Measure-Perf 'start2' 30 | Out-Null
# Temporal AA samples again, with whatever [picture] sharpen the session runs with.
foreach ($n in @('16', '4')) {
    Set-Cvars "r.TemporalAASamples $n" 2500
    Capture-View "taa$n" -Turning
}
Set-Cvars 'r.TemporalAASamples 8' 2500
Capture-View 'taa8' -Turning
P 'fov status;vram' | Out-Null
P 'gpu names on' | Out-Null
P ("gpu trace $out\trace dump fullscreen scale 4") | Out-Null
Start-Sleep 3
P 'mark trace-default;fov trace' | Out-Null
Start-Sleep 2
P 'fov translucency 1' | Out-Null
Start-Sleep 1
P 'mark trace-translucency;fov trace' | Out-Null
Start-Sleep 2
Capture-View 'transl1'
P 'fov translucency 0' | Out-Null
Start-Sleep 2
Capture-View 'base'
AB-Commands 'transl' 'fov translucency 0' 'fov translucency 1'
P 'fov off' | Out-Null
Start-Sleep 2
Capture-View 'fovoff'
P 'fov on' | Out-Null
Start-Sleep 2
foreach ($b in @('-0.5', '-1')) {
    Set-Cvars "r.Streaming.MipBias $b" 8000
    Capture-View "mipbias$b"
    P 'vram' | Out-Null
}
Set-Cvars 'r.Streaming.MipBias 0' 8000
Capture-View 'base2'
P 'vram' | Out-Null
AB-Cvars 'mipbias' 'r.Streaming.MipBias 0' 'r.Streaming.MipBias -1'
P 'fp first' | Out-Null
