# Benchmarking

How to measure any configuration of the game (plain game, UEVR, this mod in
each of its modes) the same way, without anyone at the keyboard and without a
headset, and how to read the numbers.

Everything lives in `tools/bench/` (Windows PowerShell 5.1) and `src/dev/`
(the in-game frame timer). Results are machine-specific and stay out of git,
under `captures\bench\`.

## Quick start

```
# Build the measurement DLL (only core, loader and src/dev are needed)
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\build.ps1 -BuildDir build\bench -Without xr,xr_smoke,engine,render

# One configuration, end to end
powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\bench-run.ps1 -Config uevr-2496 -BuildDir build\bench

# The comparison: several configurations, three runs each, table with spread
powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\bench-compare.ps1 -BuildDir build\bench -Runs 3 `
    -Configs flat-720p,flat-720p-uncapped,flat-2eye-2496,uevr-2496,uevr-3072

# Print the table of an earlier comparison again
powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\bench-compare.ps1 -Summarize captures\bench\<time>-compare
```

A run takes about 1.5 minutes (start, load, warm-up 20 s, record 45 s, clean
up). The comparison prints a Markdown table, writes `summary.md` and
`summary.json`, and finishes with a check that the machine is back in its
resting state (see below); it exits non-zero if anything is left over.

Requirements: everything `tools/dev/launch.ps1` needs (Steam, the game, a
save; see `docs/dev-harness.md`), an NVIDIA GPU for GPU samples (NVML,
installed with the driver), SteamVR for VR configurations, and a UEVR build
for UEVR configurations. Nothing needs administrator rights.

## What one run does (`bench-run.ps1`)

1. Takes the game lock and, for VR configurations, the SteamVR lock. It never
   holds one while waiting for the other, so two runs that need both cannot
   deadlock.
2. VR configurations: switches SteamVR to its null driver
   (`tools/dev/steamvr-null-enable.ps1`) with the configuration's render size
   and refresh rate, turns SteamVR's dashboard off (it otherwise opens over
   the application and adds compositor work), and starts SteamVR. The
   settings file is restored byte for byte afterwards.
3. UEVR configurations: makes sure a verified backup of UEVR's profile folder
   for this game exists, then writes the run's `config.txt` (see "UEVR").
4. Starts the game with `tools/dev/launch.ps1 -Until gameplay`: windowed at the
   configuration's size, the measurement DLL deployed with
   `tools/bench/bench.ini`, Luma/ReShade's `dxgi.dll` renamed away for the
   run, the latest save loaded. VR configurations get `XR_RUNTIME_JSON`
   pointing at SteamVR's runtime and every implicit OpenXR API layer disabled
   for the process (their own `disable_environment` variables), so overlays
   such as ReShade's OpenXR layer do not add their cost to one configuration
   and not another. The system's default OpenXR runtime is never changed.
5. UEVR configurations: injects UEVR and waits until its log shows the
   framework, the OpenXR swapchains and the stereo device.
6. Sets the configuration's console variables, warms up (checking that the
   game keeps presenting frames), then records for `-RecordSeconds`: frame
   times at Present, GPU samples every 250 ms, the game's CPU time per
   process and per thread.
7. Captures the game window and, for VR, SteamVR's null-driver window (what
   the runtime composited).
8. Cleans up, also after a failure: stops the game, undeploys the DLL (its
   log goes to the result folder), renames `dxgi.dll` back, restores UEVR's
   profile and verifies it file by file, stops SteamVR and restores its
   settings byte for byte, releases the locks.
9. Writes `result.json` into `captures\bench\<time>-<config>\` together with
   `frames.csv`, `gpu.csv`, `ff7vr.log`, `uevr-log.txt` and the PNGs.

Exit codes: 0 ok, 1 a step failed (the reason is in `result.json` under
`failures`), 3 the locks could not be taken.

## The measurement

### Frame times: Present

Frame times are the intervals between consecutive calls of
`IDXGISwapChain::Present` on the game's swap chain, measured by the frame
timer in `src/dev/frame_timer.cpp` inside the game:

- It is installed once the game window exists, as an inline hook on the body
  of `Present` in `dxgi.dll` when it can. UEVR (and the Steam overlay) hook
  Present by replacing the vtable slot; their hook runs first and calls the
  original function, which lands in ours, whichever is installed first.
- When the body is already hooked through the same hook library (the render
  module hooks it too, and the library hooks a function only once), the timer
  replaces the vtable slot instead and logs `Present's body is already hooked
  through the same hook library; using the vtable slot`. It then runs before
  the render module's hook and calls into it. Either way it sees each Present
  once, and the frame interval does not depend on hook order; `present_ms`
  then also includes the render module's work inside Present.
- Per frame it takes two `QueryPerformanceCounter` readings and one store into
  a preallocated array: no allocation, lock or I/O on the render thread.
- It records the time at entry to Present (the frame interval) and the time
  spent inside the original Present (`present_ms`: blocking on the GPU queue
  or on vsync).
- Only one swap chain is tracked (the first that presents; another one takes
  over only after two silent seconds). Test presents are ignored.

With UEVR the game still presents once per frame to its desktop window, and
UEVR's own frame pacing (`xrWaitFrame`) happens on the game's threads, so the
Present interval is the frame interval in both cases. The same measurement
therefore applies unchanged to every configuration.

Controlled through the dev pipe (`\\.\pipe\ff7vr-dev`): `bench status`,
`bench start`, `bench stop <csv>`, `bench cvars <names>` and
`bench setcvar <name> <value>` (see `src/dev/include/ff7vr/dev/dev.h`).

### Statistics in `result.json`

| Field | Meaning |
|---|---|
| `frames.avgFps` | frames / total time of the recording |
| `frames.frameTimeMs.p50/p95/p99` | percentiles of the frame interval |
| `frames.onePercentLowFps` | 1000 / mean of the slowest 1% of frame times |
| `frames.presentMs` | time inside the original Present (p50, p95) |
| `gpu.utilPct` | NVML GPU utilisation (share of time a kernel was running), averaged |
| `gpu.memUsedMiB` | GPU memory used on the whole device (all processes) |
| `gpu.powerW`, `clockMHz`, `tempC` | averages over the recording |
| `cpu.coresBusy` | game process CPU time / wall time (1.0 = one logical core) |
| `cpu.busiestThreads` | the six busiest threads of the game, % of one core |
| `eyeResolution`, `renderedPixels` | what was actually rendered (below) |
| `cvars` | render-related console variables read before recording |
| `backBuffer` | the game's swap chain size |

Resolution actually rendered: for UEVR the double-wide eye swapchain size
from UEVR's own log (`[VR] Width/Height`); for the flat game the back buffer
size times `r.ScreenPercentage`.

### Run-to-run spread (`bench-compare.ps1`)

Configurations are run interleaved (A B C A B C ...), each run a fresh game
start. The table gives the mean and sample standard deviation of each figure
over the runs, and the spread (max - min) / mean of the average fps. Treat a
difference as real only when it is clearly larger than the spread of both
configurations.

For VR configurations the table also has SteamVR's own per-application
statistics over the whole session (`SteamVR app GPU ms`, `SteamVR app CPU ms`,
from `steamvrAppStats` in `result.json`); they are empty for flat runs. The
p50 column shows the mean and standard deviation over the runs.

## The benchmark scene

Scene `idle`: the latest save, loaded through "Continue", the character
standing still where the save puts it, camera untouched, recording starts 20 s
after gameplay is reached (plus the time UEVR needs to start). Everything in
it is deterministic except ambient animation, NPCs and the game's own camera
drift, so frame times within one run are very stable (frame-time standard
deviation well under 1 ms on the development machine).

It is only as repeatable as the save: the newest save decides where you
stand. Keep the save fixed while comparing (the harness never saves; the
game's own autosave does not trigger while standing still).

Scene `pan` (`-Scene pan`): the same, plus relative mouse moves sent with
`SendInput` every 10 ms during the recording (`-PanStep` pixels each, default
6), which turns the camera at a constant rate while the game window has the
focus. The number of input events delivered is stored as `panInputs`. Its
effect on the camera depends on the game's mouse sensitivity setting, and
under UEVR the mouse may be handled differently; check `game.png` /
`steamvr-vrcompositor.png` before relying on it.

## UEVR

### Which build

`Find-UevrBuild` picks the UEVR build whose `UEVRBackend.dll` contains the
commit hash that the game profile's last `log.txt` names (the build the
profile was last used with). It searches the folders directly below
`%USERPROFILE%\Downloads`, `Documents` and `Desktop`. Override with
`FF7VR_UEVR_DIR` or `-UevrDir`.

### What injecting does

`UEVRInjector.exe` is a GUI; the scripts do what it does when "Inject" is
pressed with OpenXR selected and "Nullify VR plugins" ticked (the settings the
injector stores in `%LOCALAPPDATA%\praydog\UEVRInjector_*\user.config`):

1. `LoadLibraryW("<uevr>\UEVRPluginNullifier.dll")` in the game through a
   remote thread, then a remote call of its export `nullify()`: it renames the
   game's own `openxr_loader.dll` / `openvr_api.dll` strings to `.nul` so the
   engine's VR plugins cannot load and take the runtime.
2. `LoadLibraryW("<uevr>\openxr_loader.dll")`: the OpenXR loader UEVR will
   use. It reads `XR_RUNTIME_JSON` from the game's environment, which is how
   the run is pointed at SteamVR.
3. `LoadLibraryW("<uevr>\UEVRBackend.dll")`: UEVR itself. It reads the
   profile from `%APPDATA%\UnrealVRMod\ff7remake_\` (including
   `Frontend_RequestedRuntime=openxr_loader.dll`), loads the profile's
   plugins and scripts, hooks D3D11 and the engine, and creates the OpenXR
   session.

Injection happens once gameplay is reached (the title screen is driven
without UEVR, so its overlay cannot confuse the screen detection). The run
then waits for these lines in UEVR's `log.txt`: `Framework initialized`,
`Successfully created OpenXR swapchains` and `Stereo rendering device setup
successfully` / `Found active stereo device`. Their absence is a failed run.

### The profile is never left changed

UEVR rewrites `config.txt` and `log.txt` in its profile folder. Before the
first UEVR run (and whenever the profile has changed since the last backup)
the whole folder is copied to
`%USERPROFILE%\ff7-remake-vr-backups\uevr-profile-<time>\` with a SHA-256
manifest. For a run, `config.txt` is the profile's own with the
configuration's `UevrConfig` keys replaced, plus
`FrameworkConfig_RememberMenuState=true` and `FrameworkConfig_MenuOpen=false`
so UEVR's menu stays closed (it opens on injection by default; this is a UI
setting only). After every run the folder is put back exactly (files UEVR
added are removed, changed ones copied back) and every hash is compared. A
marker file in `.locks\state\` records an unfinished restore; the next run
restores first.

## Configurations

Defined in `tools/bench/configs.psd1` (keys documented at the top of that
file). The ones provided:

| Name | What |
|---|---|
| `flat-720p` | plain game, 1280x720 window, the game's saved settings (its frame cap and dynamic resolution apply) |
| `flat-720p-uncapped` | same, with `t.MaxFPS 0`: the CPU-side ceiling of the scene |
| `flat-2eye-2496` | plain game at 4800x2700 (the pixel count of two 2496x2592 eyes), uncapped |
| `uevr-2496` | UEVR with the profile as it is, about 2496x2592 per eye |
| `uevr-3072` | the same at about 3072x3216 per eye |
| `uevr-2496-hzb` | profile with `VR_DisableHZBOcclusion=false` |
| `uevr-2496-nofix` | profile with `VR_NativeStereoFix=false` |
| `uevr-2496-early` | profile with `VR_SynchronizationMode=0` |
| `mod-screen-720p` | this mod's virtual screen mode (render module, `build\full`), 1280x720, uncapped |

`tools/bench/configs-foveation.psd1` holds more configurations for
`-ConfigFile`: foveation presets, through the Null backend without pacing
(`fov-<size>-<preset>`) or SteamVR's null driver (`fov-svr-<size>-<preset>`);
see `docs/render.md`.

### Adding a configuration of this mod

Copy an entry and set `Kind = 'mod'`, a `SteamVR` block (VR modes run against
SteamVR's null driver like UEVR), `BuildDir` pointing at a full build, and the
ini keys that switch the mode on in `Set`, for example:

```
'mod-stereo-2496' = @{
    Description = 'ff7vr stereo device, 2496x2592 per eye'
    Kind = 'mod'
    Width = 1280; Height = 720
    BuildDir = 'build\release'
    SteamVR = @{ RenderWidth = 2496; RenderHeight = 2592; RefreshHz = 90 }
    Set = @('render.enabled=1', 'xr.backend=openxr', 'xr.runtime=inherit')
}
```

`Set` keys are appended to `tools/bench/bench.ini`, so the frame timer stays
on. The render module and the frame timer both hook Present; whichever comes
second falls back or chains (see "Frame times: Present"), and both see every
Present.

The mod's stereo mode: give the configuration `Stereo = $true` (or start it
with `stereo.start_in_stereo=0`). `launch.ps1` reaches gameplay with stereo
off (the menus are recognised from the window, which in stereo shows a crop of
an eye), then the run sends `stereo on` through the dev pipe and waits for
`stereo status` to report `active=1` before the warm-up starts. After the
recording it asks again: a run whose stereo is no longer active fails, and the
`eye=WxH` of that reply (the eye size the engine rendered) becomes the
result's `eyeResolution` and `renderedPixels` (`resolutionSource` says where
the size came from). The replies of `stereo status`, `fov status` and `status`
are stored in `result.json`.

### Command line knobs

`bench-run.ps1 -WarmupSeconds 20 -RecordSeconds 45 -BuildDir <dir> -UevrDir
<dir> -ConfigFile <psd1> -OutDir <dir>`; `bench-compare.ps1 -Configs a,b
-Runs 3` plus the same knobs and `-StopOnFailure`.

## Stereo at headset resolution: `vr-perf-session.ps1`

The configurations above compare whole runs. For the cost of the mod's stereo rendering and
of single settings, `tools/bench/vr-perf-session.ps1` runs one game session and measures
many settings in it, switched through the dev pipe, so differences of a few tenths of a
millisecond are visible (alternate the settings; two windows of the same setting differ by
about 0.05-0.1 ms standing still).

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\bench\vr-perf-session.ps1 `
    -EyeWidth 3600 -EyeHeight 3600 -Steps tools\bench\vr-perf-steps\scenes.ps1 [-BuildDir build\<name>] [-ExtraSet "section.key=value"]
```

What it does: the player's ini (`tools/package/ff7vr.ini`) plus the XR Null backend at the
given eye size without frame pacing (`xr.null_pace=0`), the dev pipe, `r.BloomQuality 0` in
stereo and no periodic timing lines; the latest save through the harness; `t.MaxFPS 0`; then
the steps file, dot-sourced with the helpers of `vr-perf-lib.ps1`:

| Helper | Does |
|---|---|
| `Measure-Perf <label> [seconds]` | a frame time window (`stereo frametime`, the engine's frame interval: avg, p50, p95, max) and the GPU time of the scene and of everything after it until Present (`fov timing`, timestamp queries) over the same seconds; one line `RESULT ...` and a row in `results.json` |
| `P "<cmd>;<cmd>"` | dev pipe commands, replies printed |
| `Start-Pan <step>` / `.Stop()` | turn the camera with relative mouse moves (negative step: the other way) |
| `Send-Walk <ms>` | walk forward with W |

Steps files in `tools/bench/vr-perf-steps/`: `scenes.ps1` (three views of the latest save and
turning, each with the per-eye reflections off and on: the before/after table in
`docs/engine-module.md`, "At headset resolution"), `render-scale.ps1` (fixed render scales and
the dynamic resolution), `turning.ps1` (frame logs while turning, `stereo framelog`: which
thread a slow frame comes from; `docs/engine-module.md`, "Turning: where the slow frames come
from").

A session only stops the game it started: when `launch.ps1` exits with 3 (no lock, or a game
someone else started is running), it leaves that game alone. Output: `captures\perf\<time>-<tag>-<w>x<h>\` with `results.json`,
`results.txt`, `status.txt`, captures and the mod's log. The game is stopped and everything
put back at the end (`-NoStop` keeps it running).

Without pacing the frame time is the GPU frame time of the game (it is GPU-bound at these
sizes): compare it with the headset's frame period, keeping in mind that the runtime's
compositor and, for Virtual Desktop, the video encoding take GPU time on top.

For one frame in detail: `fov trace` (GPU time per render target binding, accurate) and
`gpu trace <prefix>` (every draw with its targets, inputs and shaders; its per-draw GPU times
include idle time while the trace itself holds the CPU, so use it for what is drawn, not for
how long it takes).

## Video memory and slow phases

Three different states make stereo frames slow for seconds or minutes while the GPU reports
100 % "utilization" but draws a third of its power (80-130 W against 250-340 W when it
renders). All were measured with the Null backend at 90 Hz, the player's ini (DLSS on at
`input_scale` 0.58 unless noted, foveation `performance`, LOD lines, bloom off), walking
in the first room and the alley, on an RTX 5080 (16 GB, `DedicatedVideoMemory` 15977 MB)
whose PCIe link runs at gen 4 x8 (`nvidia-smi --query-gpu=pcie.link.gen.current,pcie.link.width.current`;
the card supports x16).

**1. The card is full.** When all processes together hold about 95 % of the card, Windows'
video memory manager moves part of the game's memory to system memory and back, and the GPU
waits for it over PCIe. Frames go to 85-130 ms and stay there for as long as the memory does
not shrink (minutes, the whole session). The game's own budget from
`QueryVideoMemoryInfo` does not show it (12.9 GB used of a 13.8-15.2 GB budget): the budget is
per process, the limit is the card. Signature: card usage (counter `GPU Adapter Memory`) at
95-99 %, the game's `Shared Usage` (counter `GPU Process Memory`) at 400-900 MB instead of
50-100, the System process's copy engine busy, and in `stereo framelog` the GPU frame time
equal to the frame interval while the game thread ticks in 3-4 ms.

**2. Uploads after loading.** For 20-40 s after the save is loaded (and longer after a change
that reallocates the scene buffers, 2 minutes after the `r.ScreenPercentage` switch below) the
game's copy engine and the System process's copy engine are busy at 50-100 % and frames take
20-70 ms; the card is far from full (62-76 %). It ends by itself. Starting right after the
previous game exited (14 s) or after a 2-minute pause made no difference.

**3. A copy-bound state that may not end (cause not found).** At 4032x3648 with DLSS at 0.58,
three runs (two with the committed code, one with a work-in-progress DLSS output mode) stayed
at a median of about 71 ms per frame for the whole run (4 minutes), and two of four earlier
3072x3264 runs without DLSS did the same for 5 minutes. It is intermittent: the same 4032
setting started 100 s after the previous game's exit ran at 11.1 ms after 20 s, and started
0.6 s after an exit it was slow (median 17-24 ms) for 105 s and then fine. The three
4-minute cases had all started 5-10 s after an exit, but other quick starts were fine, so a
quick restart is a suspect, not a proven cause. The card was only 62-90 %
full. During it the game's two copy engines are busy at 40-130 % (summed), the System
process's copy engine at 30-100 % and dwm's 3D engine at 10-90 %; the frame log shows the RHI
thread waiting (9 ms of CPU in a 72 ms frame) rather than working. In one such session, with
nothing moving (head pose frozen with `xr-sim head 0 0`, no input), it stayed slow with
foveated rendering off, with DLSS off, and even with stereo off (the flat 1280x720 game, normally
2.2 ms, took 73-249 ms). A separate process's GPU work (200 clears and copies of a 2048x2048
FP16 target, 5.6 ms when the GPU is free) took 6-77 ms next to it: the game's GPU work holds the
GPU while doing little (99 % busy, 76 W, memory-controller load 2 %, normal clocks, no
throttle reason). A lower `r.Streaming.PoolSize` did not change it at 4032 (2500 MB: still
71 ms), while at 4608x4224 the same setting gave 13 ms runs (twice). Not tried: a GPU trace
(`gpu trace`) inside the state, the card at its stock clocks (MSI Afterburner applies a custom
voltage curve and memory offset at start-up; the core sits at 2730 MHz even when nearly idle),
NVIDIA Broadcast closed.
| Run (4-5 min each) | Eye | Card, all processes | Game (DXGI) | Frames after the start | Power |
|---|---|---|---|---|---|
| 3072, no DLSS | 3072x3264 | 10.7-11.3 GB (67-71 %) | 8.6-9.4 GB | 11.11 ms (90 Hz) | 200-207 W |
| 4032, no DLSS | 4032x3648 | 12.5-12.7 GB (78-80 %) | 8.9-10.6 GB | 11.11 ms (90 Hz) | 260-279 W |
| 4608, DLSS 0.58 | 4608x4224 | 15.3-15.7 GB (95-98 %) | 11.8-12.9 GB | **85-136 ms, to the end** | 88-101 W |
| 4608, DLSS 0.58, `r.Streaming.PoolSize` 2500 | 4608x4224 | 14.4-14.6 GB (90-91 %) | 11.1-11.3 GB | 13.0-13.4 ms, GPU-bound (twice) | 303-318 W |
| 4608, DLSS via `r.ScreenPercentage` 58, `input_scale` 1 | 4608x4224 | 12.0 GB (75 %) | 8.1-8.8 GB | 14.2 ms after a 2-minute upload phase | 330-340 W |
| 4032, DLSS 0.58 (committed code, twice: pool 4000 and 2500) | 4032x3648 | 12.5-14.4 GB (78-90 %) | 9.2-12.0 GB | **69-114 ms median, to the end** (state 3) | 55-95 W |
| Player's headset session (Virtual Desktop), DLSS 0.58 | 4608x4224 | 15.4-15.85 GB (97-99 %) | 12.1-12.9 GB | 50 then 87 ms after 20 good seconds | 110-130 W |

What fills the card at 4608x4224 with DLSS:

- The engine's scene buffers are sized from the eye target. `[dlss] input_scale` and
  `[stereo] render_scale` only shrink the views' rectangles (`docs/engine-module.md`, "Render
  scale and dynamic resolution"), so with DLSS at 0.58 every scene buffer still has the full
  9216x4224, of which a third is drawn. `r.ScreenPercentage` sizes them to the rendered size:
  the same DLSS input and output with `r.ScreenPercentage 58` and `input_scale 1` used 3.9 GB
  less (12.7 -> 8.8 GB for the game).
- The texture streaming pool: `r.Streaming.PoolSize` is 4000 MB (set by the texture quality
  setting; `r.Streaming.LimitPoolSizeToVRAM` 0). At 2500 the game used 1.4 GB less.
- DLSS: 706 MB per eye for the NGX feature at a 4608x4224 output (preset K; 366 MB at
  3072x3264), plus the motion-vector and output textures.
- The mod's own per-eye fixes: a copy of the scene colour at the full buffer size (FP16,
  9216x4224, 311 MB) and the reflections target (4624x4224 FP16, 156 MB); with
  `r.ScreenPercentage 58` they are 105 and 53 MB.
- After a `capture`, its targets stay allocated until the session ends: per eye a compose
  target and a conversion target in the card and a staging copy in system memory (each
  W x H x 4 bytes: 467 MB at 4608x4224, 311 of them in the card).
- Other programs: dwm 0.7-1.1 GB, Virtual Desktop's Streamer 0.2-0.7 GB (more while it streams
  a large image), Steam's web helper 0.2 GB, browsers, editors: about 2-3 GB of the 16 with
  nothing else running.

What helps, measured:

- Keep the card below about 90 %. At 4608x4224 with DLSS, `[stereo_cvars] r.Streaming.PoolSize = 2500`
  turned 85-136 ms frames into 13 ms (two runs). In a still view at 3072x3264 the backgrounds
  were as sharp at 2500 as at 4000 (crops at 1:1; one view, characters not compared), and the
  game held 1.3 GB less.
- `r.ScreenPercentage` for the DLSS input instead of `input_scale` sizes the scene buffers (and
  the mod's scratch textures) to the rendered size: 3.9 GB less at 4608x4224. Foveated rendering
  does not follow it (its surface is still laid out for the full eye, so the rings most likely
  sit in the wrong place; not looked at in an image).
- Fewer other programs on the card (each holds some of the 2-3 GB the game cannot have).

The third state is not explained by memory and none of these settings removed it at 4032x3648.
How to watch it: the timing block's `video memory:` line (`docs/render.md`, "Timing") and its
warning, the dev command `vram`, and `tools\bench\gpu-counters.ps1 -OutCsv <file>` (every
second: busy GPU engines per process, each process's memory in the card and in system
memory, the card's total; no elevation needed) alongside `nvidia-smi --query-gpu=memory.used,utilization.gpu,power.draw,clocks.sm,pcie.link.width.current --format=csv -lms 1000`.

## Resting state

After `bench-compare.ps1` (and after every `bench-run.ps1`) the machine is
back where it started: no `ff7remake_` or SteamVR process, Luma/ReShade's
`dxgi.dll` in place, SteamVR's `steamvr.vrsettings` restored byte for byte,
UEVR's profile identical to its backup, save games identical to the newest
save backup (the UE4 crash reporter's per-start ini under `Saved\Config` is
ignored), no lock held. `bench-compare.ps1` checks each of these at the end
and reports what is not.

## What the numbers do and do not show

They show:

- Frame rate and frame-time distribution of the same scene at a known render
  size, measured identically for every configuration.
- Whether a configuration is GPU-bound (GPU utilisation near 100%, frame time
  follows pixel count), CPU-bound (one game thread near 100% of a core, GPU
  well below 100%) or paced (neither saturated; frame time sits on a fixed
  interval).
- Relative cost of settings, when everything else is equal.

They do not show:

- Headset experience. SteamVR's null driver does not pace like a headset: its
  compositor is not tied to a display, so frame pacing, reprojection and
  latency are not what a Quest 3 through Virtual Desktop does. Compare
  configurations with each other, not with a headset's refresh rate.
- Compositor and streaming cost: Virtual Desktop's encoder and the headset
  are not involved.
- Anything about scenes other than the one measured. A standing-still scene
  understates CPU load from movement, streaming and combat.
- Per-process GPU memory: `gpu.memUsedMiB` is the whole device, including the
  desktop, SteamVR and anything else running.
- Absolute values on another machine.

Known caveats:

- The game's own frame cap (`t.MaxFPS`, from its settings, 120 on the
  development machine) applies to the flat configurations unless `t.MaxFPS 0`
  is set (the `-uncapped` ones). UEVR lifts it itself (`VR_UncapFramerate`).
- The flat game uses dynamic resolution (`r.DynamicRes.OperationMode 2`,
  75-100%). It only lowers the resolution when a frame exceeds its budget
  (`r.DynamicRes.FrameTimeBudget`, recorded in each result), so at frame rates
  above that budget the flat game renders at 100%. UEVR's profile switches
  dynamic resolution off.
- `bench setcvar` runs on the dev pipe thread, not the game thread. Variables
  read on the game thread (such as `t.MaxFPS`) take effect; the values read
  back by `bench cvars` afterwards can still show the previous value.
- Keep the desktop alone during a run: the game window has to stay in the
  foreground (the result records the share of time it was; less than 100% is
  noted).
