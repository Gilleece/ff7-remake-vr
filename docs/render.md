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
| `[xr] runtime` | `auto` | `auto` (the first runtime with a headset, see "Runtime selection" below), `virtualdesktop`, `steamvr`, `system`, `inherit` or a path to a runtime JSON (see `docs/testing-headless.md`). The machine's default OpenXR runtime is never changed |
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

### Runtime selection

`[xr] runtime` picks the OpenXR runtime for the game process only: it sets
`XR_RUNTIME_JSON` in the process and passes the same path to the loader as a
loader property (`xrInitializeLoaderKHR`). The registry is only read.

With `auto` (the default) every attempt (`ff7vr xr` thread, every
`retry_interval` seconds until a session exists) builds a candidate list
(`EnumerateRuntimeCandidates`, `src/xr/src/runtime_select.cpp`):

1. sources: `ActiveRuntime` under `HKLM\SOFTWARE\Khronos\OpenXR\1`, every
   enabled value under `...\AvailableRuntimes`, and the install folders of
   runtimes that may not be registered: Virtual Desktop
   (`%ProgramFiles%\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json`),
   SteamVR (`openvrpaths.vrpath`, then the Steam folder), PICO
   (`%ProgramFiles%\PICO Streaming Service\openxr_runtime_pc\PicoStreamingXRRuntime\picostreaming-openxr.json`),
   Meta Quest Link (`Base` of `HKLM\SOFTWARE\Oculus VR, LLC\Oculus`, else
   `%ProgramFiles%\Oculus`, then `Support\oculus-runtime\oculus_openxr_64.json`),
   Windows Mixed Reality (`%SystemRoot%\System32\MixedRealityRuntime.json`) and
   Pimax (`%ProgramFiles%\Pimax\Runtime\PiOpenXR_64.json`). The first three were
   seen on a test PC; the others are the vendors' documented locations, not
   tried. PimaxXR, Varjo and Monado are recognised by their manifest names when
   registered. Duplicates (same file) are dropped;
2. order: runtimes whose process runs (`VirtualDesktop.Streamer.exe`,
   `vrserver.exe`, `OVRServer_x64.exe`, `PICO Connect.exe`,
   `MixedRealityPortal.exe`), then the active runtime, then the rest;
3. each candidate is probed (`ProbeRuntime`, `src/xr/src/runtime_probe.cpp`):
   loader pointed at its manifest, extension list (must offer
   `XR_KHR_D3D11_enable`), `xrCreateInstance`, `xrGetSystem` for a
   head-mounted display, `xrDestroyInstance`. The first that returns a system
   wins and the normal initialisation runs with it.
   `XR_ERROR_FORM_FACTOR_UNAVAILABLE` (no headset), load failures and other
   errors move on to the next;
4. not probed: SteamVR, Meta Quest Link and Windows Mixed Reality while their
   process is not running, unless they are the active runtime (loading their
   runtime starts their server, which a game is expected to do only for the
   PC's default); PICO while PICO Connect is not running, also when active,
   because its runtime (1.1.46) answers `xrGetSystem` with a system called
   `pico` when no PICO headset was ever connected, so its answer says nothing.

Switching runtimes inside one process works with the static loader
(OpenXR-SDK 1.1.63): the loader keeps the runtime library it loaded until the
last `XrInstance` is destroyed (`LoaderXrDestroyInstance` calls
`RuntimeInterface::UnloadRuntime`) or until `xrInitializeLoaderKHR` is called
while no instance exists (`InitializeLoaderInitData` unloads it and replaces
the loader properties); the next call that needs a runtime reads
`XR_RUNTIME_JSON` again (`RuntimeManifestFile::FindManifestFiles`). Verified
with `ff7vr_runtime_probe` (target in `src/xr/CMakeLists.txt`, not built by
default): Virtual Desktop, PICO, Virtual Desktop, PICO and then SteamVR,
Virtual Desktop, SteamVR, Virtual Desktop, SteamVR in one process each
reported its own runtime name (`VirtualDesktopXR` 1.0.10, `PICO XR Runtime`
1.1.46, `SteamVR/OpenXR` 2.18.2).

Log lines (they stay at info level during repeated attempts):

```
xr: OpenXR runtime (auto): skipped Virtual Desktop (...virtualdesktop-openxr.json): no headset connected (XR_ERROR_FORM_FACTOR_UNAVAILABLE) [36 ms]
xr: OpenXR runtime (auto): chose SteamVR - runtime 'SteamVR/OpenXR' 2.18.2, headset 'SteamVR/OpenXR : null', manifest ...steamxr_win64.json (vrserver.exe running; probe 2199 ms)
xr: OpenXR runtime (auto), round 1: no headset found: Virtual Desktop (...): no headset connected (...) [37 ms]; SteamVR (...): not running (looked for vrserver.exe); loading it would start it; PICO (...): not running (...); its runtime reports a headset even when none is connected
```

A round line is repeated when its text changes, otherwise at rounds 2, 4, 8,
...; the others go to debug level. A round costs one instance per probed
runtime (Virtual Desktop without a headset: 36 to 80 ms). The winner's instance
is created twice (probe, then the session); with SteamVR each takes about 2.2 s
in the game process.

Known limits: a runtime that reports a system without a headset (like PICO's)
would win if it were probed; a session, once created, is kept, so connecting
a different headset later needs `xr-restart` or `xr-runtime auto`; the
process names that mark a runtime as running for Meta Quest Link, PICO and
Windows Mixed Reality are untested.

### Implicit API layers

OpenXR loads every implicit layer registered on the
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
| no headset (`SystemUnavailable`: Virtual Desktop running without a connected headset returns `XR_ERROR_FORM_FACTOR_UNAVAILABLE`; with `auto`, at least one runtime loaded but none had a headset) | logged once, retried every `retry_interval` seconds; later failures are logged at 2, 4, 8, ... attempts |
| runtime missing (`RuntimeUnavailable`: the runtime JSON or its DLL does not exist; with `auto`, no candidate loaded at all) | same, at least every 30 s |
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

### The headset's own recenter

The mod's recenter (End, `recenter`, and once at session start with
`[screen] recenter_on_start`) is an offset on top of the runtime's LOCAL space.
A headset can also recenter by itself (holding the Meta button on a Quest,
SteamVR's reset view). OpenXR then makes "the current leveled head space the
new LOCAL space" and queues `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`
for LOCAL; located poses follow the new origin for display times from the
event's `changeTime` on. `poseInPreviousSpace` (the new origin in the old space)
is optional: Virtual Desktop's runtime (1.0.10 source) never gives it
(`poseValid` false, `changeTime` = the moment it noticed the recenter) and sends
a second event for STAGE.

The backend keeps the LOCAL event and, at the first frame whose predicted
display time reaches `changeTime`, clears its own recenter offset: the new
origin already is where the user faces, and the old offset would turn the view
away again (recomposing it with `poseInPreviousSpace` would keep the old
forward direction, which undoes what the user asked for). World-locked quads
(the virtual screen, the UI panel) are placed relative to that offset, so they
come back in front of the user in the same frame; head-locked quads use VIEW
space, which a recenter does not move. STAGE and other spaces are ignored (not
used). One line is logged:

```
xr: runtime recentred its LOCAL space (event 1 of this session, applied at frame 4515, 33 ms after the event; pose of the new origin in the old space yaw 70.0 deg, position (0.100, 0.000, 0.000) m): recenter offset was yaw 30.0 deg, position (0.100, 0.000, 0.000) m -> cleared; head in the new space yaw -0.0 deg, position (0.000, 0.000, 0.000) m
```

Frames already waited before the change (one or two) keep the old offset;
their images are still submitted with the poses they were rendered with.

### Lost tracking (pose validity)

`xrLocateViews` (`viewStateFlags`) and `xrLocateSpace` (`locationFlags`) say
whether orientation and position are valid; the values of an invalid part are
undefined. Every frame's views and head pass through one filter in the backend
(`BackendBase::SanitizePoses`, both backends):

| Runtime reports | Views and head given to the game and the layers |
|---|---|
| orientation and position valid | as located |
| orientation valid, position not (3DoF) | the runtime's orientation; the head at the last fully tracked position, each eye at its last known offset from the head (half the IPD before any) |
| orientation not valid (or any value not finite) | the last views and head that had a valid orientation, held; the recenter basis is not updated |

Changes of state are logged once each (`xr: tracking: not tracked: last views
held ... from frame 7715 (after 7714 frames tracked ...)`, `xr: tracking:
tracked from frame 8165 (after 450 frames ...)`); `status` adds `tracking full|3dof|held`
and the frame counts. The projection layer is submitted whenever the poses are
real or held (a held image stays where it was rendered), not with the neutral
stand-in used before any orientation was seen. The game thread
(`BeginGameFrame`) checks once more that every pose is finite and a unit
rotation and otherwise repeats the last views given to the game.

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
| `xr-sim status` | Null backend: emulated tracker head, LOCAL origin, head in LOCAL, recenter offset, the head the game sees, validity |
| `xr-sim head <yaw deg> [pitch deg] [x y z m]` | Null backend: sets the emulated head pose (on top of `[xr] null_motion`) |
| `xr-sim recenter-event [nopose] [delay <frames>]` | Null backend: the headset's own recenter: LOCAL moves to the current leveled head and a LOCAL change event is sent, with `poseInPreviousSpace` or without (`nopose`, like Virtual Desktop), change time `delay` frames ahead (default 3) |
| `xr-sim lose orientation\|position <frames>` | Null backend: the next frames report that part invalid, with NaN values |
| `xr-sim gaze <yaw> <pitch>`, `gaze off`, `gaze sweep [radius deg] [period s]`, `gaze blink <frames>`, `gaze status` | Null backend with `[foveation] eye_tracking` on: a simulated eye tracker (fixed gaze in head space, yaw + right and pitch + up; not tracked; a circle around the view axis; a short loss) ([Eye-tracked foveation](#eye-tracked-foveation)) |

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
- A prefix ending in `+raw` (for example `capture C:\...\eyes+raw`) also
  writes the bytes the runtime is handed, read back with no view or colour
  conversion, as RGBA PNGs: `<prefix>_rawL.png` / `_rawR.png` (each eye's
  swapchain image as stored, alpha included), `<prefix>_rawquad<i>.png` (each
  quad layer's image) and `<prefix>_src.png` (the frame's source texture;
  `R10G10B10A2` is reduced to 8 bits). The `+raw` is removed from the file
  names. It adds a few read-backs to the capture frame.

### What the runtime receives (measured)

Measured on the Null backend at 3072x3264 per eye in the room where the
latest save starts (first and third person, UI layer on and off):

- Eye swapchain images (`R8G8B8A8_UNORM_SRGB` view on a typeless texture)
  hold the engine's eye image byte for byte: every pixel of `_rawL`/`_rawR`
  is within 1 of the source's 10-bit value reduced to 8 bits (mean
  difference +0.12 to +0.15, from rounding). Alpha is 255 everywhere.
- The UI quad's image is premultiplied (alpha linear, colour stored
  sRGB-encoded as the format requires). During plain exploration 1.6 % of its
  pixels have any alpha; about 0.1 to 0.3 % carry colour where alpha is 0 or
  colour above alpha after decoding (additive UI glows, which a
  premultiplied blend adds on top). It cannot change the picture as a whole.

### What Virtual Desktop's OpenXR runtime does with it

Read in the public source of VirtualDesktop-OpenXR at the 1.0.10 release
(commit `f039941`, "Prepare for release 1.0.10"):

- `xrCreateSwapchain` (`swapchain.cpp` 335-403) turns the requested DXGI
  format into the same LibOVR format (`R8G8B8A8_UNORM_SRGB` stays sRGB) and
  always asks for `ovrTextureMisc_DX_Typeless`. A 2D, single-sample swapchain
  with array size 1 (ours) is a LibOVR swapchain used directly: no copy
  (`d3d11_native.cpp`, `resolveSwapchainImage`: a copy happens only for array
  slices above 0 or slow-path swapchains).
- Pre-processing (`frame.cpp` 980-1005, `AlphaBlendingCS.hlsl`) runs only for
  layers above the first: it clears alpha when a layer lacks
  `BLEND_TEXTURE_SOURCE_ALPHA` and premultiplies when it has
  `UNPREMULTIPLIED_ALPHA`. Our projection layer is layer 0 and our quads are
  premultiplied with the blend flag, so neither applies.
- Upscaling and sharpening (`precompositor.cpp`) run only when the registry
  values `upscaling` or `sharpen` under the runtime's key are set; its log line
  `Recommended resolution: ... (1.000 supersampling, 1.000 upscaling)`
  shows when they are not. They handle sRGB correctly when they do run.
- Depth: a projection layer becomes `ovrLayerType_EyeFovDepth` only when a
  `XrCompositionLayerDepthInfoKHR` is chained (`frame.cpp` 708-730). The mod
  enables the extension but chains no depth, so nothing changes.
- No gamma, brightness or colour setting and no per-application quirk that
  matches this game. The projection layer's flags are not read at all.

So for this submission the runtime passes the swapchain textures unchanged,
tagged sRGB, to Virtual Desktop's own (closed) compositor and encoder. Any
difference between a capture and the headset arises after that point, the
same point every other OpenXR application goes through.

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
| With ReShade/Luma as `dxgi.dll` | the game presents ReShade's swap chain proxy (see "ReShade and Luma") |

The screen layer's swapchain is created in the sRGB variant of the back
buffer's format family the runtime offers (`R8G8B8A8_UNORM_SRGB` on the Null
backend and SteamVR), and the 10-bit back buffer is converted by a shader blit
that decodes its sRGB values, so brightness matches the desktop.

## D3D11 hooks

All hooks are installed from `start()` on the loader's bootstrap thread:

- `Present`, `Present1`, `ResizeBuffers`, `ResizeBuffers1`: inline hooks on
  the functions DXGI's swap chain vtable points to (found through a throwaway
  swap chain on a NULL-driver device); the vtable slots themselves are left
  alone. Every swap chain the game creates is checked and a class whose
  functions are not hooked yet gets its own hooks. See "Coexisting with other
  Present hooks" for why these are not slot hooks.
- `IDXGIFactory::CreateSwapChain`, `IDXGIFactory2::CreateSwapChainForHwnd`,
  `D3D11CreateDevice`, `D3D11CreateDeviceAndSwapChain`,
  `ID3D11Device::CreateDeferredContext`: inline hooks, for logging facts only.
- A vectored exception handler that records the thread names the engine
  announces (exception `0x406D1388`), so logs can name the render thread.

The main swap chain is the largest one with a visible window among those that
presented during the last second. No reference to a back buffer is kept beyond
a Present call, so the game's `ResizeBuffers` always succeeds; window mode and
size changes recreate the screen layer at the new size on the next Present.

### Coexisting with other Present hooks

Until October 2026 the four functions were hooked by replacing their vtable
slots. Started by Steam (so with `gameoverlayrenderer64.dll` injected at
process start), the game then died on its first frame with a stack overflow
inside the overlay; started directly it did not, and with the render module
off it did not. What happened, from the crash dump's stack and the overlay's
code (`gameoverlayrenderer64.dll` 10.96.30.42):

1. The overlay is loaded before the mod and patches the body of DXGI's
   `CDXGISwapChain::Present` (`dxgi.dll+0x19530`) with a jump to its hook
   (the hook function starts at `+0x93e50`; it calls its saved original
   through a pointer at `+0x167340`, the call returns to `+0x93f6f`).
2. The mod replaced the vtable slot with its own detour, keeping
   `dxgi.dll+0x19530` (now the overlay's jump) as its original.
3. At the game's first Present the overlay's hook is entered straight from the
   game (`ff7remake_.exe+0x1f492ca` -> overlay `+0x93f6f`), calls its original,
   which is now the mod's detour (`XINPUT1_3.dll` frame), which calls
   `dxgi.dll+0x19530`, which jumps into the overlay's hook again; from there on
   only overlay frames repeat (`+0x93f6f` every 0x50 bytes of stack): the mod's
   detour passes nested calls straight on with a tail jump. So the overlay
   hooked Present a second time when it found the slot changed and kept one
   original for both of its hooks. Which exact address the second hook patched
   (the slot or the mod's function) was not established; the loop through the
   saved original is what the stack shows.

Now the slots keep pointing to DXGI's functions and the mod puts MinHook inline
hooks on those functions. The first Present logs the chain
(`d3d11:   Present chain: vtable ... slot -> ...; our original -> ...`), with
every jump at the start of a function followed:

| Start | Chain logged |
|---|---|
| through Steam (`steam -applaunch`), runs `captures/render2/steam1`, `steam2` | slot -> `dxgi.dll+0x19530` [jump] -> MinHook relay -> mod's detour; mod's original -> trampoline -> jump -> overlay `+0x93e50`: the overlay patched DXGI's body first, and the mod's hook runs before it |
| direct (launcher, harness) | slot -> `dxgi.dll+0x19530` [jump] -> MinHook relay [jump] -> overlay `+0x93e50`: the overlay is loaded later (when the game initialises Steam) and hooks in front of the mod's hook |

Both orders chain: each hook calls the code that was at the function's start
before it was patched. Two starts through Steam reached gameplay in 3D on the
Null backend (37 and 40 s, eye captures in both runs, no crash, no warning);
the launcher path reached gameplay in 3D with the same build
(`captures/render2/launcher`). A hook that replaces the vtable slot (as UEVR or
ReShade's proxy do) calls DXGI's body and so still reaches the mod's hook.

The benchmark's frame timer (`src/dev/frame_timer.cpp`, `[bench] frame_timer`)
hooks the same body through the same hook library, which hooks a function only
once; with the render module on it logs `MH_ERROR_ALREADY_CREATED` and then
uses the vtable slot instead (`frame_timer: Present's body is already hooked
...`), checked with `bench status` / `bench start` / `bench stop` in a direct
start (`captures/render2/024050-final`). Started through Steam, that slot hook
would meet the overlay as described above.

### ReShade and Luma

With ReShade 6.7.1 and the Luma add-on for this game installed as the game's
`dxgi.dll`, the module's start-up used to stop for good at the throwaway swap
chain: ReShade redirects `D3D11CreateDeviceAndSwapChain` and
`IDXGIFactory::CreateSwapChain` and sets up its runtime (with Luma's add-on) on
every swap chain created through it, also the 64x64 one on the NULL driver,
and that set-up never returned. A minidump of the blocked thread
(`captures/render2/reshade1/game.dmp`, made from outside while it hung) shows
the start-up thread inside user32, called from
`Luma-Final Fantasy VII Remake.addon+0x1dd2d`, called from ReShade's
`CreateSwapChain`, called from the module's probe; ReShade's own log
(`ReShade.log` in the game folder) ends at `Running on  Driver 92.78.` for
that swap chain. The game itself was not blocked (it reached the title screen
flat).

Now, when the loaded `dxgi.dll` is not the one in the system directory, there
is no throwaway swap chain: the module creates a factory with that
`dxgi.dll`'s `CreateDXGIFactory1` (no device), hooks the functions its
`CreateSwapChain` and `CreateSwapChainForHwnd` slots point to, and hooks
`Present` when the game creates its swap chain (`render: dxgi.dll is ... (not
the system's): no throwaway swap chain`). The swap chain the game gets from
ReShade is ReShade's proxy, so the hooked `Present` is the proxy's
(`swap chain class (vtable dxgi.dll+0x422070)`, inside ReShade's `dxgi.dll`);
the module runs before ReShade's own Present work.

What was seen with ReShade and Luma loaded (Null backend, eyes 2064x2208, runs
`captures/render2/reshade2`, `reshade3`, `steam-reshade`):

| | Result |
|---|---|
| Start-up, direct start and start through Steam (overlay too) | the module initialises, the XR session runs, gameplay reached in stereo, no warning or error in the module's log; ReShade's log shows Luma loaded on the game's device |
| Eye images, without the tonemapping shift below | BOTH EYES SHOW THE LEFT EYE'S IMAGE (left and right eye captures differ by 0.7 to 0.8 of 255 on average, no parallax; without ReShade 22.6 and a 368-pixel shift) |
| Where | one-frame GPU trace with read-backs (`reshade3/tr`): the two views' anti-aliasing outputs differ as they should (events 2702 and 2735, mean difference 25.8 between the left half and the right half), but the right view's tonemapping pass (2766, viewport at x 2064, reading the right view's anti-aliasing output as `t0`) writes the left view's image (mean difference 0.14 from the left view's tonemapping output, 2733). Without ReShade this pass is correct, so the pass Luma puts in place of the game's tonemapping (Luma replaces shaders when the game creates them) reads its input relative to the origin of the target and ignores the view's offset: the same kind of fault as the game's bloom, occlusion and reflections. Several bloom passes also run Luma's shaders (`ps` objects created later than the game's) |
| Not tested | ReShade's effects on the virtual screen; ReShade's OpenXR layer (the Null backend does not use the OpenXR loader; the module keeps that layer disabled for the process with a real runtime); Luma's DLSS in VR; performance |

`[stereo] tonemap_shift` (`src/engine/src/bloom_fix.cpp`, `tonemap_shift`;
`auto` by default, `0` off, `1` always; dev command `tonemapshift [0|1|2]`)
runs the right view's tonemapping draw with its input 0 (the anti-aliased
scene colour) copied from the view's rectangle to the origin of a scratch
texture, the bloom fix's mechanism. Recognised by shape: a full-screen draw
whose viewport starts at the middle of an `R16G16B16A16` target, input 0 of the
target's size, input 1 between a quarter and a half of it (the bloom result).
The game's own tonemapping shader reads at the view's rectangle, so the shift
must not run without Luma: `auto` applies it only while a module named
`Luma-Final Fantasy VII Remake.addon` is loaded (checked every 600 stereo
frames; the log says `tonemap input shift: Luma add-on loaded`). Evidence:

| Run | Result |
|---|---|
| `reshade6` (8 captures, modes 2, 0, 0, 2, 2, 1, 0, 2, 2 s apart) | shift on: eyes differ by 21.6 to 23.0 with a 368 to 384-pixel parallax shift; off: 0.69 to 0.98, no shift. `applied` grows by about 400 per 4 s while on, by at most 1 while off |
| `reshade8` (render scale 0.8 and 1.0, modes 2 and 0) | the same at both scales (on: 22.7 / 22.9, parallax; off: 0.53 / 0.86); `reshade8/sheet.png`: left eye, right eye at 0.8, right eye at 1.0 |
| `025324-noreshade` (no ReShade) | `auto (inactive; Luma add-on not loaded): applied 0`; eyes 22.4 apart with the usual parallax |
| `reshade5` counters with Luma | bloom fix, occlusion fix and reflections fix applied once per stereo frame (542 each), `missed 0`, `failed 0` |

Not known: whether every Luma setting replaces the tonemapping shader (with
one that does not, `auto` would shift the game's own shader's input and break
the right eye: set `tonemap_shift = 0`); other Luma versions; whether other
Luma passes have the same fault in scenes not traced (one frame of the first
room was traced). Luma's own fix (taking the view rectangle into account)
would make the shift unnecessary.

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
  they always match the image. Where `FViewInfo` keeps them is searched for at
  the first stereo frame; a failed search (seen once with stereo on from the
  start, when the views of the first stereo tick did not have the eye rects
  yet) is repeated on every later stereo frame until it succeeds, with a
  warning at failures 1, 2, 4, 8 and so on, and foveation stays off until
  then. `[dev] foveation_layout_fail = N` makes the first N searches fail, to
  test this. The view rect is recognised in two forms, tried in this order:
  `(0, 0, W, H)` and `(W, 0, 2W, H)` (each eye fills its half), then
  `(0, 0, w, h)` and `(X, 0, X + w, h)` with `X > w` (a render scale below 1:
  each eye covers the top-left corner of its half, `X` being the half's
  width). Before the second form existed, a session that started with
  `[stereo] render_scale` or `[dlss] input_scale` below 1 never found the
  layout (`view rect (missing) or projection (+0xe0) not found`, every frame)
  and ran without foveation; starting at 1 and lowering the scale later worked
  because the layout is found once and kept. Verified at 3072x3264 per eye,
  scale 0.58 from the ini (Null backend): `rect at +0x80 (1784x1896, right eye
  at x 3072)` on the first stereo frame, no failed search, surface for
  6144x3265, rings centred at 58.7 % / 46.5 % of each scaled rect (the same
  shares as at full size). The same with DLSS (`[dlss] input_scale = 0.58`,
  `[foveation] preset = performance`, stereo on from the start): the layout
  found on the first stereo frame, then 10 minutes of walking, turning, Insert
  off and on every 2 minutes and a 150-degree head jump with foveation active
  throughout, no WARN or ERROR, 113,130 DLSS evaluations with no failure, no
  driver event.
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
- **Render scale below 1** (`[stereo] render_scale`, dynamic resolution;
  `docs/engine-module.md`): each eye covers only the top-left part of its half of
  the scene targets, which keep their size. The size rule then uses the targets'
  size, not the rects' extent: twice the right eye's offset wide, and as high as
  that half at the rects' aspect (both axes are scaled alike and rounded to 8
  pixels, so the height is accepted within 16 pixels). Without this the mask was
  silently off whenever the scale was below 1 (scale 0.85 at 2 x 3600x3600 saved
  only 0.7 ms instead of the 1.4 ms of 0.9 with the mask).
- **GPU frame time for others:** the scene and after-scene timestamps of each
  stereo frame are summed and offered through `render::GetGpuFrameTime` (render.h);
  the engine's dynamic resolution reads it. Only measured while foveation is
  initialised (an NVIDIA GPU, `[foveation] enabled = 1`).
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
- the **subsurface recombine** stays at full rate (next section).

### Skin edges: the subsurface recombine

Under 2x2 shading the edge between skin and dark cloth (the top of Tifa's
stockings in the first room, the edge of a skirt over a thigh) showed a thin,
broken line of grey-white pixels, one pixel wide, in both eyes. 2x1 does the
same on vertical edges (the side of a thigh against the skirt, fingertips) and
leaves horizontal ones alone. Full rate does not have it. It was seen only on
skin (subsurface materials); the other edges checked (hair against the bright
doorway, the floor, cloth against cloth) have no such line.

Found by keeping single render target bindings of the scene window at full rate
(`fov exclude <first> <last>`, binding numbers as `b<n>` in `fov trace`) with
everything else at 2x2, and cropping the same edge in each capture. The line
disappears only when one of two bindings is excluded: in the one-frame GPU
trace (`gpu trace`, first room) they are the passes after the subsurface blurs
(three draws on half-size `R16G16B16A16_FLOAT` targets per eye): a full-screen
pass into a scene-size intermediate target and the per-eye passes that write
scene colour back from it with the blurred result, the G-buffer and the scene
colour as inputs, no depth bound. Neither the G-buffer pass (`passes =
no-gbuffer` keeps the line) nor the light passes before them (bindings
`b81`-`b92` excluded: line still there) are involved.

Mechanism (inferred from the above, the shaders were not disassembled): the
lighting passes store skin's diffuse light without its base colour, in a
checkerboard pattern of diffuse and specular pixels, and the recombine
multiplies the blurred diffuse light by the base colour per pixel and
reassembles the checkerboard. With one shading sample per 2x2 block, the
recombine evaluates a block straddling the edge at a single position and writes
the result to all four pixels, so the dark cloth pixels next to skin receive
skin's light without the colour applied (grey-white), and the checkerboard is
read at the wrong parity. Both passes of the recombine chain have to be coarse
for the line to show.

The fix: a frame-local rule in the shading-rate hooks. Once the lights have
bound the screen shadow mask (`B8G8R8A8_UNORM` at the scene targets' size) and a
half-size `R16G16B16A16_FLOAT` target has been bound after that (the subsurface
blur), every scene-size `R16G16B16A16_FLOAT` binding without depth runs at full
rate, up to the end of the scene window. Translucency binds depth and keeps the
mask. In the first room this selects exactly the two bindings found above
(`fov trace` notes them `subsurface recombine at full rate`); in views without
characters (the alley) no subsurface blur runs and nothing changes.

Cost (scene GPU time, p50 of 8 s windows, 2 x 3072x3264, `quality` preset, two
rounds each, Null backend unpaced): first room 7.963 / 7.955 ms with the rule
against 7.932 / 7.925 ms without (+0.03 ms; `fov off` 9.435 ms); the same room
with the head turned 150 degrees 7.842 / 7.841 against 7.863 / 7.854 (no
measurable difference); the alley 6.59 / 6.62 against 6.61 / 6.52 (the rule
does not fire there; the spread is the noise of these windows, about 0.1 ms).
So the saving of the `quality` preset (1.5 ms in that room) is kept and the rule
is on by default. `[foveation] subsurface_full_rate = 0` or `fov subsurface 0`
switches it off.

Checked with the rings at the gaze (`xr-sim gaze sweep 20 3`, radii 0.30 / 0.45
/ 0.55 so the legs pass through the coarse rings): no line in four captures
with the rule, the line in the two of three captures without it where the legs
were in a coarse ring. Hair over skin and hair against the bright background
looks mottled at 2x2 with or without the rule; at the `quality` radii faces are
inside the full-rate zone.

**Hair at 2x2.** Hair has no pass of its own in the traced frame: it is drawn
in the G-buffer pass with everything else. With all rings at 2x2 (radii
0.01 / 0.02 / 0.03), the G-buffer pass alone at full rate (`fov exclude 53`)
leaves the hair as mottled as all 2x2, while the G-buffer pass alone at 2x2
(`fov exclude 54 200`) leaves it as clean as full rate. So the coarse coverage
of the hair geometry is not the cause; one of the passes after the G-buffer
(shadow projection, occlusion, the light passes) shades hair badly at 2x2.
2x1 shows no effect. Measured on the face in the first room, left eye, three
captures each (high-frequency luminance on the dark hair pixels, mean
absolute difference to a 5x5 median, and share of pixels 40 levels above it):
full rate 2.7 to 4.1 and 0.2 to 0.4 %; all 2x2 3.6 to 4.3 and 0.6 to 1.0 %;
G-buffer at full rate, rest 2x2, 4.1 to 5.1 and 0.8 to 1.2 %; only the
G-buffer at 2x2, 2.8 and 0.1 to 0.5 %; all 2x1 2.5 to 3.0 and 0.3 to 0.6 %.

Bisection with `fov exclude` (all rings 2x2, the face in the first room, crops
compared by eye because the measure above moves with the idle animation as
much as with the shading rate): excluding `b54`-`b107` cleans the hair, but no
single range does (`b54`-`b64`, `b65`-`b79`, `b80`-`b92`, `b93`-`b101`,
`b103`-`b107` each left it mottled, as did the single bindings `b68`, `b71`,
`b80`, `b81`, `b84`, `b85`, `b88`, `b94`, `b98`). The reverse test, only one
range coarse and the rest of `b54`-`b107` at full rate: `b54`-`b64` coarse or
`b93`-`b107` coarse leaves the hair clean, `b54`-`b79` coarse mottles it. So
two groups each produce it on their own: the shadow projection and first light
(`b65`-`b79`: the per-eye `R8G8_UNORM` passes, a scene-size
`R16G16B16A16_FLOAT` pass with depth and the screen shadow mask
`B8G8R8A8_UNORM`) and the further lights (`b80`-`b92`: shadow mask and light
accumulation pairs with depth). Occlusion (`b54`-`b64`), reflections and the
subsurface chain (`b93`-`b107`) are not involved.

Mechanism (inferred, shaders not disassembled): hair is a masked material whose
coverage, normal and tangent change from one pixel to the next (strand, gap,
strand). A light pass at 2x2 reads the G-buffer and the shadow mask at one
position per block and writes that light to all four pixels, so strands
receive the light and shadow of a neighbouring strand or of the gap behind it:
the highlights break into blocks and the gaps spread as dark speckles. Smooth
surfaces change slowly across a block, so the same sharing is not visible
there; the G-buffer pass itself at 2x2 does no harm because it still writes
per-pixel attributes (only the material evaluation is shared).

The fix is a switch, off by default because it is dear: `[foveation]
lighting_full_rate = 1` (or `fov lighting 1`) keeps every binding after the
G-buffer pass of the frame at full rate, so the mask covers the G-buffer pass
only. With all rings at 2x2 it gives hair like full rate (captures
`c22L` against `c22` in the test run). Cost (scene GPU time, p50 of 8 s
windows, `quality` preset, Null backend unpaced, 2 x 3072x3264): first room
ahead 8.02 / 8.04 ms without against 9.31 / 9.29 ms with it (`fov off` 9.52);
the same room, head turned 150 degrees, 7.92 / 7.92 against 9.26 / 9.26
(`fov off` 9.48). That is +1.3 ms, nearly all of the preset's 1.5 ms saving.
Keeping only `b65`-`b92` at full rate would be cheaper (those bindings take
about 1.3 ms with the mask on in the traced frame, so roughly half of that
would come back), but it is selected by binding number only and was not
measured as a rule.

The remedy without that cost is the ring radii: at the `quality` preset (2x2
from 0.90) the face is inside the full-rate zone whenever it is looked at, and
the mottling is limited to hair in the outer rings, at 44 degrees or more from
the view axis. Players who notice it there can move the 2x2 ring out (`radii
0.7 0.9 1.15` to, for example, `0.7 1.0 1.25`) or use `2x1` for the middle
ring, which shows no mottling (neither change was measured for cost), before reaching for `lighting_full_rate`.

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
| `[foveation] subsurface_full_rate` | `1` | `1`: the subsurface recombine passes run at full rate ([Skin edges](#skin-edges-the-subsurface-recombine)); `0`: they take the mask (grey-white line on skin edges under 2x2) |
| `[foveation] lighting_full_rate` | `0` | `1`: every pass after the G-buffer runs at full rate, which removes the mottled hair in the coarse rings ([Hair at 2x2](#skin-edges-the-subsurface-recombine)) but costs about 1.3 ms of the 1.5 ms the `quality` preset saves; dev command `fov lighting 0\|1` |
| `[debug] foveation_unsupported` | `0` | `1`: behave as on a GPU without variable rate shading (tests the fallback) |
| `[foveation] eye_tracking` | `0` | `1` or `auto`: the rings follow the eye gaze when the headset has an eye tracker ([Eye-tracked foveation](#eye-tracked-foveation)); `0`: fixed at the optical centres. Read when the XR session starts |
| `[foveation] gaze_margin_deg` | `5` | degrees added to the full-rate zone's radius while the gaze drives it (covers the gaze sample's age and tracker error) |
| `[foveation] gaze_smoothing` | `0.5` | 0 to 0.95: weight of the previous centre for gaze movements under 2 degrees (fixational jitter); larger movements jump |

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
| `fov subsurface 0\|1` | the subsurface recombine at full rate (1) or with the mask (0) |
| `fov lighting 0\|1` | every pass after the G-buffer at full rate (1) or with the mask (0); see [Hair at 2x2](#skin-edges-the-subsurface-recombine) |
| `fov exclude <first> [<last>]`, `fov exclude off` | keep the scene window's render target bindings `first` to `last` (counted from the scene's start, `b<n>` in `fov trace`) at full rate; for finding which pass causes an artefact |
| `fov trace` | log every render target binding of the next stereo frame, from the scene's start to Present, with GPU times and binding numbers |
| `fov timing` | log and reply the scene GPU time, the GPU time after the scene and the hooks' CPU time since the last report |
| `fov gaze status` | eye tracking: setting, gaze source, following the gaze / holding / fixed, tracked, sample age, each eye's ring centre (pixels, share of the eye, degrees from its axis), surface refills (count, rate, CPU cost), switches, losses bridged, the ring shares |
| `fov gaze mode 0\|1\|auto`, `fov gaze margin <deg>`, `fov gaze smoothing <0..0.95>` | change the eye-tracking settings at run time (a session started with `eye_tracking = 0` has no gaze source until `xr-restart`) |
| `fov gaze dump <file.png>` | read the shading-rate surface back from the GPU and write it as a PNG, one pixel per 16x16 tile: white full rate, yellow / orange / red rings 1 / 2 / outside, grey hidden area, black no eye, a cyan cross at each eye's ring centre |

Log lines to look for:

```
foveation: scene markers installed (Render +0x21e64a0, FPostProcessing::Process +0x251c230, ...)
foveation: variable rate shading available (driver 610.47, r610_45), context hooks installed
foveation: eye views: rect at +0x80 (2064x2208, right eye at x 2064), projection at +0xe0 (...)[, found after N failed search(es)]
foveation: view rect (missing) or projection (+0xe0) not found in the eye views (search N failed); no foveated rendering until a later stereo frame finds them
foveation: surface 259x139 tiles for 4128x2208; pixels: full 37.6 %, ...; left eye ... optical centre at (1212, 1027) ...
foveation: off for this session: <reason>          (unsupported GPU or driver, NVAPI missing, an NVAPI call failed)
```

## Eye-tracked foveation

With a headset that tracks the eyes, the rings of the shading-rate surface
follow the gaze instead of sitting at the optical centres. Off by default
(`[foveation] eye_tracking = 0`). **Verified only with the Null backend's
simulated gaze; no eye-tracked headset has been used with it yet.**

### Gaze input (`src/xr`)

With `InitDesc::eyeGaze` (set when `eye_tracking` is `1` or `auto`) the OpenXR
backend enables the eye-tracking extensions the runtime offers and picks one
source when the session is created (one log line, `xr: eye gaze: source ...`
or `xr: eye gaze: no source (...)` with the reasons):

1. `XR_EXT_eye_gaze_interaction` (ratified, revision 2), used when
   `XrSystemEyeGazeInteractionPropertiesEXT::supportsEyeGazeInteraction` is
   true: an action set `ff7vr_eye_gaze` with one pose action, suggested binding
   `/user/eyes_ext/input/gaze_ext/pose` for
   `/interaction_profiles/ext/eye_gaze_interaction`, attached to the session
   (the only action set this application has), an action space with an
   identity pose. Every `WaitFrame`: `xrSyncActions`, `xrGetActionStatePose`
   (`isActive`), then `xrLocateSpace(gaze space, VIEW space, predicted display
   time)` with `XrEyeGazeSampleTimeEXT` chained. The gaze counts as tracked
   when `ORIENTATION_VALID` and `ORIENTATION_TRACKED` are both set; the
   direction is the pose's -Z axis in head space. `POSITION_TRACKED` is
   reported as "nominal" (the specification's high-quality gaze; a runtime
   clears it for a sub-nominal gaze).
2. `XR_FB_eye_tracking_social`, only when (1) is absent or reports no support
   and `XrSystemEyeTrackingPropertiesFB::supportsEyeTracking` is true:
   `xrCreateEyeTrackerFB`, then per frame `xrGetEyeGazesFB` in VIEW space at
   the predicted display time; the valid eyes' -Z axes are averaged. The
   specification says this extension's gaze may be filtered for avatars; the
   margin covers that.

What the specification says that matters here (OpenXR 1.1, section 12.33):
the gaze pose is oriented like VIEW space (-Z forward) and may originate
between the eyes; a runtime with a permission system reports the action
inactive and clears every location flag until the user allowed access; a
runtime that cannot predict the gaze returns the sample nearest the requested
time and writes its time into `XrEyeGazeSampleTimeEXT::time` (0 = unknown);
`xrSuggestInteractionProfileBindings` must accept the eye gaze path whether or
not the device has a tracker. `xrSyncActions` returns `XR_SESSION_NOT_FOCUSED`
while the session is not focused; every action is then inactive, which counts
as not tracked.

`FrameInfo::gaze` carries the sample (tracked, direction, sample time, display
time). The render module keeps the newest one with each eye's rotation
relative to the head from the same frame (`XrController::GetGaze`), so a
headset with canted displays gets the right direction per eye.

### What the runtimes offer

| Runtime | Offers | Source |
|---|---|---|
| Virtual Desktop (VDXR) | both; `supportsEyeGazeInteraction` is true only when the headset reports eye tracking (Quest Pro, PICO 4 Pro / Enterprise through Virtual Desktop) or its `simulate_eye_tracking` setting is on. The gaze is the average of both eyes' poses, used only when both are valid with confidence above 0.5; the sample time it returns is the requested time | its source (`virtualdesktop-openxr/instance.cpp`, `system.cpp`, `eye_tracking.cpp`, `space.cpp`, `action.cpp`; commit `f039941`), and the extension names in the installed `virtualdesktop-openxr.dll` |
| SteamVR | `XR_EXT_eye_gaze_interaction` (since SteamVR 2.8.3; drivers send data through `Prop_SupportsXrEyeGazeInteraction_Bool`, Steam Link forwards a Quest Pro's gaze when enabled in its settings); no `XR_FB_eye_tracking_social` | Valve's SteamVR announcements and the extension names in the installed `vrclient_x64.dll` (which also contains `XR_META_foveation_eye_tracked`) |
| PICO Connect / Streaming (PC) | both extension names in `picostreaming-openxr.dll`, plus PICO's own eye tracker types; public reports say the PC streaming path does not deliver a gaze for the PICO 4 Pro / Enterprise | the installed DLL; PICO's OpenXR documentation lists `XR_EXT_eye_gaze_interaction` for its standalone runtime |
| Meta Quest Link (Quest Pro) | `XR_FB_eye_tracking_social` only (needs developer mode and eye tracking over Link enabled in the Meta app); this is why the second source exists | public developer reports; the runtime is not installed on the development machine |

Steam Frame: not looked into beyond SteamVR itself; a gaze that SteamVR
exposes arrives through path 1.

### From gaze to rings

- Rings are measured as **angles** from the ring centre: a preset radius `r`
  (a fraction of half the eye width as a tangent) becomes `atan(r / sx)` with
  `sx` the projection's x scale. At the optical centre this is exactly the
  fixed surface (same tiles, same shares as before), and off-centre a ring
  keeps its angular size (it is stretched on the image plane towards the
  edge, as the lenses stretch it back).
- Per tile and eye the view direction of the tile centre is computed once per
  layout; a new centre then needs one dot product per tile and eye.
- The gaze direction (head space) is turned into each eye's space and becomes
  that eye's centre. Both eyes get the same direction: the tracker gives no
  vergence, and the 32 mm between an eye and the gaze origin changes the
  angle by under 2 degrees for anything farther than 1 m.
- **Latency margin**: the full-rate zone grows by `gaze_margin_deg` (default
  5) while the gaze drives the centre; the outer radii stay as the preset says
  (they are pushed out only when the margin would cross them). The sample is
  about one display period older than the frame it shades (13.9 ms at 72 Hz in
  the Null backend; a real tracker adds its own latency, and Virtual Desktop
  returns the requested time, not the true sample time).
- **Smoothing, no prediction**: a gaze step larger than 2 degrees is a saccade
  and the centre jumps there; smaller steps (fixational jitter) are smoothed
  with `gaze_smoothing`. Saccade landing points cannot be extrapolated from a
  72-90 Hz stream, so the margin, not a prediction, covers the gaze's age.
- **Hysteresis**: the gaze takes over after 3 consecutive tracked samples; a
  gaze that stops being tracked keeps its last centre for 400 ms (blinks last
  100 to 300 ms) and only then the fixed centre returns. A loss shorter than
  that changes nothing (`losses bridged` in `fov gaze status`).
- **Refills**: the surface (R8_UINT, DEFAULT usage) is refilled in place with
  `UpdateSubresource` when either eye's centre moved by more than half a tile
  (8 pixels), or the centre switched between gaze and fixed. No new texture or
  view, so the NVAPI binding stays.
- `eye_tracking = 0`: no extension is enabled, nothing is sampled, and the
  per-frame cost is one comparison; the surface is built once per layout as
  before.
- `1` and `auto` behave the same at run time (gaze when tracked, fixed
  otherwise); `1` also logs once when the session has no gaze source.

### Proof (Null backend, 2 x 3072x3264, first room, uncapped)

The Null backend simulates a tracker when `eye_tracking` is on: `xr-sim gaze
<yaw> <pitch>` (head space, yaw positive to the right, pitch positive up),
`xr-sim gaze off` (not tracked), `xr-sim gaze sweep [radius deg] [period s]`
(a circle around the view axis, default 15 degrees every 4 s), `xr-sim gaze
blink <frames>`. Scripts and captures: `captures\gaze\` (not in git).

- Ring centres follow the gaze with the right offset per eye: gaze 0/0 puts
  the centres at the optical centres (left eye 58.7 % / 46.5 % of the eye,
  right eye 41.3 % / 46.5 %); gaze 20 degrees right moves both by 16.7 % of
  the eye width (75.4 % and 58.0 %); -20/+10 gives 42.0 % / 38.1 % and
  24.6 % / 38.1 %; 30/15 gives 85.2 % / 32.6 % and 67.8 % / 32.6 %. Surface
  dumps (`fov gaze dump`) show the full-rate zone around the cross in both
  eyes.
- The GPU applies the refilled surface where it says: with every ring at 4x4
  and the mask on post-processing too (`fov rates 4x4 4x4 4x4`, `fov passes
  all`, a debug view only), the eye captures were measured per 16x16 tile
  (pixels inside 4x4 cells identical while cell borders differ = coarse).
  Against the surface dumped at the same gaze: 98.8 to 99.5 % of the textured
  full-rate tiles look full rate and 98.4 to 100 % of the coarse tiles look
  coarse, in both eyes. Against the surface of a different gaze (control):
  40 to 51 % agreement on the full-rate tiles.
- Ring shares (`quality`): fixed 37.7 % full rate / 57.6 % work; with the
  gaze at 0/0 the margin raises it to 53.7 % / 65.6 %; at 20/0 50.4 % /
  62.1 %; at 30/15 39.0 % / 53.3 % (part of the zone leaves the image).
- Refills: a moving gaze (sweep 15 degrees / 4 s, about 24 degrees per
  second) refills 66 to 79 times per second at about 110 frames per second;
  0.27 to 0.30 ms CPU per refill on the RHI thread (computation and upload),
  at most 0.57 ms in one run and 1.47 ms in another. A still gaze refills only
  when it moves.
- GPU time of the scene (p50 over about 1100 frames each, same session):

  | Setting | Scene GPU ms |
  |---|---|
  | foveation off | 9.49 |
  | `quality`, fixed centre | 7.99 |
  | `quality`, gaze still at 15/0 | 8.22 |
  | `quality`, gaze sweeping | 8.29 |
  | radii `0.35 0.55 0.80`, fixed centre | 7.31 |
  | radii `0.35 0.55 0.80`, gaze still at 15/0 | 7.41 |
  | radii `0.35 0.55 0.80`, gaze sweeping | 7.44 |

  A moving gaze costs nothing measurable on the GPU against a still one. With
  the `quality` radii the gaze costs 0.2 to 0.3 ms because the margin enlarges
  the full-rate zone; the gain of eye tracking is that a much smaller zone
  becomes acceptable (the `0.35` radii save another 0.6 to 0.7 ms against
  `quality`). Which radii look right with a real tracker is for a headset
  test; no preset is changed.
- Loss of tracking: `gaze off` -> fixed centre 408 to 418 ms later (log);
  `gaze <yaw> <pitch>` -> following the gaze 31 to 35 ms later (3 samples); a
  blink of 10 or 30 frames (about 90 / 270 ms) changed nothing (no refill, no
  switch); a blink of 80 frames (about 700 ms) went to the fixed centre and
  back.
- `eye_tracking = 0`: no `eye gaze` log line, `xr-sim gaze` reports no
  simulated tracker, the surface is built once per stereo start as before and
  its shares are identical to the earlier fixed surface at this resolution
  (37.7 / 23.7 / 26.6 / 3.5 / 8.5 %).

### Not verified

- Any real eye tracker or runtime path: the OpenXR code (action set, sync,
  locate, sample time, `XR_FB_eye_tracking_social`) is built to the
  specification and compiled, but has only run against runtimes without a
  tracker. In particular: whether Virtual Desktop with a Quest Pro reports
  `isActive` and the flags as expected, and how old its samples really are.
- Whether the margin and smoothing defaults look right: a ring edge that
  lags a saccade shows coarse shading at the new fixation point for a frame
  or two; saccadic suppression should hide most of it, untested.
- Picture at the ring edges while the centre moves: in the sweep captures
  (radii `0.35 0.55 0.80`) the edges look like those of the fixed centre at
  the same radii, judged on single frames only. Seen in both: under 2x2
  shading the top edge of a skin area against dark cloth gets a thin line of
  white pixels (first room, Tifa's legs), which full-rate shading does not
  show; that is foveation itself, not the moving centre.

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
