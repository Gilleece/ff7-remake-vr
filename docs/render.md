# Render module (`src/render`)

The render module owns D3D11 inside the game process and drives the XR
session. CMake target `ff7vr_render`; the loader links it and calls
`ff7vr::render::start()` from `src/loader/startup.cpp` (gated by
`[render] enabled`, default on).

What it does:

- finds and tracks the game's D3D11 device and swap chain (hooks installed at
  start-up, before the engine creates its device);
- runs the XR session (`src/xr`) inside the game, with retries, loss and exit
  handling, without ever stopping the game from running on the desktop;
- **screen mode**: every frame, the game's back buffer is shown in the headset
  on a flat virtual screen (a world-locked quad layer);
- **stereo mode**: the engine's side-by-side image goes to the headset as a
  projection layer, with the screen as the automatic fallback whenever the
  engine is not delivering stereo frames;
- **UI layer** in stereo: the game's UI (HUD, command menu, menus, dialogue)
  is drawn once into its own texture and shown on a quad floating in front of
  the user instead of inside each eye, and over the desktop window;
- **foveated rendering** in stereo: the periphery of each eye is shaded at a
  lower rate (NVIDIA variable rate shading), on by default with the `quality`
  preset;
- in-game captures of what each eye sees, recenter, status and test commands
  through the dev pipe;
- frame timing of everything the module does, on the CPU and the GPU.

Contents: [Modes](#modes) · [Switching between stereo and the screen](#switching-between-stereo-and-the-screen) ·
[Frame wait and pacing](#frame-wait-and-pacing) · [ini keys](#ini-keys) ·
[Session life cycle](#session-life-cycle) · [Dev commands](#dev-commands) ·
[Captures](#captures) · [Timing](#timing) · [Measured overhead](#measured-overhead) ·
[The game's D3D11 usage](#the-games-d3d11-usage) · [D3D11 hooks](#d3d11-hooks) ·
[Stereo interface](#stereo-interface) · [UI layer](#ui-layer) · [Foveated rendering](#foveated-rendering) · [First test on a Quest 3](#first-test-on-a-quest-3-through-virtual-desktop)

## Modes

| Mode | What the headset shows | Who starts XR frames (`xrWaitFrame`) |
|---|---|---|
| `screen` (default) | the back buffer on a virtual screen, 1.8 m wide, 2 m in front of the recentered head, at eye height | the module's `ff7vr xr` thread (or the presenting thread, `[xr] frame_wait = present`) |
| `stereo` | the engine's side-by-side image as a projection layer | the engine's game thread (`BeginGameFrame`); the `ff7vr xr` thread while the game thread is not starting frames |
| `stereo-test` | the back buffer as both eyes of a projection layer (head-locked, no depth); a test thread stands in for the game thread | the test thread |

The engine module switches between `screen` and `stereo` through `SetMode`;
`stereo-test` exists to exercise the stereo path without the engine.

## Switching between stereo and the screen

The virtual screen is the automatic fallback of stereo mode. Whenever the
engine is not delivering stereo frames (title screen, loading screens, menus
that stop the 3D view, a blocked game thread), the headset shows the game on
the screen instead, without a gap in the frames the runtime receives.

Rules, per XR frame:

1. A frame the engine submitted a stereo image for is ended with that image
   (projection layer).
2. A frame without a stereo image in stereo mode, when the last stereo image
   is younger than 300 ms, re-shows that last image: the eye swapchains keep
   it and the runtime re-projects it with the poses it was rendered with.
   Nothing is read from the engine's texture again. This covers hitches and
   single frames whose image went missing, which would otherwise flash the
   back buffer (in stereo the back buffer holds the desktop mirror, not a
   full view).
3. Any other frame is ended with the screen layer showing the current back
   buffer.

Who starts frames: in stereo mode the game thread paces the session through
`BeginGameFrame`. When it has not called `BeginGameFrame` for 100 ms (a
blocking level load while a loading screen is presented from another thread,
a long hitch), the `ff7vr xr` thread takes over and starts frames itself, so
the runtime keeps getting frames, which rules 2 and 3 fill. As soon as the
game thread calls `BeginGameFrame` again it paces again; the frame the XR
thread may have started in between is ended first (at most two frames are
started ahead of Present at that moment, one otherwise). Both transitions are
logged (`xr: stereo mode, but the game thread is not starting frames ...`,
`xr: stereo mode: the game thread paces frames again`).

The screen layer's swapchain exists whenever a session runs, also in stereo
mode, so switching to it never creates a swapchain at the switch.

What the user sees at a switch:

| Direction | Trigger | Behaviour |
|---|---|---|
| stereo -> screen | the engine calls `SetMode(Screen)` (menus, movies) | Frames already started by the game thread are ended with their stereo images. From the next frame on, `BeginGameFrame` returns `stereo = false` without blocking, the engine renders mono into the back buffer and the XR thread starts frames, which show the screen layer. The view changes from the 3D scene to the screen in one frame; no frame is empty or repeated |
| stereo -> screen (automatic) | the game thread stops starting frames | Up to 100 ms with no new frame (the runtime re-projects the last one, as it does for any late frame), then up to 200 ms of the last stereo image, then the screen layer with whatever the game presents (the loading screen) |
| screen -> stereo | `SetMode(Stereo)` | The XR thread keeps pacing until the game thread's first `BeginGameFrame`, then hands over. The first frames the engine still renders mono (before its eye target exists) show the screen layer with that mono image; the first stereo image replaces it in one frame |

Measured with `mode stereo-test` and the Null backend (captures in
`captures\xr\run2-null`, log lines in its `ff7vr.log`): after
`stereo-test pause 4000` the XR thread took over pacing 92 ms later,
19 frames re-showed the last stereo image, 334 frames showed the screen layer
until the test thread resumed, then stereo frames again; zero submit errors.
Every second image dropped (`stereo-test drop 2`): every dropped frame
re-showed the previous image. `mode screen` / `mode stereo-test`: one frame
of overlap at most, no gap. Captures taken during each phase show the
expected layer.

## Frame wait and pacing

`xrWaitFrame` blocks until the runtime wants the next frame. Called from the
game's Present it caps the game at the headset's refresh rate and makes every
game frame wait for the next headset frame slot: a 120 fps game on a 90 Hz
headset drops to 90 fps, and a game running slightly below the refresh rate is
quantised to half of it.

A quad layer does not need that coupling. The runtime re-projects quads with
the newest head pose on every display refresh, so the screen stays steady
however often its content changes, and a quad carries no pose that has to
match the predicted display time. So by default (`[xr] frame_wait = thread`)
the `ff7vr xr` thread calls `WaitFrame` and keeps one started frame ready; the
next Present takes it, copies the back buffer into the screen layer's
swapchain and ends the frame (`BeginFrame`/`SubmitFrame` on the presenting
thread, which owns the immediate context). The game never blocks: when it
renders faster than the headset, Presents without a started frame do nothing;
when it renders slower, the runtime gets frames at the game's rate.

`frame_wait = present` calls `WaitFrame` inside the Present hook instead (the
classic single-threaded loop). Measured in gameplay with the Null backend
pacing at 90 Hz and the game at its 120 fps limit: `thread` keeps 120.0 fps
(frame interval p99 8.9 ms, present hook 0.017 ms on average); `present`
drops the game to 90.0 fps (frame interval 11.1 ms, present hook 9.3 ms, all
of it spent waiting). SteamVR's null driver paces at about 120 Hz, the
game's own limit, so the two settings measure the same there; see
[Measured overhead](#measured-overhead).

**Default: `thread`.** The screen should cost the game nothing, and the
headset gains nothing from coupling: the quad is re-projected at the display
rate either way. In stereo mode the game thread waits for the frame in
`BeginGameFrame`, because there the rendered views must match the frame's
predicted display time; that is the engine's pacing and independent of this
key. The key can be switched at run time with `frame-wait`.

## ini keys

All keys are optional. `ff7vr.ini` sits next to the DLL.

| Key | Default | Meaning |
|---|---|---|
| `[render] enabled` | `1` | `0`: the module is not started (no hooks, no XR) |
| `[render] mode` | `screen` | `screen` or `stereo-test` (stereo itself is switched on by the engine module) |
| `[render] stats_interval` | `10` | seconds between timing reports in `ff7vr.log` |
| `[render] gpu_timing` | `1` | measure the module's GPU work with timestamp queries |
| `[render] capture_dir` | `ff7vr-captures` next to the DLL | where relative capture prefixes go |
| `[xr] enabled` | `1` | `0`: hooks and timing only, no XR session (the baseline for overhead measurements) |
| `[xr] backend` | `openxr` | `openxr` or `null` (no runtime; captures only) |
| `[xr] runtime` | `virtualdesktop` | `virtualdesktop`, `steamvr`, `system`, `inherit` or a path to a runtime JSON (see `docs/testing-headless.md`). The machine's default OpenXR runtime is never changed |
| `[xr] resolution_scale` | `1.0` | scale of the runtime's recommended eye size (stereo render size and eye swapchains) |
| `[xr] eye_width`, `eye_height` | `0` | explicit eye size instead of the recommendation |
| `[xr] disable_implicit_layers` | `reshade` | implicit OpenXR API layers to disable for the game process: `0`/`none`, `1`/`all`, or a comma-separated list of parts of a layer's name or manifest path. See below |
| `[xr] debug_utils` | `0` | log `XR_EXT_debug_utils` messages from the runtime |
| `[xr] retry_interval` | `5` | seconds between attempts while no headset is available (at least 30 s when the runtime itself is missing, doubled after other errors) |
| `[xr] reconnect_after_exit` | `0` | after the runtime asked the application to exit (user quit it from the VR dashboard, runtime shutting down): `0` stay off until `xr-restart`, `1` retry like after a lost session |
| `[xr] frame_wait` | `thread` | `thread` or `present`, see [Frame wait and pacing](#frame-wait-and-pacing) |
| `[xr] null_refresh_hz`, `null_pace`, `null_motion` | `90`, `1`, `static` | Null backend: emulated refresh rate, whether `WaitFrame` paces to it, head motion (`static`, `yaw`, `sway`, `yawsway`) |
| `[screen] distance` | `2.0` | metres from the recentered head to the screen |
| `[screen] width` | `1.8` | screen width in metres (height follows the back buffer's aspect ratio) |
| `[screen] offset_y` | `0` | vertical offset of the screen centre from eye height, metres |
| `[screen] follow_head` | `0` | `1`: head-locked screen instead of world-locked |
| `[screen] recenter_on_start` | `1` | recenter when the session starts, so the screen appears in front of the user |
| `[ui] layer` | `1` | in stereo, show the game's UI on its own layer instead of in the eye images (see [UI layer](#ui-layer)) |
| `[ui] distance` | `3.0` | metres from the recentered head to the UI quad |
| `[ui] size` | `2.0` | height of the UI quad in metres; the width follows the UI's 16:9 aspect (3.56 m). Same meaning as UEVR's `UI_Size` |
| `[ui] offset_x`, `offset_y` | `0`, `0` | offset of the quad's centre from straight ahead at eye height, metres |
| `[ui] follow_head` | `0` | `1`: the UI follows the head (UEVR's `UI_FollowView`); `0`: it stays where recenter put it |
| `[ui] layer_width` | `1920` | width of the quad's image in pixels; a larger game UI texture is scaled down when copied in. `0` = the game's UI texture width |
| `[ui] mirror` | `1` | also draw the UI over the desktop window in stereo (the window shows an eye image, which no longer has the UI) |
| `[ui] once_per_frame` | `1` | engine side: draw the UI for the first eye only (`0`: the game draws it for both eyes, as without the mod) |
| `[foveation] ...` | on, `quality` | foveated rendering in stereo; keys in [Foveated rendering](#foveated-rendering) |

Screen size: 1.8 m at 2 m covers about 48 x 28 degrees for a 16:9 image, so
the whole picture including the HUD in the corners is visible with small eye
movements, and at 2 m the eyes converge at roughly the distance headset optics
are typically focused at, which is easier on the eyes than a near screen
(a judgement, not measured; check it on the headset). Make the screen larger
with `width` (or nearer with `distance`); the angle is
`2 * atan(width / 2 / distance)`.

Implicit API layers: OpenXR loads every implicit layer registered on the
machine into the game. Registered layers seen on a typical machine with
Virtual Desktop and ReShade installed:

- `XR_APILAYER_reshade` (ReShade's XR support) loads `ReShade64.dll` into the
  process. When ReShade is already the game's `dxgi.dll` (for example with the
  Luma addon), that is a second ReShade instance in the same process, and it
  would apply effects to the XR swapchains on top of the image the first one
  already processed. Disabled by default.
- `XR_APILAYER_VIRTUALDESKTOP_oculus_compatibility` is Virtual Desktop's
  compatibility shim for games built with the OculusXR plugin; it does nothing
  for this game and stays enabled, since it belongs to the target runtime.
- `XR_APILAYER_MBUCCHIA_toolkit` (OpenXR Toolkit) is left alone; it is often
  registered but disabled.

Layers are disabled for the game process only, through each layer's own
`disable_environment` variable. The log lists every implicit layer and its
state (`xr:   implicit API layer ... (enabled|disabled|disabled for this
process)`) when a session is created.

## Session life cycle

The `ff7vr xr` thread creates the backend once the main swap chain has
presented (so the game's device is known) and then:

| Situation | What happens |
|---|---|
| no headset (`SystemUnavailable`: Virtual Desktop running without a connected headset returns `XR_ERROR_FORM_FACTOR_UNAVAILABLE`) | logged once, retried every `retry_interval` seconds; later failures are logged at 2, 4, 8, ... attempts |
| runtime missing (`RuntimeUnavailable`: the runtime JSON or its DLL does not exist) | same, at least every 30 s |
| wrong GPU (`GraphicsMismatch`) | logged, not retried until `xr-restart` |
| session created, not yet running | frames are polled every 10 ms; the game presents normally |
| session lost | session ended, new attempt after `retry_interval` |
| runtime asks to exit | session ended; stays off until `xr-restart` (or retried, `reconnect_after_exit = 1`) |
| the game's device changes | session ended and created again on the new device |
| the game exits | `ExitProcess`/`TerminateProcess` of the game are hooked; the session is ended there (at most 4 s) |

Every attempt runs on the `ff7vr xr` thread, never on a game thread. Measured
in gameplay (`captures\xr\run1-failure`): with Virtual Desktop running and no
headset, an attempt takes about 60 ms on that thread and repeats every 5 s;
with the runtime JSON missing or its DLL missing an attempt takes 1 to 3 ms
and repeats every 30 s. In every case the game stayed at its 120 fps limit
with a frame interval maximum of 9 to 13 ms per 5 s period, the same as with
the session switched off.

Ending a session needs the immediate context, which belongs to the presenting
thread. The XR thread therefore parks that thread inside its next Present
hook, ends the session, and lets it go (up to about 1.5 s for the runtime's
graceful stop). If the game is not presenting (minimised, loading), the
session is ended without parking.

SteamVR names its adapter (`xrGetD3D11GraphicsRequirementsKHR` LUID) only
once its compositor runs. A LUID of 0 is asked again for up to 2 s, then the
session is created on the game's adapter.

## Dev commands

Sent through the dev pipe (`[dev] pipe = 1`), for example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\send-input.ps1 -Pipe "status"
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\send-input.ps1 -Pipe "capture $PWD\captures\xr\title"
```

| Command | Reply / effect |
|---|---|
| `status` | logs the D3D11 facts, threads, XR state, runtime, counters and timing to `ff7vr.log`; replies with a one-line summary |
| `capture <prefix> [timeout ms]` | captures what each eye sees in the next submitted frame to `<prefix>_L.png` and `<prefix>_R.png`; replies `ok frame=<id> L=<path> R=<path>` once written (default timeout 5000 ms) |
| `recenter` | the current head position and yaw become the origin (the screen moves in front of the user) |
| `xr-restart` | ends the session if there is one and starts a new one now (also after an exit request or `xr-stop`) |
| `xr-stop` | ends the session and stays off until `xr-restart`; hooks and timing keep running (the idle baseline) |
| `xr-runtime <selection>` | uses another runtime (same values as `[xr] runtime`) from the next attempt on and restarts the session |
| `frame-wait thread\|present` | moves the screen mode frame wait at run time |
| `mode screen` / `mode stereo` / `mode stereo-test` | switches the mode at the next frame |
| `stereo-test pause <ms>` | in `stereo-test`: the test thread starts no frames for that long, like a game thread blocked by a load |
| `stereo-test drop <n>` | in `stereo-test`: every n-th frame gets no stereo image (`0` = off) |
| `ui status` | UI layer state, placement, image size, frames shown / held / dropped |
| `ui on` / `ui off` | UI on its own layer, or back in the eye images as the game composites it (for comparison) |
| `ui distance <m>`, `ui size <m>`, `ui offset <x> <y>`, `ui follow <0\|1>`, `ui mirror <0\|1>` | change the placement at run time |
| `ui dump <png>` | write the game's next UI texture as it is (with its alpha channel) to a PNG; works in mono too |
| `uihook status` | engine side: UI passes seen, redirected, skipped for the second eye, texture size and format |
| `uihook once <0\|1>` | draw the UI for the first eye only, or for both |
| `uihook proj` | log the projection matrices of the next two views the UI pass receives (in screen mode: the game camera's FOV) |
| `fov ...` | foveated rendering: status, settings, one-frame trace, timing ([Foveation dev commands](#foveation-dev-commands)) |

Modules register commands with `ff7vr::dev_commands::add` (`src/core`); the
pipe passes every line it does not handle itself to `dev_commands::dispatch`.

## Captures

`capture` writes what each eye would see: the layers composited the way the
runtime does it (projection image over each eye's field of view, quads with
perspective from the frame's eye views), at the eye swapchain size. With the
Null backend that is a Quest 3 class view (2064x2208 per eye, asymmetric FOV,
64 mm IPD); with SteamVR's null driver 1512x1680.

- An absolute prefix is used as given. Scripts should pass a path under the
  repository's `captures\` folder (gitignored), for example
  `captures\xr\<run>\<name>`, so nothing is left in the game folder.
- A relative prefix goes to `[render] capture_dir`, by default
  `ff7vr-captures\` next to the DLL in the game's `End\Binaries\Win64`.
- A capture costs one frame of 10 to 20 ms on the presenting thread (GPU
  composite and read-back); it is a development tool.

## Timing

Every `stats_interval` seconds `ff7vr.log` gets a block like this (SteamVR
null driver, gameplay, screen mode):

```
timing: last 5.0 s, 120.0 fps; xr openxr (Focused); mode screen; presents ... submitted screen ... stereo 0 held 0 without XR frame ... errors 0
timing:   game frame interval: avg 8.334 p50 8.340 p95 8.660 p99 8.840 max 9.400 ms (n 600)
timing:   IDXGISwapChain::Present: ...
timing:   present hook: ...
timing:   xr submit: ...
timing:   xr frame wait: ...
timing:   gpu copy: ...
timing:   gpu submit total: ...
timing:   runtime calls per ended frame (CPU, presenting thread): acquire+wait image ... ms, release image ... ms, begin frame ... ms, end frame ... ms (n ...)
```

| Series | Measured how |
|---|---|
| game frame interval | QPC difference between consecutive entries into Present for the main swap chain: the game's real frame time as seen at Present |
| IDXGISwapChain::Present | QPC around the call to the real Present (driver queueing, any hook further in) |
| present hook | QPC from entry into our detour to just before the real Present: everything the module adds on the presenting thread (swap chain tracking, XR submit, and with `frame_wait = present` the frame wait) |
| xr submit | QPC around `IXrBackend::SubmitFrame` (begin frame, acquire/wait/release of the swapchain image, the copy, `xrEndFrame`) |
| xr frame wait | QPC around `WaitFrame` on whatever thread calls it; with `frame_wait = thread` it does not delay the game |
| gpu copy | D3D11 timestamp queries around each copy or blit into an XR swapchain image, summed per frame |
| gpu submit total | timestamps around the whole submission on the immediate context: the copies plus whatever the runtime records on the context inside its calls |
| runtime calls | CPU time inside `xrAcquireSwapchainImage` + `xrWaitSwapchainImage`, `xrReleaseSwapchainImage`, `xrBeginFrame` and `xrEndFrame`, per ended frame |

Each report covers only its own period and gives avg, p50, p95, p99 and max
with the sample count. GPU results are read back a few frames later without
stalling. The header counts `held` frames (rule 2 under
[Switching](#switching-between-stereo-and-the-screen)) and `without XR frame`
(Presents that had no started frame to end, normal when the game renders
faster than the headset).

## Measured overhead

Same scene for every row: gameplay, standing still after loading the latest
save, 1280x720 window, RTX 5080. The game limits itself to 120 fps (its frame
rate setting; `-ExecCmds="t.MaxFPS 0"` does not lift it), so the frame rate
is the same in every row and the cost shows up in the per-frame series
instead. Values are the average over 3 to 4 periods of 5 s (600 frames each).

| Configuration | fps | Frame interval avg / p99 / max (ms) | Present hook avg / p99 (ms) | GPU, our copies (ms) |
|---|---|---|---|---|
| Mod idle (`xr-stop`, hooks and timing only) | 120.0 | 8.334 / 8.8-9.4 / 9.5-10.7 | 0.002 / 0.003 | - |
| Screen, Null backend, `frame_wait = thread` | 120.0 | 8.333 / 8.8-8.9 / 9.0-10.2 | 0.017 / 0.026 | 0.005 |
| Screen, Null backend (90 Hz), `frame_wait = present` | **90.0** | 11.11 / 11.7-11.8 / 11.9-12.1 | 9.33 / 9.8 (waiting) | 0.005 |
| Screen, SteamVR null driver, `frame_wait = thread` | 120.0 | 8.333 / 9.0-9.5 / 9.3-11.3 | 0.39-0.42 / 0.54-0.70 | 0.006 |
| Screen, SteamVR null driver, `frame_wait = present` | 120.0 | 8.333 / 8.8-9.1 / 9.1-11.7 | 0.37 / 0.44-0.54 | 0.006 |
| Stereo test, SteamVR null driver | 120.0 | 8.334 / 9.0-9.1 / 9.4-9.7 | 0.40 / 0.51-0.60 | 0.011 |

Where the SteamVR present hook time goes (per frame, steady state):
`xrBeginFrame` 0.03 ms, `xrEndFrame` 0.20-0.23 ms, `xrReleaseSwapchainImage`
0.11-0.14 ms, `xrAcquireSwapchainImage` + wait 0.003 ms, our own work the
rest (about 0.03 ms). The "gpu submit total" series reads 4-5 ms on SteamVR:
the timestamps around the submission span what the runtime queues or waits
for on the immediate context inside `xrEndFrame`, so it is not the cost of our
copies (those are the "gpu copy" column). With the Null backend both series
agree.

Conclusions:

- With `frame_wait = thread` the virtual screen does not change the game's
  frame rate or frame pacing on either backend; the CPU cost on the
  presenting thread is 0.02 ms with the Null backend and about 0.4 ms with
  SteamVR, almost all of it inside SteamVR's own calls; the GPU cost of the
  copy is 5-6 microseconds at 1280x720.
- `frame_wait = present` ties the game to the runtime's rate: on a 90 Hz
  runtime a 120 fps game runs at 90 fps. SteamVR's null driver happens to
  pace at about 120 Hz, the game's limit, so it hides the effect there.
- **SteamVR blocks in `xrBeginFrame` at times.** For the first 15 to 20 s
  after a session starts (and in one run for the rest of the session after a
  switch to stereo and back), `xrBeginFrame` took 6 to 7.5 ms per frame on
  the presenting thread. The game then ran at 108-118 fps instead of 120, or
  stayed at 120 with the limiter's idle time used up. Seen only with
  SteamVR's null driver; whether a real headset or Virtual Desktop do the
  same is not known. The `runtime calls` timing line shows it
  (`begin frame` in milliseconds instead of hundredths).

Logs: `captures\xr\run1-failure`, `run2-null`, `run3-steamvr`,
`run4-steamvr` (each `ff7vr.log` with `MARK seg-...` lines per segment).

## The game's D3D11 usage

Recorded with the module's `d3d11:` log lines (first Present of each swap
chain, `status`) on game version 1.0.0.7 launched with `-d3d11 -windowed
-ResX=1280 -ResY=720`:

| Fact | Value |
|---|---|
| Device creation | `D3D11CreateDeviceAndSwapChain` on the main thread. Two probe devices first (flags `0x1` single-threaded, feature levels 11_0/10_0), then the real one: hardware driver, flags `0x0`, feature level 11_0 only, no multithread protection, no deferred contexts |
| Adapter | the GPU the desktop runs on; its LUID is logged (`d3d11:   device ...: adapter '...', LUID ...`) and matched against the runtime's (`xr: session created ... adapter LUID ...`). SteamVR reports its LUID only after its compositor runs |
| Swap chain | created with `IDXGIFactory::CreateSwapChain`: 2 buffers, `DXGI_SWAP_EFFECT_FLIP_DISCARD` (4), flags `0x802` (`ALLOW_TEARING` + `ALLOW_MODE_SWITCH`), alpha mode ignore, scaling stretch |
| Back buffer | `R10G10B10A2_UNORM` holding sRGB-encoded values (SDR output), sized to the window or fullscreen resolution (1280x720 in the test runs), bind flags render target + shader resource |
| Present | sync interval 0 with `DXGI_PRESENT_ALLOW_TEARING`; the frame limit is the game's own, not vsync |
| Presenting thread | the main thread for the first frames, then the engine's `RHIThread` for the rest of the session (logged as `the main swap chain is now presented from thread ... 'RHIThread'`); the render thread is `RenderThread 1` |
| With ReShade/Luma as `dxgi.dll` | the game presents ReShade's swap chain proxy; the module's vtable hook sees the image with ReShade's effects applied |

The screen layer's swapchain is created in the sRGB variant of the back
buffer's format family the runtime offers (`R8G8B8A8_UNORM_SRGB` on the Null
backend and SteamVR), and the 10-bit back buffer is converted by a shader blit
that decodes its sRGB values, so brightness matches the desktop.

## D3D11 hooks

All hooks are installed from `start()` on the loader's bootstrap thread:

- `Present`, `Present1`, `ResizeBuffers`, `ResizeBuffers1`: slots of DXGI's
  swap chain vtable, found through a throwaway swap chain on a NULL-driver
  device. Every swap chain the game creates is checked and a new class gets
  its own hooks. A vtable slot hook chains with inline hooks that other
  software places on the same functions (the Steam overlay, ReShade, a frame
  timer): callers through the vtable reach us first, we call what was in the
  slot.
- `IDXGIFactory::CreateSwapChain`, `IDXGIFactory2::CreateSwapChainForHwnd`,
  `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`,
  `ID3D11Device::CreateDeferredContext`: inline hooks, for logging facts only.
- A vectored exception handler that records the thread names the engine
  announces (exception `0x406D1388`), so logs can name the render thread.

The main swap chain is the largest one with a visible window among those that
presented during the last second. No reference to a back buffer is kept beyond
a Present call, so the game's `ResizeBuffers` always succeeds; window mode and
size changes recreate the screen layer at the new size on the next Present.

## Stereo interface

Declared in `src/render/include/ff7vr/render/render.h`, used by the engine's
stereo device (`src/engine`, `docs/re/stereo-hook-plan.md`):

```cpp
namespace ff7vr::render {
enum class Mode { Screen, Stereo };
void SetMode(Mode mode);                       // any thread; effective at the next frame
Mode GetMode();

struct EyeSetup { uint32_t eyeWidth, eyeHeight; xr::Fov fov[2]; float refreshHz; };
bool GetEyeSetup(EyeSetup* out);               // any thread; false while no session exists

struct StereoFrame {
    bool stereo;                               // false: render mono this frame
    uint64_t frameId;                          // pass to SubmitStereoFrame
    bool shouldRender;
    xr::View views[2];                         // tracking space after recenter
    xr::Pose head;
    int64_t predictedDisplayTime, predictedDisplayPeriod;  // ns
};
StereoFrame BeginGameFrame();                  // game thread, once per frame at frame start

struct StereoSubmit {
    uint64_t frameId;
    ID3D11Texture2D* texture;                  // side-by-side image
    DXGI_FORMAT viewFormat;                    // required for typeless textures
    xr::ColorEncoding encoding;                // Srgb (gamma-encoded) or Linear
    xr::Rect eyeRects[2];                      // width/height 0 = left / right half
    bool haveRenderedViews;
    xr::View renderedViews[2];                 // views the image was rendered with, if they differ
};
void SubmitStereoFrame(const StereoSubmit&);   // render thread, before the frame's Present
}
```

| Call | Thread | Use |
|---|---|---|
| `SetMode(Mode::Stereo / Screen)` | any | stereo wanted or not; effective at the next frame |
| `GetEyeSetup(&setup)` | any | per-eye render size (`eyeWidth` x `eyeHeight`; the engine's target is twice as wide), FOVs, refresh rate; false while no session exists |
| `BeginGameFrame()` | game thread, frame start | waits for the XR frame (pacing) and returns its id and both eye views; `stereo == false` means render mono this frame |
| `SubmitStereoFrame({frameId, texture, viewFormat, ...})` | render thread, before Present | records the side-by-side image; the Present hook copies the eye rects into the XR swapchains and ends the frame |

Rules the implementation relies on:

- `BeginGameFrame` never blocks when the session is not running, when the mode
  is Screen, or when earlier frames have not been presented within 250 ms
  (paused, minimised or loading without Present); the engine then renders mono.
- At most one frame is started ahead of Present (two for one frame while the
  XR thread hands pacing back to the game thread).
- Every started frame is ended by a Present, in order: with the stereo image
  when `SubmitStereoFrame` named it, otherwise as described under
  [Switching](#switching-between-stereo-and-the-screen).
- Images are kept per frame id (the four most recent ids), so the render
  thread may record frame N+1 before the presenting thread (`RHIThread`) has
  presented frame N. A second `SubmitStereoFrame` for the same id replaces
  the first; the engine module uses this to offer each image for every frame
  the coming Present may end, with the views it was rendered with. The
  texture is referenced until that frame's Present.
- Views are `xr::View` in tracking space after recenter (`xr_math.h` converts
  to Unreal's units and axes).

`mode stereo-test` exercises this path in the game without the engine: a test
thread calls `BeginGameFrame` and the Present hook submits the back buffer as
both eyes.

## UI layer

The game draws its UI (HUD, command menu, main and save menus, dialogue,
markers) inside the scene renderer into a texture of its own and composites it
in each view's post-processing (`docs/re/engine.md`, section 9). In stereo
that puts it into each eye at zero parallax, as a central crop at twice the
size: the area banner, the command menu and the party panel in the corners are
cut off, and the UI sits at infinite depth while it covers near objects.

With `[ui] layer = 1` (default), whenever the engine renders a stereo frame
and a session runs:

1. The engine module (`src/engine/src/ui_layer.cpp`) lets the game draw the UI
   for the first eye only, keeps it out of both eye images, and reports the UI
   texture to this module before the frame's Present (`SubmitUiLayer`,
   render.h "UI LAYER").
2. At Present the texture is copied into the UI quad layer's swapchain: scaled
   to `[ui] layer_width` and converted from Unreal's inverted alpha (empty =
   alpha 1) to premultiplied alpha. The quad is submitted after the
   projection layer, alpha-blended (`XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`),
   in the same frame as the eye images. The Null backend composites it the same
   way, so captures show it.
3. The UI is also drawn over the desktop window (`[ui] mirror`), whose image
   is a crop of an eye in stereo. Without this the title screen would be black
   on the desktop and menus invisible there.

Frames that re-show the last stereo image keep the quad's last image; frames
shown on the virtual screen (loading, mono fallback) carry no UI quad, their
UI is in the game's image as usual. Screen mode, mono frames and the game
without the stereo device are unchanged.

### Placement

Defaults follow the community UEVR profile for this game (`UI_Distance=3.0`,
`UI_Size=2.0`, `UI_FollowView=false`): a 3.56 x 2.0 m quad 3 m in front of
the recentered head, at eye height, staying in place when the head turns.
That covers **61.3 x 36.9 degrees**: the corners are within an eye movement of
about 30 degrees sideways and 18 degrees up or down. A 1920 px wide layer image
gives about 28 px per degree at its centre, more than the eye images carry
(the Null backend's Quest 3 class eye: about 17 px per degree at the centre),
so text stays sharp; the game's 3840x2160 UI texture is box-filtered 2:1 into
it. The angle for other values: `2 * atan(size * 16/9 / 2 / distance)` wide,
`2 * atan(size / 2 / distance)` high. Curvature (a cylinder layer) is not
implemented.

### World-anchored elements

Target markers, names, damage numbers and the objective marker are placed by
the game from the game camera, not from the eye views. In the gameplay scene
tested its projection is 50.0 x 29.4 degrees (`uihook proj` in screen mode),
while the default quad covers 61.3 x 36.9. On the quad an element therefore
appears further from the centre than its object, for the eye midpoint and a
level game camera:

| Position on the UI (centre to edge) | 25 % | 50 % | 75 % | edge |
|---|---|---|---|---|
| object direction, horizontal | 6.6 deg | 13.1 deg | 19.3 deg | 25.0 deg |
| element on the default quad | 8.4 deg | 16.5 deg | 24.0 deg | 30.7 deg |
| offset, horizontal / vertical | 1.8 / 1.0 deg | 3.4 / 2.0 deg | 4.7 / 2.9 deg | 5.7 / 3.7 deg |

The cheap correction is to give the quad the camera's angle: `[ui] size =
1.57` at `distance = 3` (2.80 x 1.57 m, 50 x 29.4 degrees). Then an element
lines up with its object's direction exactly, with three remaining effects:

- the game camera's pitch: the eye cameras drop it (`[stereo]
  decoupled_pitch`), the markers do not, so markers shift vertically by about
  that pitch (not measured; a fixed correction would be to tilt the quad by
  the camera pitch every frame, which needs the camera rotation from the
  engine module);
- parallax: the quad is at 3 m, so each eye sees an element shifted against
  its object by half the IPD times `1/Z - 1/3 m`: 0.6 deg for an object at
  1.5 m, 0.2 deg at 5 m, 0.4 deg at 10 m;
- head position: leaning moves the eyes, not the game camera.

The camera's FOV is a game setting and changes with camera modes (INFERRED,
only one scene measured), so the matching size is not automatic yet; the
default stays the larger, easier to read UEVR size.

### Measured cost

Null backend, gameplay, eyes 2064x2208, game UI texture 3840x2160, 1280x720
window, averages over 5 s periods (`captures\ui\run2`, `MARK seg-...`):

| | GPU, our work per frame | Present hook (CPU) |
|---|---|---|
| UI in the eyes (`ui off`) | 0.063 ms (eye copies) | 0.022 ms |
| UI layer, no window overlay (`ui mirror 0`) | 0.077 ms | 0.026 ms |
| UI layer and window overlay (default) | 0.086 ms | 0.035 ms |

So the layer costs about 0.014 ms of GPU time for the scaled copy into the
quad and 0.009 ms for the window overlay. In exchange the game draws the UI
once per frame instead of twice, and the eyes' post-processing samples a 1x1
fallback texture instead of the UI (both not timed: they are the game's own
GPU work). No extra full-screen copy is made: the UI texture is read once into
the quad image, and once into the window.

### Checking it

```powershell
tools\dev\launch.ps1 -Until gameplay -KeepRunning -Set "xr.backend=null;stereo.start_in_stereo=0"
tools\dev\send-input.ps1 -Pipe "stereo on"
tools\dev\send-input.ps1 -Pipe "capture $PWD\captures\ui\shot"     # eyes with the UI quad
tools\dev\send-input.ps1 -Pipe "ui off;capture $PWD\captures\ui\noui;ui on"   # the game's own composite
tools\dev\send-input.ps1 -Keys space        # command menu; m = main menu, esc closes
tools\dev\send-input.ps1 -Pipe "ui dump $PWD\captures\ui\uitex.png;uihook status;ui status"
```

Reach gameplay in mono (`stereo.start_in_stereo=0`): the harness recognises the
title and main menu from the window, which in stereo shows an eye's crop of
the 3D scene behind the menu.

## Foveated rendering

In stereo the periphery of each eye is shaded at a lower rate than its centre
(fixed foveated rendering). A headset's lenses blur and compress the
periphery, and the eyes rarely turn more than 15 to 20 degrees away from
straight ahead, so the outer parts of the eye image can be shaded once per
2x1 or 2x2 pixels with little visible difference while saving pixel shader
work. D3D11 has no vendor-neutral variable rate shading; this uses NVIDIA's
through NVAPI (`src/render/src/foveation.cpp`, NVAPI SDK R615, MIT, fetched
into `third_party/_fetched/nvapi-src`). On a GPU or driver without it, the
feature switches itself off with one log line and nothing else changes.

### How it works

- **Shading-rate surface.** One `R8_UINT` texel per 16x16 pixel tile covers
  the side-by-side scene targets. Each tile holds a ring index: full rate
  inside the inner radius, then two rings, then everything beyond, plus an
  index for tiles the headset cannot show (below). A table maps the indices
  to NVIDIA shading rates. Tiles that overlap both eyes (an eye width that is
  not a multiple of 16) take the finer rate of the two.
- **One centre per eye, at the eye's optical centre.** The ring distance is
  measured from where the eye's view axis meets the image, which is off-centre
  because headset FOVs are asymmetric (left eye of the Null backend's Quest 3
  class FOV: at 58.7 % of the width, 46.5 % of the height; the right eye
  mirrored). Distances are angle tangents from the view axis in units of half
  the eye's width, so a ring is a circle of constant angle whatever the
  asymmetry. The centres and the rects are taken from the views the engine
  actually renders (view rect and projection of each eye in `FViewInfo`), so
  they always match the image.
- **Which draws get it.** The engine module marks where the scene of a stereo
  frame starts and ends, in the order of the frame's GPU work: hooks on
  `FDeferredShadingSceneRenderer::Render` and `FPostProcessing::Process` (and
  the in-game UI pass) append RHI commands that run on the RHI thread just
  before the scene's first draw (with both eyes' rects and projections) and
  just before the UI pass and post-processing (`src/engine/src/ui_layer.cpp`,
  render.h "FIXED FOVEATED RENDERING"). Hooks on the immediate context's
  `OMSetRenderTargets` and `OMSetRenderTargetsAndUnorderedAccessViews` then
  switch variable rate shading on while, inside that window, render target 0
  has the scene targets' size (the eye rects' extent, which the engine rounds
  up to a multiple of 4) and is not a skipped format, and off for every other
  binding. The Present hook switches it off before any of this module's own
  work.
- **Never affected:** mono frames, screen mode and the plain game (no stereo
  view family, so no scene is marked), the in-game UI pass (after the scene
  window), the post-processing chain including temporal AA, bloom and the
  tonemapper, the desktop mirror and the XR copies (after the scene window),
  half and quarter resolution buffers, shadow maps, depth-only passes and
  compute work (wrong size, no colour target, or not affected by VRS at all).
  The desktop mirror shows a crop of the left eye, so the eye image itself is
  foveated there too; nothing is drawn coarsely into the window.

### What the scene does per frame and what takes the mask

`fov trace` logs every render target binding of one stereo frame from the
scene's start to Present, with its size, format, number of targets and GPU
time. The first room of the benchmark save at 2 x 2496x2592 (Null backend,
balanced preset), 7.04 ms of GPU time inside the scene window:

| Binding (scene targets 4992x2592) | GPU ms | Mask |
|---|---|---|
| depth-only passes (shadow depths, depth prepass) | 1.17 | no colour target |
| work with no render target bound (compute, unordered access) | 1.54 | not affected by VRS |
| targets of other sizes (shadow and lighting atlases, mip chains, half resolution) | 0.96 | other size |
| volume targets (translucency lighting) | 0.79 | not a 2D target |
| G-buffer base pass (6 targets, `R16G16B16A16_FLOAT` first) | 0.54 | yes |
| velocity (`R16G16_UNORM`) | 0.03 | skipped by default |
| small full-size passes (`R8_UNORM`, `R8G8_UNORM`) | 0.23 | yes |
| screen shadow mask (`B8G8R8A8_UNORM` with depth) | 0.20 | yes |
| lights, reflections, fog and translucency into scene colour (`R16G16B16A16_FLOAT`) | 1.59 | yes |

So 2.6 ms of the scene's 7.05 ms is shaded on targets the mask applies to;
the rest (depth-only work, compute, other sizes, volumes) is not pixel shading
on the scene targets and cannot gain from it. That bounds the saving: the
`balanced` preset removes about half of those 2.6 ms. Post-processing after the scene costs
about 0.9 ms at this size and is left at full rate.

Passes that write data later passes read per pixel:

- the **velocity buffer** is skipped by default (`skip_formats = 35`): it is
  temporal data that temporal AA and motion blur read per pixel, and shading
  it at full rate costs 0.03 ms;
- **temporal AA and everything after the scene** are outside the window;
- **depth** is never coarse: VRS only changes how often the pixel shader runs,
  depth testing and writing stay per pixel, and depth-only passes have no
  colour target;
- the **G-buffer pass** takes the mask. Leaving it at full rate
  (`passes = no-gbuffer`) gave back 0.2 of the 1.34 ms saved by the balanced
  preset and did not remove the visible stair-steps on bright edges, which come
  from the lighting passes as much as from the base pass.

### Settings

| Key | Default | Meaning |
|---|---|---|
| `[foveation] enabled` | `1` | `0`: never used in this session (the scene markers pass straight through) |
| `[foveation] preset` | `quality` | `quality`, `balanced`, `performance` or `off` (table below) |
| `[foveation] radii` | preset | three increasing ring radii, fractions of half the eye width from the optical centre, e.g. `0.7 0.9 1.15` |
| `[foveation] rates` | preset | shading rate between the first and second radius, between the second and third, and beyond: `1x1`, `2x1`, `1x2`, `2x2`, `4x2`, `2x4`, `4x4` (pixels per shading sample, width x height) |
| `[foveation] hidden_area` | `coarse` | tiles inside the runtime's hidden area mesh: `coarse` (4x4), `cull` (not drawn at all), `off` (treated like the outer ring) |
| `[foveation] passes` | `scene` | `scene`: matching targets inside the scene window; `no-gbuffer`: the same without the G-buffer pass (3 or more targets); `all`: matching targets from the scene's start until Present, including post-processing (for comparison only) |
| `[foveation] skip_formats` | `35` | DXGI formats of render target 0 that never get the mask (35 = `R16G16_UNORM`, the velocity buffer) |
| `[debug] foveation_unsupported` | `0` | `1`: behave as on a GPU without variable rate shading (tests the fallback) |

Presets (radius 1.0 is half the eye width; with the Null backend's Quest 3
class FOV that is a tangent of 1.09, so 0.70 is about 37 degrees from the view
axis, 0.90 about 44, 1.15 about 51; the image reaches about 1.6 in its outer
corners):

| Preset | Full rate inside | Then | Then | Beyond | Pixels at full rate | Pixel shading work |
|---|---|---|---|---|---|---|
| `quality` (default) | 0.70 | 2x1 to 0.90 | 2x2 to 1.15 | 2x2 | 37.6 % | 57.6 % |
| `balanced` | 0.55 | 2x2 to 0.80 | 2x2 to 1.05 | 4x4 | 23.3 % | 38.4 % |
| `performance` | 0.45 | 2x2 to 0.65 | 4x4 to 0.90 | 4x4 | 15.6 % | 23.5 % |

Shares inside both eye rects with the Null backend's FOV and hidden area
(8.3 % of the pixels, shaded 4x4); the work column is the pixel shader
invocations relative to full rate on the masked targets.

**Why `quality` is the default.** Captures at headset resolution (first room,
still and with the head turning, and outdoors) compared with the same view at
full rate: the fovea is pixel-identical (difference maps are black inside the
inner ring apart from animated characters, as between two full-rate captures);
`quality` shows only a slight stair-step on the brightest light bar at the very
edge of the image. `balanced` makes the edges of bright light fixtures and
high-contrast texture detail visibly stair-stepped from about 31 degrees off
axis and softens wood grain and corrugated metal; `performance` makes them
blocky (4x4) from about 36 degrees. Judged on the full eye image, which is
harsher than what the lenses show; not checked in a headset yet. Temporal AA
over coarse tiles: no smearing or trailing in the captures with the head
turning, compared with full rate at the same head angle. Bloom and exposure:
mean brightness changes by under 1 % (`balanced`: 25.05 to 24.88). Right eye:
the same as the left, no artefact of its own; nothing at the seam between the
eyes.

### The parts of the image the headset cannot show

OpenXR reports them as a triangle mesh per eye (`XR_KHR_visibility_mask`,
`IXrBackend::GetHiddenAreaMesh`; the Null backend emulates one: everything
outside an ellipse around the view axis 5 % larger than the FOV's half-extents,
8.3 % of the image). Tiles entirely inside it get their own index:

- `coarse` (default): shaded once per 4x4 pixels. Their content stays
  plausible, so nothing differs where it can bleed into the visible image.
- `cull`: not rasterised at all. The culled pixels keep whatever the targets
  held before (in the captures: stale red content from another pass, not
  black), and the passes after the scene read them: bloom spreads them into
  the visible image, temporal AA can pull them in at the edge when the head
  turns, and auto exposure includes them. For at most 8 % of the pixels at
  1/16 of the cost already, that is not worth it, so culling is not the
  default.

SteamVR's null driver reports an empty mesh (0 triangles), so there the outer
ring covers the corners. Virtual Desktop's mask has not been seen yet.

### Cost and savings

GPU time of the scene window (`gpu scene` in the timing block, timestamps at
the scene markers), first room, Null backend unpaced, 2 x 2496x2592, all
settings in one session, p50 over about 500-1000 frames each:

| Setting | Scene GPU ms | Saved |
|---|---|---|
| off | 7.71 (again at the end: 7.71) | - |
| `quality` | 6.76 | 0.95 ms (12 %) |
| `balanced` | 6.38 | 1.34 ms (17 %) |
| `performance` | 6.12 | 1.59 ms (21 %) |
| `balanced`, `passes = no-gbuffer` | 6.58 | 1.14 ms |

The context hooks cost 0.02 ms of CPU per frame on the RHI thread (about 95
render target bindings in the scene window per frame, 16 to 17 of them get
the mask). Frame times measured with the benchmark harness are in
`docs/benchmarking.md` style below.

BENCH_TABLE_PLACEHOLDER

### Foveation dev commands

| Command | Effect |
|---|---|
| `fov status` | settings, state, stereo frames and bindings so far, the surface's ring shares and the optical centres |
| `fov on` / `fov off` | switch the mask; after `fov off` the scene's GPU time keeps being measured, so on and off compare in one session |
| `fov preset <name>`, `fov radii <a> <b> <c>`, `fov rates <a> <b> <c>`, `fov hidden off\|coarse\|cull`, `fov passes scene\|no-gbuffer\|all`, `fov skip [formats]` | change the settings at run time (the surface is rebuilt at the next stereo frame) |
| `fov trace` | log every render target binding of the next stereo frame, from the scene's start to Present, with GPU times |
| `fov timing` | log and reply the scene GPU time, the GPU time after the scene and the hooks' CPU time since the last report |

Log lines to look for:

```
foveation: scene markers installed (Render +0x21e64a0, FPostProcessing::Process +0x251c230, ...)
foveation: variable rate shading available (driver 610.47, r610_45), context hooks installed
foveation: eye views: rect at +0x80 (2064x2208), projection at +0xe0 (...)
foveation: surface 259x139 tiles for 4128x2208; pixels: full 37.6 %, ...; left eye ... optical centre at (1212, 1027) ...
foveation: off for this session: <reason>          (unsupported GPU or driver, NVAPI missing, an NVAPI call failed)
```

## First test on a Quest 3 through Virtual Desktop

Everything above was verified with the Null backend and SteamVR's null driver;
nothing has run on a real headset yet. The defaults are set for this first
test (backend `openxr`, runtime `virtualdesktop`, screen mode). Start Virtual
Desktop's Streamer on the PC, connect the headset in Virtual Desktop, then
start the game normally. Check, in this order:

1. **Session.** `ff7vr.log` (next to the DLL) shows
   `xr: session created ... runtime 'VirtualDesktopXR' ...` and
   `xr: frame loop running (state Focused ...)`. If it shows
   `no session (SystemUnavailable ...)` instead, the headset was not
   connected in Virtual Desktop; connecting it later is enough, the module
   retries every 5 s.
2. **Layers.** The `xr:   implicit API layer` lines show ReShade's layer as
   `disabled for this process` and Virtual Desktop's as `enabled`.
3. **Where the screen is.** It should appear straight ahead at eye height
   when the session starts. If it is off to the side or too high/low, use
   Virtual Desktop's recenter or the dev pipe's `recenter` and check whether
   it then sits in front of you.
4. **Size and distance.** 1.8 m at 2 m. Adjust `[screen] width`,
   `distance`, `offset_y` until the HUD corners are readable without turning
   the head.
5. **Image.** Sharp, not too dark or washed out compared with the desktop
   window. The back buffer is 10-bit; the screen layer converts it. If the
   game's HDR output is enabled, the back buffer holds HDR10 values and the
   screen will look wrong: screen mode assumes SDR output.
6. **Smoothness.** Head movement must feel smooth even when the game runs
   below the headset's refresh rate (the quad is re-projected by the
   runtime). The `timing:` lines should show the game's usual frame rate,
   `errors 0`, and a `present hook` average below about 0.5 ms. If the hook
   takes several milliseconds, the `runtime calls` line tells which runtime
   call blocks (SteamVR's null driver blocks in `begin frame` at times, see
   [Measured overhead](#measured-overhead)); compare against a run with
   `[xr] enabled = 0`.
7. **Refresh rate and resolution.** The session line reports the eye size and
   refresh rate Virtual Desktop chose (its quality preset and the headset's
   72/80/90/120 Hz). Screen mode does not depend on them.
8. **Disconnect and reconnect.** Disconnect the headset in Virtual Desktop
   while the game runs: the log should show the session ending
   (`xr: ending the session`) and the game keep running on the desktop;
   reconnect: a new `session created` within about 5 s. Quitting the game
   from the VR dashboard ends the session and keeps it off until
   `xr-restart` (or set `[xr] reconnect_after_exit = 1`).
9. **Exit.** Quitting the game normally logs `render: ExitProcess: ending the
   XR session` and `session ended`; Virtual Desktop returns to its own view.
10. **Stereo** (once the engine module's stereo device is enabled): menus and
    loading screens should switch to the screen and back without a black or
    frozen frame (see [Switching](#switching-between-stereo-and-the-screen)).
11. **UI layer**: in stereo the HUD floats about 3 m ahead as one flat panel,
    complete to its corners, and is not doubled into the 3D image; Space opens
    the command menu on it. Check that it is sharp and comfortable to read; if
    it is too large or too small adjust `[ui] size` / `distance` (or `ui size
    <m>` live), and compare markers over enemies with `[ui] size = 1.57`
    ([World-anchored elements](#world-anchored-elements)).
12. **Foveated rendering** (on by default, `quality`): look straight ahead and
    let the eyes wander to the edges of the view. The image should look as
    sharp as before up to well beyond comfortable eye movement; bright edges
    (lamps, light fixtures, sky behind railings) at the very edge may look
    slightly stepped. Compare with `[foveation] enabled = 0` (or `fov off`
    through the dev pipe) and try `fov preset balanced`. The log lines
    `xr: eye 0 hidden area mesh: N triangles` tell whether Virtual Desktop
    reports the region the lenses cannot show.
