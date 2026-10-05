# Benchmark configurations for foveated rendering in stereo (docs/render.md,
# "Foveated rendering"). Use with -ConfigFile:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\bench-compare.ps1 `
#       -ConfigFile tools\bench\configs-foveation.psd1 -BuildDir build\<name> -Runs 3 `
#       -Configs fov-2496-off,fov-2496-balanced
#
# fov-<size>-<preset>: the mod's stereo mode through the XR layer's Null
# backend (no runtime, no pacing: nothing limits the frame rate), at a set
# per-eye size with the Null backend's Quest 3 class asymmetric FOV and its
# emulated hidden area: the rendering cost alone.
# fov-svr-<size>-<preset>: the same through SteamVR's null driver at that
# per-eye size, the way the UEVR configurations run (symmetric FOV, no hidden
# area mesh, SteamVR's compositor on the same GPU). The game's own frame cap is
# lifted with t.MaxFPS 0. Only foveation differs between the configurations of
# one size. The Null ones are Kind 'flat' because no SteamVR is involved; for
# both kinds the size rendered is the eye size below (logged as "stereo: rendering STEREO ... eye WxH" in ff7vr.log),
# not the window size the result's renderedPixels is computed from.
#
# The game reaches gameplay in mono (the harness recognises the title screen
# from the window); stereo is switched on through the dev pipe (`stereo on`)
# once the run's warm-up starts, which restarts the warm-up after the short
# hitch of the switch.
@{
    Configs = @{
        'fov-2496-off' = @{
            Description = 'ff7vr stereo, Null backend unpaced, 2496x2592 per eye, foveation off'
            Kind = 'flat'
            Width = 1280; Height = 720
            Set = @('stereo.start_in_stereo=0', 'xr.backend=null', 'xr.null_pace=0', 'xr.eye_width=2496', 'xr.eye_height=2592',
                    'foveation.enabled=0')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-2496-quality' = @{
            Description = 'ff7vr stereo, Null backend unpaced, 2496x2592 per eye, foveation preset quality'
            Kind = 'flat'
            Width = 1280; Height = 720
            Set = @('stereo.start_in_stereo=0', 'xr.backend=null', 'xr.null_pace=0', 'xr.eye_width=2496', 'xr.eye_height=2592',
                    'foveation.enabled=1', 'foveation.preset=quality')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-2496-balanced' = @{
            Description = 'ff7vr stereo, Null backend unpaced, 2496x2592 per eye, foveation preset balanced'
            Kind = 'flat'
            Width = 1280; Height = 720
            Set = @('stereo.start_in_stereo=0', 'xr.backend=null', 'xr.null_pace=0', 'xr.eye_width=2496', 'xr.eye_height=2592',
                    'foveation.enabled=1', 'foveation.preset=balanced')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-2496-performance' = @{
            Description = 'ff7vr stereo, Null backend unpaced, 2496x2592 per eye, foveation preset performance'
            Kind = 'flat'
            Width = 1280; Height = 720
            Set = @('stereo.start_in_stereo=0', 'xr.backend=null', 'xr.null_pace=0', 'xr.eye_width=2496', 'xr.eye_height=2592',
                    'foveation.enabled=1', 'foveation.preset=performance')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-3072-off' = @{
            Description = 'ff7vr stereo, Null backend unpaced, 3072x3216 per eye, foveation off'
            Kind = 'flat'
            Width = 1280; Height = 720
            Set = @('stereo.start_in_stereo=0', 'xr.backend=null', 'xr.null_pace=0', 'xr.eye_width=3072', 'xr.eye_height=3216',
                    'foveation.enabled=0')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-3072-quality' = @{
            Description = 'ff7vr stereo, Null backend unpaced, 3072x3216 per eye, foveation preset quality'
            Kind = 'flat'
            Width = 1280; Height = 720
            Set = @('stereo.start_in_stereo=0', 'xr.backend=null', 'xr.null_pace=0', 'xr.eye_width=3072', 'xr.eye_height=3216',
                    'foveation.enabled=1', 'foveation.preset=quality')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-3072-balanced' = @{
            Description = 'ff7vr stereo, Null backend unpaced, 3072x3216 per eye, foveation preset balanced'
            Kind = 'flat'
            Width = 1280; Height = 720
            Set = @('stereo.start_in_stereo=0', 'xr.backend=null', 'xr.null_pace=0', 'xr.eye_width=3072', 'xr.eye_height=3216',
                    'foveation.enabled=1', 'foveation.preset=balanced')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-3072-performance' = @{
            Description = 'ff7vr stereo, Null backend unpaced, 3072x3216 per eye, foveation preset performance'
            Kind = 'flat'
            Width = 1280; Height = 720
            Set = @('stereo.start_in_stereo=0', 'xr.backend=null', 'xr.null_pace=0', 'xr.eye_width=3072', 'xr.eye_height=3216',
                    'foveation.enabled=1', 'foveation.preset=performance')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-svr-2496-off' = @{
            Description = 'ff7vr stereo through SteamVR null driver, 2496x2592 per eye, foveation off'
            Kind = 'mod'
            Width = 1280; Height = 720
            SteamVR = @{ RenderWidth = 2496; RenderHeight = 2592; RefreshHz = 90 }
            Set = @('stereo.start_in_stereo=0', 'xr.backend=openxr', 'xr.runtime=inherit', 'foveation.enabled=0')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-svr-2496-quality' = @{
            Description = 'ff7vr stereo through SteamVR null driver, 2496x2592 per eye, foveation quality'
            Kind = 'mod'
            Width = 1280; Height = 720
            SteamVR = @{ RenderWidth = 2496; RenderHeight = 2592; RefreshHz = 90 }
            Set = @('stereo.start_in_stereo=0', 'xr.backend=openxr', 'xr.runtime=inherit', 'foveation.enabled=1', 'foveation.preset=quality')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-svr-2496-balanced' = @{
            Description = 'ff7vr stereo through SteamVR null driver, 2496x2592 per eye, foveation balanced'
            Kind = 'mod'
            Width = 1280; Height = 720
            SteamVR = @{ RenderWidth = 2496; RenderHeight = 2592; RefreshHz = 90 }
            Set = @('stereo.start_in_stereo=0', 'xr.backend=openxr', 'xr.runtime=inherit', 'foveation.enabled=1', 'foveation.preset=balanced')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-svr-3072-off' = @{
            Description = 'ff7vr stereo through SteamVR null driver, 3072x3216 per eye, foveation off'
            Kind = 'mod'
            Width = 1280; Height = 720
            SteamVR = @{ RenderWidth = 3072; RenderHeight = 3216; RefreshHz = 90 }
            Set = @('stereo.start_in_stereo=0', 'xr.backend=openxr', 'xr.runtime=inherit', 'foveation.enabled=0')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-svr-3072-quality' = @{
            Description = 'ff7vr stereo through SteamVR null driver, 3072x3216 per eye, foveation quality'
            Kind = 'mod'
            Width = 1280; Height = 720
            SteamVR = @{ RenderWidth = 3072; RenderHeight = 3216; RefreshHz = 90 }
            Set = @('stereo.start_in_stereo=0', 'xr.backend=openxr', 'xr.runtime=inherit', 'foveation.enabled=1', 'foveation.preset=quality')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'fov-svr-3072-balanced' = @{
            Description = 'ff7vr stereo through SteamVR null driver, 3072x3216 per eye, foveation balanced'
            Kind = 'mod'
            Width = 1280; Height = 720
            SteamVR = @{ RenderWidth = 3072; RenderHeight = 3216; RefreshHz = 90 }
            Set = @('stereo.start_in_stereo=0', 'xr.backend=openxr', 'xr.runtime=inherit', 'foveation.enabled=1', 'foveation.preset=balanced')
            Cvars = @{ 't.MaxFPS' = '0' }
        }
    }
}
