# Benchmark configurations for tools/bench/bench-run.ps1 and bench-compare.ps1.
#
# Every configuration runs the same way: the game starts windowed with the
# mod DLL loaded only for measurement (tools/bench/bench.ini: frame timer on,
# nothing else), the latest save is loaded, and frames are timed at
# IDXGISwapChain::Present. The keys below describe what differs.
#
#   Description   one line for the result table
#   Kind          flat  - the plain game, no VR
#                 uevr  - UEVR injected into the game once gameplay is reached
#                 mod   - this project's own VR modes (ini keys in Set)
#   Width, Height window client size (-ResX/-ResY, windowed). The game's saved
#                 settings are not touched.
#   Set           extra ini keys for the mod DLL, "section.key=value"
#   SteamVR       for VR kinds: SteamVR null driver settings for the run
#                 (RenderWidth/RenderHeight = the runtime's recommended
#                 per-eye size, RefreshHz). Applied with
#                 tools/dev/steamvr-null-enable.ps1 and restored afterwards.
#   UevrConfig    for Kind uevr: keys replaced in UEVR's config.txt for this
#                 run only (the profile is restored and verified after
#                 every run)
#   Cvars         console variables set once gameplay is reached, at console
#                 priority, for this process only (nothing is saved). Used to
#                 lift the game's own frame cap (t.MaxFPS comes from the game's
#                 settings). The value takes effect on the game thread only:
#                 use it for game-thread variables such as t.MaxFPS.
#
# UEVR's per-eye size is the runtime's recommended size times
# OpenXR_ResolutionScale (0.861 in the profile this was set up for), slightly
# enlarged when projection cropping is on. RenderWidth/Height below are chosen
# so the eye size lands on the target; the size actually rendered is read from
# UEVR's log and stored in the result.
#
# Add a configuration by copying an entry. Names are used in file names.
@{
    Configs = @{
        'flat-720p' = @{
            Description = 'Plain game, flat, 1280x720 window, game settings as saved (frame cap, dynamic resolution)'
            Kind = 'flat'
            Width = 1280; Height = 720
        }
        'flat-720p-uncapped' = @{
            Description = 'Plain game, flat, 1280x720 window, no frame cap'
            Kind = 'flat'
            Width = 1280; Height = 720
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'flat-2eye-2496' = @{
            Description = 'Plain game, flat, 4800x2700 window (pixel count of two 2496x2592 eyes), no frame cap'
            Kind = 'flat'
            Width = 4800; Height = 2700
            Cvars = @{ 't.MaxFPS' = '0' }
        }
        'uevr-2496' = @{
            Description = 'UEVR, owner profile, about 2496x2592 per eye'
            Kind = 'uevr'
            Width = 1280; Height = 720
            SteamVR = @{ RenderWidth = 2900; RenderHeight = 3010; RefreshHz = 90 }
            UevrConfig = @{}
        }
        'uevr-3072' = @{
            Description = 'UEVR, owner profile, about 3072x3216 per eye'
            Kind = 'uevr'
            Width = 1280; Height = 720
            SteamVR = @{ RenderWidth = 3568; RenderHeight = 3735; RefreshHz = 90 }
            UevrConfig = @{}
        }
        'uevr-2496-hzb' = @{
            Description = 'UEVR, owner profile with HZB occlusion left on, about 2496x2592 per eye'
            Kind = 'uevr'
            Width = 1280; Height = 720
            SteamVR = @{ RenderWidth = 2900; RenderHeight = 3010; RefreshHz = 90 }
            UevrConfig = @{ VR_DisableHZBOcclusion = 'false' }
        }
    }
}
