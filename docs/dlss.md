# DLSS in stereo (optional, `src/engine/src/dlss.cpp`)

NVIDIA DLSS in place of the game's temporal anti-aliasing, per eye, in the stereo rendering
of the engine module. Optional at build time and off by default at run time.

Status: see "What works" below. Everything here was measured headless (Null backend, no
headset).

## Building it

The NVIDIA DLSS SDK is not part of this repository (it has NVIDIA's own licence, below).
The default build does not contain any DLSS code.

```
cmake -S . -B build\<name> -DFF7VR_DLSS=ON      # once; the setting stays in that build folder
powershell -NoProfile -ExecutionPolicy Bypass -File tools\dev\build.ps1 -BuildDir build\<name>
```

With `FF7VR_DLSS=ON` the SDK is taken from `FF7VR_DLSS_SDK_DIR` (default
`third_party/_fetched/dlss-sdk`, ignored by git) or cloned there from
`github.com/NVIDIA/DLSS` at the commit of SDK 310.9.1. The mod links the SDK's static NGX
library (`lib/Windows_x86_64/x64/nvsdk_ngx_s.lib`); the DLSS model itself is NVIDIA's
`nvngx_dlss.dll`, which NGX loads at run time from the game's folder, from the mod's folder
or from `[dlss] dll_dir`.

## How it works

In stereo the engine runs its temporal anti-aliasing once per eye, as one full-screen draw at
the eye's rectangle of double-wide targets (`docs/re/engine.md` section 12). With
`[dlss] enabled = 1` that draw is replaced, on the RHI thread, inside the same hook on the
immediate context's `DrawIndexed` that the bloom and occlusion fixes use:

1. **Recognition.** A full-screen draw whose pixel shader reads a depth view at `t1`, a float
   colour target at `t2` and `t3`, a two-channel 16-bit velocity buffer at `t4`, writes one
   target of `t2`'s size and has the view's uniform buffer at `cb1`. The first match fixes the
   pixel shader; later draws must use the same one (2 per stereo frame, checked).
2. **Jitter.** The engine writes each view's uniform buffer through `Map`/`Unmap` or
   `UpdateSubresource`; hooks on those (and on `CreateBuffer`) keep rows 110-145 of every
   4096-byte constant buffer, keyed by the buffer. At the draw, the rows of the bound `cb1`
   give `TemporalAAJitter`; DLSS gets `x * width / 2`, `-y * height / 2` pixels.
3. **Motion vectors.** A small pixel shader (compiled at run time) draws the eye's
   rectangle of a double-wide `R16G16_FLOAT` texture: camera motion from depth and the view's
   `ClipToPrevClip` (read from the bound `cb1` directly), or the velocity buffer's decoded
   value where something moved, converted to pixels pointing to the previous frame.
4. **DLSS.** One feature per eye (separate histories), created in DLAA mode at the eye's
   size with output sub-rectangles enabled. Each evaluation reads the eye's rectangle of
   the engine's scene colour (`t2`), scene depth (`t1`) and the motion vectors through the
   SDK's sub-rectangle bases, and writes the eye's rectangle of an output texture
   (`R16G16B16A16_FLOAT`, unordered access). Flags: HDR input, inverted depth, low-resolution
   motion vectors, auto exposure.
5. **Back into the engine.** The eye's rectangle of the output is copied into the pass's
   render target (that eye's `TemporalAA` history target), so the bloom, the tonemapper and
   the next frame's history see the DLSS result. The game's draw is skipped.

All of steps 3-5 run inside our own D3D11 pipeline state (`SwapDeviceContextState`), so the
engine's bound state is exactly as it was afterwards. If anything is missing (no captured
view rows, a failed evaluation), the game's own draw runs instead and `dlss status` says why.

NGX is initialised at the first stereo frame on the RHI thread (`NVSDK_NGX_D3D11_Init_with_ProjectID`,
custom engine type), which blocks that thread for about 1 to 1.6 s once.

## What works (measured headless)

Null backend, Quest 3 class asymmetric FOV, RTX 5080, `xr.null_pace = 0`, `t.MaxFPS 0`,
first room and the street outside of the latest save; frame times are `stereo status`
averages over 6 s windows (`stereo frametime 6`); GPU times per eye from timestamp queries
around the motion vector pass, the DLSS evaluation and the copy. Every DLSS number here is
preset L, because the driver override forced it (next section).

- **NGX in this process:** `NVSDK_NGX_D3D11_Init_with_ProjectID` succeeds (custom engine
  type, 0.9 to 1.6 s on the RHI thread, once), `SuperSampling.Available 1`,
  `NeedsUpdatedDriver 0`, `MinDriverVersion 470.0`, `FeatureInitResult 1`. Feature creation
  14 to 100 ms per eye.
- **DLAA in both eyes** (`[dlss] enabled = 1`, mode `dlaa`): the pass is replaced every
  stereo frame (2 per frame, no fallbacks); both eyes show their own view with the right
  parallax; no ghost between the eyes. Compared with the game's anti-aliasing at the same
  spot (third person, still camera, `captures/dlss/r3/sheet_static_1.png`,
  `sheet_static_2.png`): visibly finer detail on textures, cleaner edges on the vent grille
  and the gramophone, no ghosting on the animated crank. Two captures of the still scene
  differ by 0.46 (game) and 0.87 (DLSS) mean levels, game against DLSS by 1.15. Walking out
  of the room (camera translating) and in the street: no smearing or ghosting seen in the
  captures (`r4/sheet_walk.png`, `r4/sheet_street.png`); mean brightness the same (27.8
  against 28.2).
- **History:** after `stereo off` / `stereo on` with DLSS on, the first frames are clean
  (`r3/sheet_after_on.png`; the features are reset when an eye was not evaluated in the
  previous frame). Switching DLSS off releases the features.
- **Other fixes and foveation:** with DLSS on, the bloom fix and the occlusion fix still
  apply once per frame (`applied` = frames, `missed 0`, `failed 0`); foveated rendering on
  or off makes no visible difference to the DLSS image (`r3/sheet_fov.png`; foveation does
  not touch the post-processing passes, and DLSS resolves the coarsely shaded periphery as
  well as the game's anti-aliasing does).
- **Cost: DLAA is too expensive at headset resolution.**

| Eye size | Game's AA | DLSS DLAA (preset L) | GPU per eye (motion vectors + DLSS + copy) |
|---|---|---|---|
| 3072x3264, first room, first person | 9.81 ms | 26.6 ms | 7.8 ms (motion vectors 0.045 ms) |
| 3072x3264, street | 9.82 ms | 25.5 ms | 7.95 ms |
| 3600x3600, first room, third person | 12.58 ms | 34.6 ms | 10.0 ms |

Synthetic cost of one evaluation (`dlss bench`, blank textures, same preset L), per eye:

| Output | DLAA | Quality (0.667) | Balanced (0.58) | Performance (0.5) |
|---|---|---|---|---|
| 3072x3264 | 7.41 ms | 3.92 ms | 3.27 ms | 2.64 ms |
| 3600x3600 | 9.79 ms | 5.03 ms | 4.21 ms | 3.46 ms |

The cost follows the input pixel count more than the output. NVIDIA's table for an RTX 5080
(4K output, Performance mode) gives 2.24 ms for L, 1.74 ms for M and 1.31 ms for J/K, so K
should cost about 0.6 times and M about 0.8 times these numbers (not measured here: the
override forces L).

## Upscale mode (`[dlss] mode = upscale`, with `r.ScreenPercentage` below 100)

Below 100 % screen percentage this engine renders each eye at the reduced size up to and
including the tonemapper, and its last pass (colour grading and grain) scales the result up
into the eye texture (`docs/re/engine.md` section 12). In upscale mode:

1. the anti-aliasing pass only copies the jittered scene colour through (and the motion
   vectors are drawn then, at the reduced size);
2. at each eye's last pass, the same draw is first run once more into a texture of the
   reduced size, with a reduced viewport and scissor and a copy of its pixel shader
   constants whose output rectangle (rows 34/35) is the reduced rectangle, so it grades
   without scaling; the constants are checked once against the viewport and the mode stops
   if they do not match;
3. DLSS (display-referred input, one feature per eye, output sub-rectangles) scales that
   into the eye's rectangle of an `R10G10B10A2_UNORM` texture, which is copied into the eye
   texture; the game's scaling draw is skipped.

The percentage is the game's own console variable, for example in `[stereo_cvars]`:
`r.ScreenPercentage = 50` (Performance), `58` (Balanced), `67` (Quality). With the driver
override described below forcing DLAA as the "performance mode", explicit input and output
sizes still upscale (checked: a 1536x1632 input gives a full 3072x3264 eye image).

Measured at eyes 3072x3264 (first room, third person, still camera; `captures/dlss/r8`,
`r9`; preset L):

| Path | Rendered per eye | Frame time | DLSS GPU per eye |
|---|---|---|---|
| game, 100 % | 3072x3264 | 10.16 ms | - |
| game, 67 % (its own upscale) | 2059x2187 | 6.84 to 7.18 ms | - |
| game, 50 % (its own upscale) | 1536x1632 | 5.35 ms | - |
| DLSS upscale, 67 % | 2059x2187 | 14.23 to 14.35 ms | 3.83 ms |
| DLSS upscale, 58 % | 1782x1893 | 11.80 to 11.93 ms | 3.14 ms |
| DLSS upscale, 50 % | 1536x1632 | 9.81 to 9.92 ms | 2.50 ms |

Picture (`r8/sheet_up_a.png`, `sheet_up_c.png`, `r9/sheet_up_all.png`,
`r9/sheet_up50_R.png`): DLSS from 50 % is close to the native 100 % image in texture detail
and edges and far sharper than the game's own 50 % (blurred, stair-stepped grille edges);
Cloud's dithered hair is grainier at 50 % than at 58 or 67 %; both eyes correct (the right
eye was black before the scissor fix). With preset L this is a quality gain at equal cost,
not a speed gain: DLSS from 50 % costs as much as rendering 100 % with the game's
anti-aliasing. With K (about 0.6 times the cost, inferred from NVIDIA's table) 50 % would
come to roughly 8 ms; not measured.

Not done in upscale mode: a texture mip bias for the lower render size (DLSS guide 3.5;
`r.MipMapLODBias -1` in `[stereo_cvars]` is the candidate, untested), a check of the game's
dynamic resolution (it never changed the view size in any run, also not at 26 ms frames),
and the history reset on camera cuts.

## The NVIDIA App's DLSS override decides the model

NGX applies the driver profile of the game (`ff7remake_.exe`, profile "FINAL FANTASY VII
REMAKE") to every DLSS feature in the process, including the mod's. When the NVIDIA App's
DLSS override is set for this game (a common setting for flat play with a DLSS mod), NGX's
own log shows what it does (lines from the test machine, where the override was set to
DLAA and the latest model):

```
ngx: [NGXSecureLoadFeature] Feature dlss override enabled
ngx: [NGXInitLog] Loaded from path "C:\ProgramData\NVIDIA\NGX\models\dlss\versions\20318464\files/160_E658700.bin" by ff7remake_.exe   (310.9.0, APP_NAME = nvapp_override)
ngx: [NGXOverrideStatusCallback] Feature [SuperSampling] Override Reported: PerformanceMode, Value: 5, Requested: Yes, Applied: Yes
ngx: [NGXOverrideStatusCallback] Feature [SuperSampling] Override Reported: ModelPreset, Value: 12, Requested: Yes, Applied: Yes
ngx: [NgxDltss::FillCreationParams] Info: (DLAA) Using DRS Overridden Preset L      (the same for every mode)
```

So with that override:

- the model is the one the driver downloaded (310.9.0), not the `nvngx_dlss.dll` in the game
  folder (310.6.0, which came with Luma) nor one in `[dlss] dll_dir`;
- every mode runs preset L, whatever `[dlss] preset` asks for (K, J, M and default were all
  measured and all ran L at the same cost);
- the optimal-settings query returns the full size for every mode (PerformanceMode forced
  to DLAA).

The mod cannot opt out of it (the SDK has no setting for this). To compare presets, or to
let the mod's choice apply, the override for this game has to be switched off (or set to
the wanted preset) in the NVIDIA App; that also changes what Luma gets in flat play.

## Preset names and versions

From the SDK's `nvsdk_ngx_defs.h` and programming guide (SDK 310.9.1, guide revision
310.6.0): presets A-D are removed, E and F deprecated, G-I reserved; J and K are the first
transformer model (K the default for DLAA, Quality and Balanced); **L and M were added in
SDK 310.5.0** (November 2025): L the default for Ultra Performance, M the default for
Performance, both "deliver a sharper, more stable image with less ghosting than J, K" at a
higher cost and are "peak performant on RTX 40 series GPUs and above". The SDK does not use
the name "DLSS 4.5"; that the second-generation transformer marketed under it is L/M is an
inference from the dates. A preset is requested per mode with
`NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_<Mode>,
NVSDK_NGX_DLSS_Hint_Render_Preset_<Letter>)` before the feature is created; L/M need a
library of 310.5.0 or later (the game folder's 310.6.0 and the driver's 310.9.0 both
qualify). Exposure: the guide says the exposure texture is used only by J and K, "Preset L
always uses AutoExposure"; the mod sets the auto-exposure flag.

## What the player needs

- An NVIDIA RTX GPU and a driver with NGX (any current GeForce driver).
- `nvngx_dlss.dll` where NGX finds it: next to the game's exe (`End\Binaries\Win64`), or in
  the folder named by `[dlss] dll_dir`.

## Settings (`[dlss]` in `ff7vr.ini`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `0` | `1`: DLSS replaces the game's temporal anti-aliasing while the engine renders in stereo |
| `init` | `0` | `1`: initialise NGX and query DLSS at the first stereo frame even while `enabled = 0` (diagnostics) |
| `mode` | `dlaa` | only `dlaa` (anti-aliasing at the eye size); upscaling modes are not implemented |
| `preset` | `default` | DLSS model: `default` (NVIDIA's choice for the mode), `j`, `k`, `l`, `m` |
| `auto_exposure` | `1` | DLSS computes the exposure itself |
| `mv_jitter` | `2` | how the camera motion vectors treat the jitter: `2` as the engine computes them, not flagged; `1` flagged as jittered; `0` the jitter difference removed |
| `camera_cut_reset` | `0` | reset the history when the view's camera-cut flag is set (field not verified) |
| `dll_dir` | empty | extra folder searched for `nvngx_dlss.dll` (searched before the mod's folder) |
| `log_ngx` | `1` | NGX's own messages in `ff7vr.log` (the first 400) |

## Dev commands

| Command | Effect |
|---|---|
| `dlss status` | NGX state, capability, feature library, the recognised pass, counters, features, GPU time per eye, jitter |
| `dlss on` / `dlss off` | switch while the game runs (the history is reset) |
| `dlss init` | initialise NGX now (at the next stereo frame) |
| `dlss preset <default\|j\|k\|l\|m>` | change the model (features are recreated) |
| `dlss autoexp <0\|1>`, `dlss mvjitter <0\|1\|2>`, `dlss jitter <sx> <sy>`, `dlss cutreset <0\|1>` | tests |
| `dlss reset`, `dlss recreate` | reset the history, release and recreate the features |
| `dlss dump` | log the view uniform buffer rows 110-145 of the next two views |
| `dlss timing` | restart the GPU time averages |

## Licence of the NVIDIA DLSS SDK

The SDK (`LICENSE.txt` in the SDK: "NVIDIA RTX SDKs LICENSE", v. March 14, 2024, with the
"NVIDIA RTX SUPPLEMENT") governs the headers, the static NGX library and `nvngx_dlss.dll`.
The conditions that matter for this project, quoted:

- Grant, 1.c: "Distribute any software and materials within the SDK, other than developer
  tools provided for your internal use, as incorporated in object code format into a
  software application subject to the distribution requirements indicated in this license."
- 2.a: "An application must have material additional functionality, beyond the included
  portions of the SDK."
- 2.b: "The following notice shall be included in modifications and derivative works of
  source code distributed: 'This software contains source code provided by NVIDIA
  Corporation.'"
- 2.c: "You agree to distribute the SDK subject to the terms at least as protective as the
  terms of this license ..."
- 4.b: "Except as expressly provided in this license, you may not copy, sell, rent,
  sublicense, transfer, distribute, modify, or create derivative works of any portion of
  the SDK. For clarity, you may not distribute or sublicense the SDK as a stand-alone
  product."
- 4.e: "You may not use the SDK in any manner that would cause it to become subject to an
  open source software license. As examples, licenses that require as a condition of use,
  modification, and/or distribution that the SDK be: (i) disclosed or distributed in source
  code form; (ii) licensed for the purpose of making derivative works; or (iii)
  redistributable at no charge."
- Supplement 1: "the DLSS SDK, NGX SDK and RTX Dynamic Vibrance SDK are licensed for you to
  develop applications only for their use in systems with NVIDIA GPUs."
- Supplement 4: "You are required to notify NVIDIA prior to commercial release of an
  application (including a plug-in to a commercial application) that incorporates, or is
  based on, the DLSS SDK ..." (form at developer.nvidia.com/sw-notification).
- Supplement 7.1(b): "For applications that incorporate the DLSS SDK or NGX SDK or portions
  thereof, you must attribute the use of the applicable SDK and include the NVIDIA Marks on
  splash screens, in the about box of the application (if present), and in credits for game
  applications."
- Programming guide 4.2.3: the DLSS DLL "includes third party code that needs to be
  acknowledged in the public documentation of the final product"; the texts are in the
  guide's section 9.6 (curl, an SGI bitmap font, d3dx12.h).

What follows for this repository (an MIT-licensed project): the SDK's headers, libraries
and DLLs must not be committed (they would become source-distributed under MIT, 4.b and
4.e), which is why the build fetches them and why the code builds without them. The mod's
own source that calls the SDK is not "source code provided by NVIDIA" (the calls follow the
SDK's documented helper functions; no SDK source was copied into it). Whether a binary
release that links the NGX library and ships `nvngx_dlss.dll` is compatible with releasing
the mod under MIT is a question for the project owner (4.e names "redistributable at no
charge" as an example of a licence condition it forbids for the SDK itself; the SDK parts
would have to be under NVIDIA's terms in such a release, not MIT). A release would also
need the attribution of 7.1(b) and the DLL's third-party notices.
