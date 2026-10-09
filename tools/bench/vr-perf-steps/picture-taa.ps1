# Picture quality, part 1: the game's values of the candidate console variables, then
# temporal AA and anisotropic filtering in third person (Cloud's hair close, the street and
# its signs far): captures of each setting standing and while turning, and A/B frame times.
. "$PSScriptRoot\picture-lib.ps1"
Start-Sleep 3
Save-CvarSurvey @(
    'r.PostProcessAAQuality', 'r.TemporalAACurrentFrameWeight', 'r.TemporalAACatmullRom', 'r.TemporalAASamples',
    'r.TemporalAAFilterSize', 'r.TemporalAAPauseCorrect', 'r.Tonemapper.Sharpen', 'r.MaxAnisotropy',
    'r.DepthOfFieldQuality', 'r.DepthOfField.MaxSize', 'r.Tonemapper.Quality', 'r.Tonemapper.GrainQuantization',
    'r.DefaultFeature.AntiAliasing', 'r.DefaultFeature.AutoExposure', 'r.DefaultFeature.MotionBlur',
    'r.Shadow.MaxResolution', 'r.Shadow.MaxCSMResolution', 'r.Shadow.RadiusThreshold', 'r.Shadow.DistanceScale',
    'r.Shadow.CSM.TransitionScale', 'r.Shadow.CSM.MaxCascades', 'r.Shadow.FilterMethod', 'r.Shadow.PerObject',
    'r.Shadow.MinResolution', 'r.Shadow.TexelsPerPixel', 'r.Shadow.MaxNumPointShadowCacheUpdatesPerFrame',
    'r.Streaming.MipBias', 'r.Streaming.PoolSize', 'r.Streaming.LimitPoolSizeToVRAM', 'r.Streaming.MaxEffectiveScreenSize',
    'r.Streaming.Boost', 'r.Streaming.UseFixedPoolSize', 'r.MipMapLODBias',
    'r.SSS.Quality', 'r.SSS.Scale', 'r.SSS.SampleSet', 'r.SSS.HalfRes', 'r.LightShaftQuality', 'r.Upscale.Quality',
    'r.Upscale.Softness', 'r.ScreenPercentage', 'r.SSR.Quality', 'r.AmbientOcclusionLevels', 'r.AmbientOcclusionMaxQuality',
    'r.EyeAdaptationQuality', 'r.DetailMode', 'r.MaterialQualityLevel', 'r.ContactShadows', 'r.CapsuleShadows',
    'r.RefractionQuality', 'r.SceneColorFormat', 'r.TranslucencyVolumeBlur', 'r.ParticleLightQuality', 'r.LensFlareQuality',
    'r.BloomQuality', 'r.MotionBlurQuality', 'r.SceneColorFringeQuality', 'r.VolumetricFog', 'r.VolumetricFog.GridPixelSize',
    'r.VolumetricFog.GridSizeZ', 'r.SeparateTranslucency', 'r.SkeletalMeshLODRadiusScale', 'r.ViewDistanceScale',
    'sg.ResolutionQuality', 'sg.ViewDistanceQuality', 'sg.AntiAliasingQuality', 'sg.ShadowQuality', 'sg.PostProcessQuality',
    'sg.TextureQuality', 'sg.EffectsQuality', 'sg.FoliageQuality') "$out\cvars.txt"

$cfw = Get-CvarValue 'r.TemporalAACurrentFrameWeight'
$cr = Get-CvarValue 'r.TemporalAACatmullRom'
$smp = Get-CvarValue 'r.TemporalAASamples'
$fs = Get-CvarValue 'r.TemporalAAFilterSize'
$aniso = Get-CvarValue 'r.MaxAnisotropy'
Write-Host "game values: weight $cfw catmullrom $cr samples $smp filtersize $fs aniso $aniso"
$taaBase = "r.TemporalAACurrentFrameWeight $cfw;r.TemporalAACatmullRom $cr;r.TemporalAASamples $smp;r.TemporalAAFilterSize $fs"

P 'fp third' | Out-Null
Start-Sleep 4
P 'fp status' | Out-Null

Capture-View 'base' -Turning
$cands = [ordered]@{
    'w010'   = 'r.TemporalAACurrentFrameWeight 0.1'
    'w020'   = 'r.TemporalAACurrentFrameWeight 0.2'
    'cr1'    = 'r.TemporalAACatmullRom 1'
    's4'     = 'r.TemporalAASamples 4'
    's16'    = 'r.TemporalAASamples 16'
    'fs05'   = 'r.TemporalAAFilterSize 0.5'
    'cr1w010' = 'r.TemporalAACatmullRom 1;r.TemporalAACurrentFrameWeight 0.1'
}
foreach ($k in $cands.Keys) {
    Set-Cvars $cands[$k]
    Capture-View $k -Turning
    Set-Cvars $taaBase 1500
}
Capture-View 'base2' -Turning
if ($aniso) {
    Set-Cvars 'r.MaxAnisotropy 16' 2500
    Capture-View 'aniso16'
    Set-Cvars "r.MaxAnisotropy $aniso" 2500
    Capture-View 'base3'
}
# Frame times (15 s windows alternating with the game's values).
AB-Cvars 'cr1' $taaBase 'r.TemporalAACatmullRom 1'
AB-Cvars 'w020' $taaBase 'r.TemporalAACurrentFrameWeight 0.2'
if ($aniso) { AB-Cvars 'aniso16' "r.MaxAnisotropy $aniso" 'r.MaxAnisotropy 16' }
P 'fp first' | Out-Null
