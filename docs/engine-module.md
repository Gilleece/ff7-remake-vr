# Engine module (`src/engine`)

The engine module puts the game's own Unreal Engine 4.18 stereo path to work: it installs
a stereo rendering device into the engine, so the engine renders both eyes in one frame,
at headset resolution, into a double-wide render target, with per-eye cameras built from
the headset's views on top of the game's third-person camera.

CMake target `ff7vr_engine`; the loader starts it from `src/loader/startup.cpp`
(`ff7vr::engine::start`). Reverse-engineering background: `docs/re/engine.md` (engine
facts) and `docs/re/stereo-hook-plan.md` (the design this module implements).

## What it hooks

Nothing in the game's code is patched for stereo. Two slots of the `UGameEngine` vtable
are replaced, and one data member is written:

| What | Where | Why |
|---|---|---|
| `UEngine::InitializeHMDDevice` | `UGameEngine` vtable slot 111, swapped at process start | after the engine's own code has run, our device is stored in `GEngine->StereoRenderingDevice` (`+0xD50`, shared pointer with a static reference controller). This happens inside `UEngine::Init`, before the game viewport and the local player exist, so the engine allocates the per-eye view states itself (TAA, occlusion and eye adaptation history per eye) |
| `UGameEngine::Tick` | `UGameEngine` vtable slot 78 | per-frame point on the game thread: fetches the frame's eye views from the host (for OpenXR this is where the frame wait happens), decides whether this frame is stereo, applies queued console variable writes |
| light sort-key immediate | one byte in `FDeferredShadingSceneRenderer::RenderLights` | only when `[stereo] light_fix = 1` or the `stereo lightfix 1` command |

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
    stereo this frame = wanted && frame.stereo
  ... UGameViewportClient::Draw
    AdjustViewRect                            left eye [0, w) x [0, h), right eye [w, 2w)
    CalculateStereoViewOffset                 eye camera (below)
    GetStereoProjectionMatrix                 asymmetric projection from the eye's FOV
  ... FSceneViewport::EnqueueBeginRenderFrame
    ShouldUseSeparateRenderTarget / NeedReAllocateViewportRenderTarget
render thread
    FSceneViewport::InitDynamicRHI (on reallocation): CalculateRenderTargetSize = 2w x h
    scene renders both eyes into the separate target
    Slate DrawWindow_RenderThread -> RenderTexture_RenderThread
        host.eye_texture_ready(texture, eye rects, frame id)
        appends an RHI command that draws the desktop mirror into the back buffer
RHI thread (this game runs D3D11 with one)
    executes the recorded commands in order: scene, our mirror command, Slate UI, Present
        (the render module copies the eye rects into the XR swapchains at Present)
```

The engine renders in stereo only in frames the host confirms; in other frames the device
reports stereo off and the engine renders the normal window (the separate target is
released and reallocated at the next stereo frame).

## Hosts: where the eye size and views come from

`ff7vr/engine/stereo_host.h` is the interface: eye size and views in, eye texture out.

| `[stereo] host` | Source | Use |
|---|---|---|
| `render` (default when the render module is built) | the render module's XR session (`docs/render.md`): `GetEyeSetup`, `BeginGameFrame`, `SubmitStereoFrame` | normal use; the headset (or the Null backend / SteamVR null driver for tests) |
| `fixed` | built-in values from the ini: eye size, FOV, IPD, scripted head motion | engine work without any XR session; nothing consumes the eye texture |

With the render host and no running XR session (no headset), every frame is mono: the
game runs normally on the desktop.

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
| `enabled` | `0` | `1`: install the stereo device; `0`: nothing is hooked, the game runs unmodified |
| `start_in_stereo` | `1` | stereo switched on from the start (frames are still mono until the host can show them) |
| `host` | `render` | `render` or `fixed` (see above) |
| `world_scale` | `1.0` | multiplies WorldToMeters for the head offset and IPD |
| `decoupled_pitch` | `1` | drop the game camera's pitch and roll |
| `positional` | `1` | apply head position |
| `mirror` | `crop` | desktop window in stereo: `crop` (left eye, centre crop at the window's aspect), `left`, `right`, `both` (side by side, letterboxed), `off` |
| `light_fix` | `0` | light sort-key patch (see `docs/re/engine.md` section 6) |
| `allow_unknown_build` | `0` | try a game build other than 1.0.0.7 if every signature and layout check passes |
| `log_frames` | `0` | log the eye cameras of the first N stereo frames |
| `eye_width`, `eye_height` | `1280`, `1440` | fixed host: per-eye render size |
| `fov_left_deg`, `fov_right_deg`, `fov_up_deg`, `fov_down_deg` | `-45`, `45`, `45`, `-45` | fixed host: left eye FOV (the right eye mirrors left/right) |
| `ipd_mm` | `64` | fixed host |
| `head_motion` | `static` | fixed host: `static`, `yaw`, `sway`, `yawsway` (same scripts as the XR Null backend) |

## Dev commands

Through the dev pipe (`[dev] pipe = 1`, `tools\dev\send-input.ps1 -Pipe "<command>"`):

| Command | Effect |
|---|---|
| `stereo status` | installed, wanted/active, eye size, eye texture size and format, call counters, frame time of the last 10 s window |
| `stereo views` | last eye cameras (rotation, location) and projection terms |
| `stereo on` / `stereo off` | switch stereo from the next frame |
| `stereo mirror <crop\|left\|right\|both\|off>` | desktop mirror |
| `stereo eye <w> <h>`, `stereo fov <l> <r> <u> <d>`, `stereo ipd <mm>`, `stereo motion <...>` | fixed host values |
| `stereo scale <f>`, `stereo pitch <0\|1>`, `stereo positional <0\|1>` | camera settings |
| `stereo lightfix <0\|1>` | light sort-key patch |
| `stereo log <n>` | log the eye cameras of the next n stereo frames |
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
stereo: render target size 2Wx H (window ...)
stereo: views built inside UGameEngine::Tick (...)
frame time: ... frames, avg ... ms ... (stereo, eye WxH)
```
