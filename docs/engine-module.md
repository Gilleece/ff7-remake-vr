# Engine module (`src/engine`)

The engine module puts the game's own Unreal Engine 4.18 stereo path to work: it installs
a stereo rendering device into the engine, so the engine renders both eyes in one frame,
at headset resolution, into a double-wide render target, with per-eye cameras built from
the headset's views on top of the game's third-person camera.

CMake target `ff7vr_engine`; the loader starts it from `src/loader/startup.cpp`
(`ff7vr::engine::start`). Reverse-engineering background: `docs/re/engine.md` (engine
facts) and `docs/re/stereo-hook-plan.md` (the design this module implements).

## What it hooks

Two slots of the `UGameEngine` vtable are replaced, one engine member and one engine
global are written, and two single bytes of code can be changed:

| What | Where | Why |
|---|---|---|
| `UEngine::InitializeHMDDevice` | `UGameEngine` vtable slot 111, swapped at process start | after the engine's own code has run, our device is stored in `GEngine->StereoRenderingDevice` (`+0xD50`, shared pointer with a static reference controller). This happens inside `UEngine::Init`, before the game viewport and the local player exist, so the engine allocates the per-eye view states itself (TAA, occlusion and eye adaptation history per eye) |
| `UGameEngine::Tick` | `UGameEngine` vtable slot 78 | per-frame point on the game thread: fetches the frame's eye views from the host (for OpenXR this is where the frame wait happens), decides whether this frame is stereo, applies queued console variable writes |
| `GSystemResolution` | engine global | only while stereo renders: set to the eye target size, because this build sizes its scene buffers from it (`docs/re/engine.md` section 8); the game's value is put back when stereo stops |
| windowed-fullscreen view rect | the `jne` at RVA `0x3018fb8` in `ULocalPlayer::CalcSceneView` | only while stereo renders: made unconditional so that, in windowed fullscreen, the game does not replace the eye rects with the full screen |
| light sort-key immediate | one byte in `FDeferredShadingSceneRenderer::RenderLights` | only when `[stereo] light_fix = 1` or the `stereo lightfix 1` command |
| Square Enix's bloom reduce pass (`Process`) | inline hook, render thread | `[stereo] bloom_fix` (default on): for the first level of a view that does not start at the origin, an RHI command arms the right-eye bloom fix (below) |
| D3D11 immediate context draw/dispatch/clear/copy functions | inline hooks, RHI thread | installed at the first stereo frame when the bloom fix is on (it needs to act on one DrawIndexed), or by the first GPU trace; otherwise not installed |
| `FRenderTargetPool::FindFreeElement` | inline hook, render thread | only after `gpu names on` (GPU trace labels) |
| the object array and name pool | read on the game thread | only with `[stereo] movie_screen = 1`: movie detection (below) |

Everything else goes through the device's own function tables, which the engine calls:
view rects, per-eye view offset and projection, the size of the separate render target,
and `RenderTexture_RenderThread`, from which the eye texture is handed to the XR host and
the desktop mirror is drawn.

### Start-up checks

`start()` runs on the loader's thread at process start, seconds before the engine
initialises:

1. The build must be file version 1.0.0.7 (SizeOfImage and PE timestamp). Another build is
   refused unless `[stereo] allow_unknown_build = 1`.
2. Three signatures (the `UGameEngine` vtable, `InitializeHMDDevice`, the slot offset
   used by `UEngine::Init`) are resolved and the slot is checked to hold that function;
   then the slot is swapped. This takes about 0.2 s.
3. On a separate thread every other address is resolved by signature (each checked for
   uniqueness and, on the known build, against the RVA recorded for it), and 13 call sites
   are checked to call exactly the vtable slots our device implements. About 1 s.
4. When `InitializeHMDDevice` runs, the hook calls the original, waits for step 3 and
   installs the device only if everything passed. Otherwise the log says why
   (`engine: stereo disabled: ...`) and the game runs unmodified.

Signature names match `tools/re/signatures.json`; keep both in sync.

## Frame flow

```
game thread, UGameEngine::Tick (our hook)
    host.begin_game_frame(wanted, frame)     views, frame id, "can this frame be stereo"
    GSystemResolution = eye target size       (while stereo)
  ... FSceneViewport::EnqueueBeginRenderFrame
    ShouldUseSeparateRenderTarget / NeedReAllocateViewportRenderTarget / UpdateViewport
  ... UGameViewportClient::Draw               (same Tick: both eyes use this frame's views)
    AdjustViewRect                            left eye [0, w) x [0, h), right eye [w, 2w)
    CalculateStereoViewOffset                 eye camera (below)
    GetStereoProjectionMatrix                 asymmetric projection from the eye's FOV
  end of Tick: frame id and views queued for the render thread
render thread
    FSceneViewport::InitDynamicRHI (on reallocation): CalculateRenderTargetSize = 2w x h
    scene renders both eyes into the separate target
    Slate DrawWindow_RenderThread -> RenderTexture_RenderThread
        appends the frame-end command to the RHI command list
RHI thread (this game runs D3D11 with one)
    executes the recorded commands in order: scene, frame-end command, Slate UI, Present
    frame-end command: host.eye_texture_ready(texture, eye rects, frame id, views)
                       desktop mirror blit into the back buffer
    Present (the render module copies the eye rects into the XR swapchains)
```

The eye texture is handed over on the presenting thread immediately before the Present
that ends the frame, so its content is exactly that frame's image. The game thread can be
two frames ahead of that Present; the frame id and the views travel with the frame through
the render thread to the RHI thread (`docs/re/engine.md`, "Frame pipeline").

The engine renders in stereo while stereo is wanted and the host provides frames. A frame
for which the host has no XR frame (a late frame, a hitch) is still rendered in stereo with
the last views, so the eye target is not released; after 45 such frames in a row, or when
stereo is switched off, the engine renders the normal window (the separate target is
released and reallocated when stereo resumes, which costs a short hitch).

## Hosts: where the eye size and views come from

`ff7vr/engine/stereo_host.h` is the interface: eye size and views in, eye texture out.

| `[stereo] host` | Source | Use |
|---|---|---|
| `render` (default when the render module is built) | the render module's XR session (`docs/render.md`): `GetEyeSetup`, `BeginGameFrame`, `SubmitStereoFrame` | normal use; the headset (or the Null backend / SteamVR null driver for tests) |
| `fixed` | built-in values from the ini: eye size, FOV, IPD, scripted head motion | engine work without any XR session; nothing consumes the eye texture |

With the render host and no running XR session (no headset), every frame is mono: the
game runs normally on the desktop.

How the render host pairs images with XR frames: the render module ends its waited XR
frames in order, one per Present, and the engine has up to two frames in flight between the
wait (game thread) and Present, so the XR frame a Present ends is not always the one the
image was rendered for (right after stereo starts, the Presents of the last mono frames
end the first stereo frames). Since the image is handed over right before its own Present,
the host offers it for every XR frame that Present may end (the open ones, at most four),
each time with the views it was really rendered with (`StereoSubmit::renderedViews`), so the
runtime re-projects it correctly whichever frame carries it.

## The eye camera

`CalculateStereoViewOffset` receives the game's camera (the third-person camera manager's
point of view) and returns each eye's camera:

```
orientation = camera_yaw_only * eye_orientation          (decoupled pitch, default)
location    = camera_location + camera_yaw_only.rotate(eye_position * WorldToMeters * world_scale)
```

`eye_orientation` and `eye_position` are the eye's pose in tracking space (head pose and
the eye's offset from it, so the IPD comes from the runtime), converted from OpenXR axes
to Unreal's. The projection is built from the eye's own FOV tangents (asymmetric), with
the engine's near plane (`GNearClippingPlane`) and reversed-Z infinite far plane, the same
form the engine's own stereo code uses.

Choices for a seated player in a third-person game:

- **Decoupled pitch (default on).** The game's camera pitches and sometimes rolls when the
  right stick moves it or a scripted camera frames a scene. Applied to a headset, that
  tilts the whole world against the player's inner ear. With decoupled pitch only the game
  camera's yaw turns the player; looking up and down is done with the head, and the
  horizon stays level whatever the game camera does. Its position is still the game's.
- **Head position is applied (default on)**, scaled by the world's `WorldToMeters` (100)
  and `world_scale`, so leaning in moves the view the way a seated player expects.
  `positional = 0` keeps head rotation and the IPD only.
- **World scale 1.0**: the IPD and head motion are at real-world scale. Larger values make
  the world look smaller (the player becomes a giant), smaller values the opposite.
- The yaw from the right stick is applied as the game does it (smooth turning); recenter
  is the XR host's (`recenter` dev command of the render module).

## ini keys (`[stereo]` in `ff7vr.ini`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `1` | `1`: install the stereo device; `0`: nothing is hooked, the game runs unmodified |
| `start_in_stereo` | `1` | stereo switched on from the start (frames are still mono until the host can show them) |
| `host` | `render` | `render` or `fixed` (see above) |
| `world_scale` | `1.0` | multiplies WorldToMeters for the head offset and IPD |
| `decoupled_pitch` | `1` | drop the game camera's pitch and roll |
| `positional` | `1` | apply head position |
| `mirror` | `crop` | desktop window in stereo: `crop` (left eye, centre crop at the window's aspect), `left`, `right`, `both` (side by side, letterboxed), `off` |
| `light_fix` | `0` | light sort-key patch (see `docs/re/engine.md` section 6) |
| `bloom_fix` | `1` | right-eye bloom fix (below) |
| `movie_screen` | `0` | movie detection: stereo is held off while a pre-rendered movie plays, so the virtual screen shows it (below) |
| `allow_unknown_build` | `0` | try a game build other than 1.0.0.7 if every signature and layout check passes |
| `log_frames` | `0` | log the eye cameras of the first N stereo frames |
| `eye_width`, `eye_height` | `1280`, `1440` | fixed host: per-eye render size |
| `fov_left_deg`, `fov_right_deg`, `fov_up_deg`, `fov_down_deg` | `-45`, `45`, `45`, `-45` | fixed host: left eye FOV (the right eye mirrors left/right) |
| `ipd_mm` | `64` | fixed host |
| `head_motion` | `static` | fixed host: `static`, `yaw`, `sway`, `yawsway` (same scripts as the XR Null backend) |

Two more sections hold console variables (names are case-insensitive):

| Section | Meaning |
|---|---|
| `[cvars]` | `name = value`, set once when the device is installed (inside `UEngine::Init`, before the game creates its viewport and UI) |
| `[stereo_cvars]` | `name = value`, held only while the engine renders in stereo: each variable's value is saved when stereo starts and put back when it stops |

## Dev commands

Through the dev pipe (`[dev] pipe = 1`, `tools\dev\send-input.ps1 -Pipe "<command>"`):

| Command | Effect |
|---|---|
| `stereo status` | installed, wanted/active, eye size, eye texture size and format, call counters, frame time of the last 10 s window |
| `stereo views` | last eye cameras (rotation, location) and projection terms |
| `stereo on` / `stereo off` | switch stereo from the next frame |
| `stereo mirror <crop\|left\|right\|both\|off>` | desktop mirror |
| `stereo eye <w> <h>`, `stereo fov <l> <r> <u> <d>`, `stereo ipd <mm>`, `stereo motion <...>` | fixed host values |
| `stereo head <yaw> [pitch]` | fixed host: a fixed head rotation in degrees (left and up positive), added to the motion script |
| `stereo scale <f>`, `stereo pitch <0\|1>`, `stereo positional <0\|1>` | camera settings |
| `stereo lightfix <0\|1>` | light sort-key patch |
| `stereo log <n>` | log the eye cameras of the next n stereo frames |
| `stereo bloomfix [0\|1]` | right-eye bloom fix on/off, with its counters (reduce passes seen, commands queued, draws fixed, misses) |
| `stereo movie [on\|off]`, `stereo movie menu <0\|1>` | movie detection on/off and its state; `menu 1` counts the menu background players too (test) |
| `stereo swap <0\|1>` | test: render the right eye into the left half of the target and the left eye into the right half (the eyes then come out swapped). Tells a bug that follows a view's position in the target from one that follows the view |
| `gpu names on`, `gpu trace <prefix> [dump fullscreen \| dump <from> <to>] [scale <n>]`, `gpu status` | one-frame GPU trace (`docs/re/engine.md`, Tools) |
| `re peek <rva> <n>`, `re poke <rva> <hex bytes>` | read or patch the game image (to try a patch in a running game) |
| `stereo host <render\|fixed>` | switch the source of eye size and views (for tests; switching away from `render` leaves the render module in stereo mode) |
| `cvar get <name>` | integer and float value and set-by priority of a console variable |
| `cvar set <name> <value>` | set it with console priority (applied on the game thread) |

`ff7vr/engine/cvars.h` offers the same console variable access to other code.

## How to verify

Unit tests of the camera and projection math (no game):

```
build\<name>\src\engine\ff7vr_engine_tests.exe
```

In the game, through the harness (`docs/dev-harness.md`):

```
# Reach gameplay in mono, then switch stereo on and look at both eyes in the window
tools\dev\launch.ps1 -Until gameplay -KeepRunning -Set "stereo.start_in_stereo=0;stereo.mirror=both;xr.backend=null"
tools\dev\send-input.ps1 -Pipe "stereo on"
tools\dev\screenshot.ps1
.venv\Scripts\python.exe tools\re\live_check.py      # device, view states, separate target size
tools\dev\send-input.ps1 -Pipe "capture <repo>\captures\stereo\shot"   # per-eye PNGs (render module)
tools\dev\stop.ps1
```

`live_check.py` should show the mod's device at `GEngine+0xD50`, three distinct view
states, `bUseSeparateRenderTarget / bForce = (0, 1)` and a render target of twice the eye
width by the eye height, independent of the window size.

The log lines to look for:

```
engine: build 1.0.0.7 (known), signatures resolved in ... ms, layout checks passed
engine: stereo device installed at GEngine+0xd50 (stereo on at start)
engine: local player view states ... (three distinct, per-eye history available)
stereo: rendering STEREO from tick ... (eye WxH, ...)
fixes: GSystemResolution 1280x720 -> 2WxH while stereo renders (scene buffers cover the eye target)
stereo: render target size 2WxH (window ...)
stereo: views built inside UGameEngine::Tick (...)
frame time: ... frames, avg ... ms ... (stereo, eye WxH)
```

With the render host, the render module's `status` command shows the stereo frames it
ended (`submitted ... stereo N`, `submit errors 0`).

`stereo head <yaw> [pitch]` with the fixed host checks that the world stays fixed when the
head turns: with a 90 degree symmetric FOV (focal length = half the eye width in pixels)
a 10 degree turn moves the image centre by `f * tan(10 deg)`.

## Measured

Gameplay, first room of the save used by the harness, 1280x720 window, no XR session
(fixed host), the game's 120 fps cap lifted (`t.MaxFPS 0`), RTX 5080 / Ryzen 7 5800X3D:

| Rendering | Frame time avg | p95 |
|---|---|---|
| mono, 1280x720 window (device installed, stereo off) | 2.24 ms | 2.52 ms |
| stereo 2 x 2064x2208 | 7.0 ms | 7.4 ms |
| stereo 2 x 2500x2600 | 8.6 ms | 9.0 ms |

## Right-eye bloom fix

Square Enix's bloom builds a mip chain per view with every level at the origin of its
target, and its first pass reduces the view's full-resolution input (a target holding both
eyes side by side). That pass's pixel shader samples the input relative to the origin, so
for the right eye it reduced the left eye's image: the right eye showed a soft copy of the
left eye's lamps, windows and lit surfaces at the left eye's image positions. Details and
how it was found: `docs/re/engine.md`, section 10.

`src/engine/src/bloom_fix.cpp`: the hook on the reduce pass's `Process` (render thread)
appends an RHI command for the first level of a view whose rectangle (`FViewInfo+0x70`) does
not start at the origin. On the RHI thread the command arms the next DrawIndexed on the
immediate context, which is that pass's draw: the view's rectangle of the bound input
(shader resource 0) is copied to the origin of a scratch texture of the same size and format,
the scratch texture is bound in its place for this one draw, and the engine's binding is put
back afterwards. The engine's textures are not changed. `stereo bloomfix` shows `applied`
growing by one per stereo frame and `missed 0`.

Cost: one copy of the eye's rectangle per frame (2064x2208 RGBA16F, 36 MB of copy traffic)
and a scratch texture as large as the scene colour target (4128x2208 RGBA16F, 73 MB of video
memory).

Evidence (Null backend, Quest 3 class asymmetric FOV, eyes 2064x2208, right eye, fix off and
on in the same session): `captures/stereo/runL/crop_room_R_before_after.png` (first room),
`captures/stereo/runM/crop_street_R_before_after.png` (street outside),
`captures/stereo/runM/crop_shop_R_before_after.png` (item shop). Full eye images:
`runL/l_nofix_*.png`, `l_fix2_*.png`, `runM/m09_*`, `n03_*`. The left eye is the same with the
fix on and off (mean pixel difference 0.6, the same as between two captures without any
change).

## Movies

The game plays its pre-rendered movies (`.emov` files under
`End/Content/GameContents/Movie`) through Unreal's media framework: every movie has a
`UMediaPlayer` asset (`<name>_MediaPlayer`, packages under `/Game/GameContents/Movie/...`),
and the menu backgrounds use the same mechanism from packages under `/Menu/`.

`src/engine/src/movie_watch.cpp` (`[stereo] movie_screen = 1`): on the game thread it finds
the `MediaPlayer` class and its `IsPlaying` function by name once (object array and name pool,
a slice per frame), then keeps scanning the object array a slice per frame (16384 slots) for
`MediaPlayer` objects and asks each non-menu one `IsPlaying` through `ProcessEvent` every
frame. While one plays, stereo is switched off: the engine renders the normal window and
the render module shows it on the virtual screen (the automatic fallback of stereo mode);
stereo comes back when no movie plays. The switch reallocates the eye target (a short
hitch at the start and end of a movie).

Status: the class and function are found in gameplay (`stereo movie`); detection of a
playing movie has not been seen yet (no movie was reached with scripted input), so the key
is off by default.

## UI layer

`src/engine/src/ui_layer.cpp` (start function `start_ui_layer`, called by the loader right
after `start`; installed only with `[stereo] enabled = 1`) keeps the game's UI out of the eye
images while the render module shows it on a quad layer (`docs/render.md`, "UI layer"):

| What | Where | Why |
|---|---|---|
| `FSceneRenderTargets::BeginRenderingInGameUI` | inline hook, render thread | a second eye of the same view family gets no UI pass: one UI render per frame |
| `FSceneRenderTargets::EndRenderingInGameUI` | inline hook, render thread | clears the view family's in-game UI flag (`+0x3C`, bit `0x80`) after the pass, so post-processing binds the empty fallback texture; appends an RHI command that reports the UI texture to the render module on the RHI thread before the frame's Present |

Both act only for stereo eye views while `render::UiLayerWanted()` is true; every other call
runs the engine's code unchanged. The engine facts are in `docs/re/engine.md`, section 9.
Signatures are resolved at start-up with the same rules as the rest of the module (unique
match, checked against the RVA of build 1.0.0.7). `[ui] once_per_frame = 0` (or `uihook once 0`)
lets the game draw the UI for both eyes again; `uihook status` shows the counters.

## Known problems

- Square Enix's custom glare (`docs/re/engine.md`, section 10) puts both views' glare at the
  same place of one target; in a scene with glare primitives the left eye would get the
  right eye's glare. Not seen yet (no glare primitives in the scenes tested).
- Without the UI layer (`[ui] layer = 0`, or no XR session) the in-game UI is composited
  into each eye as a central crop of the 16:9 UI; the size variables cannot fix that
  (`docs/re/engine.md`, "What the UI composite does with an eye view"). With it the UI is on
  its own layer (section "UI layer").
- With a real OpenXR runtime the render module must hand the frame its XR thread already
  waited to the game thread at the start of stereo instead of waiting a second one
  (`XrController::BeginGameFrame`); otherwise the game thread blocks in `xrWaitFrame`
  forever when the pipeline is idle (seen with SteamVR's null driver).
