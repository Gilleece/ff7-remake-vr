# Picture quality, part 2 (third person, the view where the latest save starts): temporal AA
# samples, the engine's tonemapper sharpening, full-resolution subsurface scattering, the
# tonemapper quality levels (vignette, grain), shadow distance and resolution. A still
# capture of each (and one while the emulated head turns for the anti-aliasing settings),
# then A/B frame times.
. "$PSScriptRoot\picture-lib.ps1"
Start-Sleep 3
P 'fp third' | Out-Null
Start-Sleep 4
P 'gpu names on' | Out-Null
P ("gpu trace $out\trace") | Out-Null
Start-Sleep 2
Capture-View 'base' -Turning
$taa = [ordered]@{
    's16'       = @('r.TemporalAASamples 16', 'r.TemporalAASamples 8')
    's4'        = @('r.TemporalAASamples 4', 'r.TemporalAASamples 8')
    'sharpen05' = @('r.Tonemapper.Sharpen 0.5', 'r.Tonemapper.Sharpen 0')
    'sharpen10' = @('r.Tonemapper.Sharpen 1', 'r.Tonemapper.Sharpen 0')
}
foreach ($k in $taa.Keys) {
    Set-Cvars $taa[$k][0] 2500
    Capture-View $k -Turning
    Set-Cvars $taa[$k][1] 1500
}
$tests = [ordered]@{
    'sssfull'   = @('r.SSS.HalfRes 0', 'r.SSS.HalfRes 1')
    'sssq1'     = @('r.SSS.Quality 1', 'r.SSS.Quality 0')
    'tmq4'      = @('r.Tonemapper.Quality 4', 'r.Tonemapper.Quality 5')
    'tmq3'      = @('r.Tonemapper.Quality 3', 'r.Tonemapper.Quality 5')
    'tmq2'      = @('r.Tonemapper.Quality 2', 'r.Tonemapper.Quality 5')
    'tmq1'      = @('r.Tonemapper.Quality 1', 'r.Tonemapper.Quality 5')
    'shdist15'  = @('r.Shadow.DistanceScale 1.5', 'r.Shadow.DistanceScale 1')
    'shtexel2'  = @('r.Shadow.TexelsPerPixel 2', 'r.Shadow.TexelsPerPixel 1.27324')
    'shradius'  = @('r.Shadow.RadiusThreshold 0.003', 'r.Shadow.RadiusThreshold 0.01')
    'shtrans2'  = @('r.Shadow.CSM.TransitionScale 2', 'r.Shadow.CSM.TransitionScale 1')
    'pcss'      = @('r.Shadow.FilterMethod 1', 'r.Shadow.FilterMethod 0')
}
foreach ($k in $tests.Keys) {
    Set-Cvars $tests[$k][0] 2500
    Capture-View $k
    Set-Cvars $tests[$k][1] 1500
}
Capture-View 'base2'
AB-Cvars 'sharpen10' 'r.Tonemapper.Sharpen 0' 'r.Tonemapper.Sharpen 1'
AB-Cvars 'sssfull' 'r.SSS.HalfRes 1;r.SSS.Quality 0' 'r.SSS.HalfRes 0;r.SSS.Quality 1'
AB-Cvars 'tmq1' 'r.Tonemapper.Quality 5' 'r.Tonemapper.Quality 1'
AB-Cvars 'shadows' 'r.Shadow.DistanceScale 1;r.Shadow.TexelsPerPixel 1.27324;r.Shadow.RadiusThreshold 0.01' 'r.Shadow.DistanceScale 1.5;r.Shadow.TexelsPerPixel 2;r.Shadow.RadiusThreshold 0.003'
P 'fp first' | Out-Null
