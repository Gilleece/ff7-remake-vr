# Testing the XR layer without a headset

The XR layer (`src/xr`, CMake target `ff7vr_xr`) talks to the VR runtime. It
has two backends behind one interface (`src/xr/include/ff7vr/xr/xr.h`):

- **OpenXR** (D3D11): the real backend. The OpenXR loader is linked statically.
- **Null**: no runtime. A Quest 3 class virtual headset with a fixed FOV,
  64 mm IPD and scripted head motion. It can write each eye to PNG.

`tools/xr_smoke` is a small console app that exercises either backend without
the game: it creates its own D3D11 device, draws a test pattern for both eyes
into one side-by-side texture, submits it for N frames, prints what the runtime
reports plus frame timing, and exits non-zero on any failure.

This page covers:

1. building `xr_smoke`
2. the Null backend test (no VR software involved)
3. OpenXR runtime selection
4. OpenXR against SteamVR's null driver (a virtual headset), including what
   the scripts change on the machine and how to undo it

## 1. Building

Use a Visual Studio developer shell (MSVC x64, the bundled CMake and Ninja).

Standalone, independent of the rest of the project:

```powershell
cmake -S tools/xr_smoke -B build/xr_smoke -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/xr_smoke
# -> build/xr_smoke/xr_smoke.exe
```

As part of the root project (`tools\dev\build.ps1` or a plain CMake build) the
root `CMakeLists.txt` adds `src/xr` and `tools/xr_smoke`; the exe ends up in
`<build dir>\tools\xr_smoke\xr_smoke.exe`. `-Without xr_smoke` skips it.

Dependencies are pinned by commit and shared in `third_party/_fetched`
(`FETCHCONTENT_BASE_DIR`): OpenXR-SDK `release-1.1.63` and stb. A checkout that
is already there at the pinned commit is used without network access.
Shaders are compiled at build time with `fxc` from the Windows SDK.

## 2. Null backend

```powershell
xr_smoke --backend null --frames 120 --capture out\null
```

This writes `out\null_f0000_L.png`, `out\null_f0000_R.png` (first frame) and
`out\null_f0119_*.png` (last frame), then reads them back and checks:

- the size equals the eye swapchain size;
- a green marker is at the top-left and not at the top-right or bottom-left
  (so the image is neither mirrored nor upside down);
- the probe patch has this eye's colour (left: dark red `110,40,40`, right:
  dark blue `40,40,110`), so the eyes are not swapped;
- an sRGB 128 grey patch and an sRGB 188 grey patch come out within +-2, so
  gamma is handled correctly (no washed-out or too dark image).

Open the PNGs to look at them too: each eye shows its name (`LEFT` / `RIGHT`),
the frame counter, a 100 px grid, a yellow crosshair at the eye centre and a
16-step grey ramp. Next to the 128 patch there is a patch of 1 px black and
white lines; from a distance it should look as bright as the 188 patch beside
it. That is the visual gamma check.

Useful variations (all of these pass on the reference machine):

| Option | What it exercises |
|---|---|
| `--source-format r10g10b10a2` (default), `bgra8`, `rgba8`, `rgba8srgb`, `bgra8typeless`, `rgba16f` | source formats; sRGB-encoded UNORM, `_SRGB`, typeless with a view format, linear FP16 |
| `--swapchain-format rgba8srgb`, `bgra8srgb`, `rgba16f`, `rgb10a2`, `rgba8`, `bgra8` | emulated swapchain format (copy path when the bits can be copied, shader blit otherwise) |
| `--threaded` | `WaitFrame` on one thread, `BeginFrame`/`SubmitFrame` on another, one frame in flight, like the engine |
| `--alternate-eyes` | only one eye is updated per frame; the other keeps its last image |
| `--recenter-at N` | calls `Recenter()` before frame N and checks that the head is at the origin facing -Z afterwards |
| `--motion static\|yaw\|sway\|yawsway` | Null head motion (deterministic, a function of the frame number) |
| `--eye-size WxH` | per-eye size (default 2064x2208) |
| `--no-pace` | do not emulate vsync (default paces to 90 Hz) |

Every run also checks that `SubmitFrame` leaves the D3D11 pipeline state
exactly as the caller set it.

Exit code 0 and `RESULT: PASS` on the last line mean everything passed.

## 3. OpenXR runtime selection

The runtime is chosen per process; the machine's default OpenXR runtime is
never changed. `InitDesc::runtime` (and `xr_smoke --runtime`) accepts:

| Value | Runtime |
|---|---|
| `virtualdesktop` (default), `vdxr` | Virtual Desktop's VDXR runtime (`virtualdesktop-openxr.json`, found in the registry list of available runtimes or the Streamer's install folder) |
| `steamvr` | SteamVR (`steamxr_win64.json`, found through `%LOCALAPPDATA%\openvr\openvrpaths.vrpath`, the Steam install or the registry list) |
| `system` | the machine default (registry `ActiveRuntime`) |
| `inherit` | whatever `XR_RUNTIME_JSON` already says in the environment |
| a path | that runtime JSON file |

The library sets `XR_RUNTIME_JSON` in its own process before the loader
creates the instance, and also hands the same path to the statically linked
loader as a loader property (`XR_EXT_loader_init_properties`), which works
even in an elevated process where the loader ignores environment variables.

Implicit API layers registered on the machine (OpenXR Toolkit, ReShade,
Virtual Desktop's compatibility layer, ...) load into every OpenXR process.
`xr_smoke` lists them. `--no-implicit-layers` (`InitDesc::disableImplicitApiLayers`)
disables all of them for the process only, through each layer's own
`disable_environment` variable.

### Virtual Desktop (VDXR)

VDXR is the default target (Quest 3 through Virtual Desktop). Without a
headset connected, `xr_smoke --backend openxr --runtime virtualdesktop` loads
the runtime (`VirtualDesktopXR`) and stops at `xrGetSystem` with
`XR_ERROR_FORM_FACTOR_UNAVAILABLE`, reported as `Result::SystemUnavailable`:
the host should keep running flat and call `Init` again later. That much has
been observed. Everything below is expected behaviour that has **not been
tested with a headset yet**:

- The Virtual Desktop Streamer must be running and the headset connected in VD
  before `Init`; otherwise `SystemUnavailable` as above.
- The recommended resolution follows the quality preset chosen in Virtual
  Desktop and can be large; `InitDesc::resolutionScale` or explicit
  `eyeWidth`/`eyeHeight` bound it. Check `maxWidth`/`maxHeight` in `RuntimeInfo`.
- The refresh rate follows the headset setting (72/80/90/120 Hz on Quest 3).
  `RuntimeInfo::refreshHz` comes from `XR_FB_display_refresh_rate` when the
  runtime offers it, otherwise from the predicted display period.
- The session can stay `Visible` without `Focused` while VD's own menu is
  open; frames keep being submitted.
- The FOV is asymmetric and differs per eye; never assume symmetric frusta.
- VDXR runs on the game's GPU; on a machine with several GPUs the D3D11 device
  must be on the adapter the runtime asks for (`GraphicsMismatch` otherwise).
- Virtual Desktop installs the implicit layer
  `XR_APILAYER_VIRTUALDESKTOP_oculus_compatibility`, which is loaded into every
  OpenXR process (seen loaded in the SteamVR runs below).

## 4. OpenXR against SteamVR's null driver

SteamVR ships a "null" driver: a virtual headset that renders into a desktop
window. With it, SteamVR's OpenXR runtime runs a real session without a
headset. It is disabled by default and has to be switched on in SteamVR's
settings file.

### Scripts (`tools\dev`, Windows PowerShell 5.1)

| Script | What it does |
|---|---|
| `steamvr-null-enable.ps1` | backs up `steamvr.vrsettings` once, then switches SteamVR to the null driver |
| `steamvr-null-restore.ps1` | stops SteamVR, puts the original `steamvr.vrsettings` back byte for byte and deletes the backup |
| `steamvr-start.ps1 -Owner <name>` | takes the `.locks\steamvr` lock, starts SteamVR and waits for `vrserver` and `vrcompositor` |
| `steamvr-stop.ps1 -Owner <name>` | stops every SteamVR process and releases the lock |
| `steamvr-common.ps1` | shared helpers (dot-sourced by the others) |

Steam and SteamVR are found through `%LOCALAPPDATA%\openvr\openvrpaths.vrpath`,
the Steam registry key and `libraryfolders.vdf`; no path is hard-coded.

### What changes on the machine

Only one file is edited: `<Steam>\config\steamvr.vrsettings` (the folder listed
under `config` in `openvrpaths.vrpath`). Before the first edit it is copied to
`steamvr.vrsettings.ff7vr-backup` in the same folder; that backup is never
overwritten while it exists, so enabling twice is safe. The keys set are:

```json
"steamvr":     { "forcedDriver": "null", "requireHmd": true, "enableHomeApp": false },
"driver_null": { "enable": true }
```

plus `driver_null.renderWidth`, `renderHeight`, `displayFrequency`,
`windowWidth`, `windowHeight` if the matching parameters are passed to the
enable script. The null driver's own defaults live in
`<SteamVR>\drivers\null\resources\settings\default.vrsettings` (read only, not
modified): `enable: false`, render size 1512x1680 per eye, 90 Hz, a 2160x1200
window at 0,0.

While the null driver is forced, SteamVR ignores real headsets, so **always
run the restore script afterwards**. The system default OpenXR runtime and the
registry are not touched.

### Running the test

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\steamvr-null-enable.ps1 -StopSteamVr
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\steamvr-start.ps1 -Owner dev
xr_smoke --backend openxr --runtime steamvr --frames 300 --capture out\steamvr
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\steamvr-stop.ps1 -Owner dev
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\steamvr-null-restore.ps1
```

`steamvr-start.ps1` is optional (the OpenXR runtime starts SteamVR on demand),
but it makes the first `xrCreateInstance` fast and holds the lock for the
whole run. If anything fails, still run the last two commands.

What a passing run reports (SteamVR 2.18 on the reference machine):

- runtime `SteamVR/OpenXR 2.18.2`, system `SteamVR/OpenXR : null`, vendor `0x28DE`
- eye swapchains 1512x1680, `R8G8B8A8_UNORM_SRGB`, 3 images each; the runtime
  offers `R8G8B8A8_UNORM_SRGB, B8G8R8A8_UNORM_SRGB, R32G32B32A32_FLOAT,
  R16G16B16A16_FLOAT, R10G10B10A2_UNORM` and depth formats
- 90 Hz (`XR_FB_display_refresh_rate`), `XR_KHR_composition_layer_depth` available
- views: identity head pose, eyes at +-31.5 mm, symmetric 45 degree FOV
- session `IDLE -> READY -> SYNCHRONIZED -> VISIBLE (-> FOCUSED)`, every
  `xrEndFrame` accepted, clean `STOPPING -> IDLE -> EXITING` on shutdown
- captures of the swapchain images pass the same checks as the Null backend

Note that the null compositor is not tied to a display: `xrWaitFrame` paces at
about 8.3 ms (120 Hz) although the predicted display period says 11.1 ms.
Frame timing measured against SteamVR null is therefore not representative of
a headset.

Captures show what was written into the runtime's swapchain images, i.e.
exactly what the runtime received; they do not show the compositor's output.

## Conventions the engine side relies on

See the comments in `src/xr/include/ff7vr/xr/xr_math.h` and `xr.h`:

- poses are in the OpenXR LOCAL space plus the library's recenter offset:
  right-handed, +X right, +Y up, -Z forward, metres;
- `ToUnrealPosition` / `ToUnrealQuat` / `ProjectionUnreal` convert to Unreal's
  left-handed, Z-up, X-forward, centimetre conventions;
- `WaitFrame` belongs on the game thread; `BeginFrame`, `SubmitFrame`,
  `SkipFrame` and `RelocateViews` on the render thread, which must own the
  immediate context while they run. Every frame returned with
  `sessionRunning == true` must be begun and then submitted or skipped.
