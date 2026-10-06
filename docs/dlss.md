# DLSS in stereo (optional, `src/engine/src/dlss.cpp`)

NVIDIA DLSS in place of the game's temporal anti-aliasing, per eye, in the stereo rendering
of the engine module. Optional at build time and off by default at run time.

Status: two modes, upscale (DLSS scales each eye up to its full size from the rendered part,
`[dlss] input_scale`; the useful one) and DLAA (anti-aliasing at the rendered size; too slow
at headset resolution). The history is reset on camera cuts; upscale mode works with
`[stereo] render_scale` and dynamic resolution (under the NVIDIA App's DLSS override each
change of the size recreates the features, a hitch) and always hands the runtime the full
eye. Everything here was measured headless (Null backend, no headset); nothing has been seen
in a headset yet. Not handled: texture mip bias in upscale mode, mono frames. **GPU faults:**
three driver resets in the test runs with DLSS on; the same kind of fault also happened once
without DLSS, and in isolation runs DLSS failed only together with foveated rendering
(variable rate shading): run DLSS with `[foveation] enabled = 0` until that is understood
(section "GPU faults").

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
   `UpdateSubresource`; hooks on those (and on `CreateBuffer`) keep rows 50-145 of every
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

Lifetimes: a feature that is replaced (a new input size, a changed flag, `dlss off`,
`dlss recreate`) and every texture an evaluation reads or writes that is replaced (the
motion vectors, the outputs, the graded copy) is released only after the GPU has finished
the frame that last used it: it waits for an event query issued at that frame's end and at
least three more frames (`dlss status`: "deferred release"). The SDK guide requires this for
features (section 5.5: a feature handle should only be released once the command lists used
in its evaluations are no longer in flight). The engine's depth texture that an evaluation
reads is held the same way. NGX itself delays the destruction of a released feature's
resources by about 10 s (its verbose log: `NGXStoreCallToReleaseFeature`, then
`NGXCubinGeneric::CollectGarbage` exactly 10 s later); DLSS on D3D11 runs NVIDIA's kernels
through the driver ("cubin" kernels), and NGX keeps per-resource caches keyed by the
resource pointer.

NGX is initialised at the first stereo frame on the RHI thread (`NVSDK_NGX_D3D11_Init_with_ProjectID`,
custom engine type), which blocks that thread for about 1 to 1.6 s once.

Credit: which draw is the anti-aliasing pass, its input slots, the rows of the view buffer
and the jitter scale were first read in the FF7 Remake module of the Luma framework (a
flat-screen DLSS mod for this game, github.com/Filoppi/Luma-Framework, by Filippo Tarpini
and contributors) and then checked in this game in stereo (`docs/re/engine.md` section 12).
No code was taken from it; the motion vector conversion is Unreal Engine 4's standard one.

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
- **Stability on a still scene** (jitter shimmer): four captures in a row per setting, third
  person, mean change between consecutive captures in two regions without animated objects
  (the counter; the wall and vent), `captures/dlss/r11`:

  | Setting | Counter | Wall and vent |
  |---|---|---|
  | game's anti-aliasing | 0.36 (0.57 in a second burst) | 0.33 (0.54) |
  | DLAA, `mv_jitter = 2` (default) | 0.36 | 0.36 |
  | DLAA, `mv_jitter = 0` (jitter difference removed) | 0.76 | 0.90 |
  | DLAA, `mv_jitter = 1` (flagged as jittered) | 0.70 | 0.82 |
  | upscale from 50 % | 0.10 | 0.09 |

  So the camera motion the engine's `ClipToPrevClip` gives is already free of jitter for
  DLSS's purposes (the default), and the other two settings make the image shimmer.
- **Head turning** (Null backend `xr.null_motion = yaw`, `captures/dlss/r13/sheet_yaw_lamp.png`):
  the hanging lamp and the ceiling planks while the head turns, with the game's
  anti-aliasing, DLAA and upscale from 50 %: no doubled bulb, no smearing, the planks stay
  sharper with DLSS.
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

## Upscale mode (`[dlss] mode = upscale`, the default)

Each eye is rendered at a part of its size, set by `[dlss] input_scale` (which sets
`[stereo] render_scale`: the views' rectangles shrink inside full-size targets, see
`docs/engine-module.md`), by `r.ScreenPercentage`, or both (they multiply). The engine runs
everything up to and including the tonemapper at that size, and its last pass (colour grading
and grain) writes the eye texture: scaled up to the full eye with `r.ScreenPercentage`, at
the view's rectangle with `render_scale` (`docs/re/engine.md` section 12). In upscale mode:

1. the anti-aliasing pass only copies the jittered scene colour through (and the motion
   vectors are drawn then, at the reduced size);
2. at each eye's last pass, the same draw is first run once more into a texture of the
   reduced size, with a reduced viewport and scissor and a copy of its pixel shader
   constants whose output rectangle (rows 34/35) is the reduced rectangle, so it grades
   without scaling; the constants are checked once against the viewport and the mode stops
   if they do not match;
3. DLSS (display-referred input, one feature per eye, output sub-rectangles) scales that
   into the eye's whole half of an `R10G10B10A2_UNORM` texture, which is copied into the eye
   texture; the game's draw is skipped;
4. the eye rectangles handed to the runtime (`render_host.cpp`) become the whole halves for
   the eyes DLSS wrote that frame, so the headset gets the full-size image also when the views
   rendered less (`render_scale`, dynamic resolution); an eye that fell back to the game's
   pass keeps its rectangle.

With the driver override described below forcing DLAA as the "performance mode", explicit
input and output sizes still upscale (checked: a 1536x1632 input gives a full 3072x3264 eye
image).

**`input_scale` (`render_scale`) or `r.ScreenPercentage`?** Use `[dlss] input_scale`
(0.5, 0.58, 0.67 for NVIDIA's Performance, Balanced and Quality ratios). It changes only the
views' rectangles: no reallocation when it changes (`r.ScreenPercentage` changes reallocate the
scene buffers, 54 to 138 ms hitches), it is what `[stereo] dynamic_resolution` moves, and
the rectangles are multiples of 8 at the start of each half, so the right eye's view never
overhangs a scaled buffer (at 58 and 67 % of a 3072-wide eye, `r.ScreenPercentage` makes the
right view reach 1 to 3 pixels past the buffer; the bloom and occlusion fixes clamp that
since `bbe071a`, the DLSS pass clips it). `r.ScreenPercentage` through `[stereo_cvars]`
still works with DLSS (the input is then the product of both, the output still the full eye)
and was checked once together with `render_scale` (`captures/dlss/r22`); it only keeps the
hitch.

### The numbers (preset L, measured; K and M inferred)

Null backend, Quest 3 class asymmetric field of view, RTX 5080, `xr.null_pace = 0`, frame cap
lifted; upscale mode through `render_scale` (`[dlss] input_scale`); frame times are the
average and 95th percentile of 6 s windows (`stereo status`), DLSS GPU time per eye from
timestamp queries around the motion vectors, the evaluation and the copy. Room = the first
room of the test save, third person, still camera; street = the alley outside, first person,
reached by walking (`captures/dlss/r23` at 3072x3264, `r24` at 3600x3600; the game's own
anti-aliasing at 100 % measured in the same run as the reference). Every DLSS number is
preset L, forced by the NVIDIA App override on the test machine. The K and M columns are
**inferred, not measured**: the frame time with the DLSS share scaled by NVIDIA's own cost
ratios for an RTX 5080 (guide, 4K Performance: L 2.24 ms, M 1.74 ms, K 1.31 ms), and the
DLSS time per eye scaled the same way.

| Eye | Scene | Path | Input per eye | Frame ms avg (p95) | DLSS ms per eye, L (measured) | K inferred: frame / DLSS per eye | M inferred: frame / DLSS per eye | Picture (from the sheets) |
|---|---|---|---|---|---|---|---|---|
| 3072x3264 | room | game's anti-aliasing, 100 % | 3072x3264 | 9.91 (10.35) | - | - | - | the reference |
| 3072x3264 | room | DLSS from 0.5 | 1536x1632 | 10.49 (10.94) | 2.50 | 8.4 / 1.46 | 9.4 / 1.94 | edges and textures close to 100 %; hair grainy; grain in soft shadow edges |
| 3072x3264 | room | DLSS from 0.58 | 1784x1896 | 12.49 (13.03) | 3.14 | 9.9 / 1.84 | 11.1 / 2.44 | as 0.5, hair less grainy |
| 3072x3264 | room | DLSS from 0.67 | 2056x2184 | 14.69 (15.12) | 3.83 | 11.5 / 2.24 | 13.0 / 2.98 | closest to 100 %; hair about as at 100 % |
| 3072x3264 | room | game, render scale 0.5 (runtime scales up) | 1536x1632 | not timed in this run | - | - | - | clearly blurred (`sheet_room.png`) |
| 3072x3264 | street | game's anti-aliasing, 100 % | 3072x3264 | 9.10 (9.46) | - | - | - | the reference |
| 3072x3264 | street | DLSS from 0.5 | 1536x1632 | 9.56 (9.95) | 2.53 | 7.5 / 1.48 | 8.4 / 1.96 | bricks, stairs, walls close to 100 %; shadowed ground a little crunchier |
| 3072x3264 | street | DLSS from 0.58 | 1784x1896 | 11.35 (11.66) | 3.17 | 8.7 / 1.85 | 9.9 / 2.46 | as 0.5 |
| 3072x3264 | street | DLSS from 0.67 | 2056x2184 | 13.51 (14.00) | 3.92 | 10.3 / 2.29 | 11.8 / 3.04 | closest to 100 % |
| 3600x3600 | room | game's anti-aliasing, 100 % | 3600x3600 | 14.43 (14.90) | - | - | - | the reference (not captured) |
| 3600x3600 | room | DLSS from 0.5 | 1800x1800 | 13.11 (13.64) | 3.30 | 10.4 / 1.93 | 11.6 / 2.57 | not captured (same ratios as at 3072x3264) |
| 3600x3600 | room | DLSS from 0.58 | 2088x2088 | 15.41 (15.76) | 4.06 | 12.0 / 2.38 | 13.6 / 3.16 | not captured (same ratios as at 3072x3264) |
| 3600x3600 | room | DLSS from 0.67 | 2416x2416 | 18.36 (18.81) | 4.98 | 14.2 / 2.91 | 16.1 / 3.86 | not captured (same ratios as at 3072x3264) |
| 3600x3600 | room | game, render scale 0.5 (runtime scales up) | 1800x1800 | 6.34 (6.72) | - | - | - | blurred (as at 3072x3264) |
| 3600x3600 | street | game's anti-aliasing, 100 % | 3600x3600 | 11.55 (12.55) | - | - | - | the reference (not captured) |
| 3600x3600 | street | DLSS from 0.5 | 1800x1800 | 12.06 (12.50) | 3.34 | 9.3 / 1.95 | 10.6 / 2.59 | not captured (same ratios as at 3072x3264) |
| 3600x3600 | street | DLSS from 0.58 | 2088x2088 | 14.23 (14.68) | 4.12 | 10.8 / 2.41 | 12.4 / 3.20 | not captured (same ratios as at 3072x3264) |
| 3600x3600 | street | DLSS from 0.67 | 2416x2416 | 17.03 (17.36) | 5.06 | 12.8 / 2.96 | 14.8 / 3.93 | not captured (same ratios as at 3072x3264) |
| 3600x3600 | street | game, render scale 0.5 (runtime scales up) | 1800x1800 | 5.23 (5.62) | - | - | - | blurred (as at 3072x3264) |

What the table says, with preset L as forced now:

- At 3072x3264, DLSS from 0.5 costs 5 to 6 % more than the game's own anti-aliasing at
  100 % (10.49 against 9.91 ms in the room, 9.56 against 9.10 in the street) for a picture
  close to it: with L it is a picture choice, not a speed gain. 0.58 costs about 25 % more,
  0.67 about 48 % more. The game itself at 0.5 without DLSS is more than twice as fast (6.34
  against 14.43 ms at 3600x3600) but blurred.
- At 3600x3600, DLSS from 0.5 is 9 % faster than the game at 100 % in the room (13.11
  against 14.43 ms) and 4 % slower in the street (12.06 against 11.55). Headless, that is
  inside 72 Hz (13.9 ms) at both spots, before Virtual Desktop's encoding; 0.58 and 0.67
  are not.
- DLSS costs 2.5 ms per eye at 3072x3264 from 0.5 and 3.3 ms at 3600x3600; it grows with
  the input (about 3.9 and 5.0 ms at 0.67). Two eyes double it: at 0.5 and 3072x3264 DLSS
  is 5 of the 10.5 ms.
- Inferred, not measured: with preset K the same frames would take 2 to 2.7 ms less at 0.5
  (8.4 and 7.5 ms at 3072x3264, 10.4 and 9.3 at 3600x3600), 15 to 28 % faster than the
  game at 100 %; with M 1 to 1.5 ms less. Whether K's picture at 0.5 holds up against L's is
  not known here (the override prevents the comparison).

### Picture at 50 to 67 %

What DLSS from a reduced input looks like against the game's own anti-aliasing at 100 %
(left eye, eyes 3072x3264, preset L; `captures/dlss/r23/sheet_room.png`,
`sheet_street.png`; the street spot of r23 is an alley, the one of r12 the yard behind it):

- **Edges and texture detail:** at 0.5, 0.58 and 0.67 close to the 100 % image (bricks,
  stairs, the gramophone, the vent bars); rendering at 0.5 without DLSS (the runtime scales
  the image) is clearly blurred.
- **Hair:** Cloud's dithered hair is grainy at 0.5 (dark speckled strands), less at 0.58,
  about like the 100 % image at 0.67.
- **Shadows:** where a shadow edge is soft (the penumbra under the pipe and on the wall in the
  yard, `r12/sheet_street_up.png`), 0.5 shows a fine grain that the 100 % image does not
  have. In the alley and the room (`r23/sheet_st_tests.png`) the shadowed ground and walls at
  0.5 are as clean as at 100 %: a count of isolated bright dots in shadow (pixels more than
  6 levels above the median of their 3x3 neighbours, among pixels whose median is below 50)
  gave 0.22 % at 0.5 against 0.26 % at 100 % (`speckle.py`, a quarter of the eye with
  shadowed ground, 4 captures each). On a still camera in the room DLSS from 0.5 is calmer
  than the game's anti-aliasing (mean change between captures in dark areas 0.25 against
  0.50 levels, `r21`).

Cheap settings tried at 0.5 for the shadow grain (street, `r23` and `r25`, preset L):

| Setting at 0.5 | Isolated bright dots in shadow, alley (`r23`, >6 / >12 levels) | Seen in the crops (`r23/sheet_st_tests.png`) |
|---|---|---|
| game's anti-aliasing at 100 % (reference) | 0.26 % / 0.04 % | - |
| DLSS defaults | 0.22 % / 0.04 % | as the reference |
| `dlss autoexp 0` (no auto exposure; preset L ignores it per the guide) | 0.17 % / 0.03 % | no visible change |
| `dlss hdr 1` (input flagged HDR, 16-bit internal) | 0.24 % / 0.05 % | no visible change |
| `dlss mvjitter 1` (motion vectors flagged as jittered) | 0.06 % / 0.01 % | softer overall (fewer dots because less detail) |
| `dlss nograin 1` (last pass without its noise texture) | 0.31 % / 0.05 % | no visible change |
| `dlss sharpness`, `preexp` | not measured | `InSharpness` is deprecated (guide 3.11: the SDK's sharpening pass was removed); `InPreExposure` is for engines that pre-multiply exposure into HDR input (guide 3.9.2), which the display-referred input here is not |

So the grain comes with the input, not with the last pass or with DLSS's exposure: the
game's soft shadows (and the hair) are dithered, its anti-aliasing at 100 % averages that
in linear HDR before tonemapping, and at a quarter of the pixels the dither is coarser than
what DLSS (display-referred, after grading) smooths out. What helps is more input: 0.58
or 0.67. `[dlss] mv_jitter = 2` (the default) stays right in upscale mode; `1` softens and
in DLAA made the image shimmer (table above, "Stability"). `NVSDK_NGX_DLSS_Feature_Flags_MVLowRes`
is required here (the motion vectors are drawn at the input size; without the flag DLSS
would read them as output-size vectors). Texture mip bias for the lower render size (DLSS
guide 3.5): `r.MipMapLODBias -1` at 0.5 showed no clear difference in the crops checked
(`r14/sheet_bias.png`, the counter and the gramophone), so it is left to the player
(`[stereo_cvars] r.MipMapLODBias = -1`); the flat-screen Luma mod instead adds a bias to the
game's samplers (recreated with `MipLODBias`), which was not tried. `render_scale` and
`r.ScreenPercentage` at the same input give the same texture sharpness
(`r22/sheet_rs_vs_sp.png`).

### DLSS before the tonemapper (not built; what it would take)

NVIDIA's guide (3.1) puts DLSS before tonemapping, as early in post-processing as possible:
here that is the anti-aliasing pass, with linear HDR input (the scene colour before grading
and grain) and a full-size output. Then every pass after it must run at the full eye size:
the bloom chain (about 6 levels per eye, its first pass with the right-eye fix of
`docs/engine-module.md`), eye adaptation, the tonemapper, the glare pass and the last pass.
This engine (4.18) has no notion of an upscale inside the anti-aliasing pass (UE's TAAU and
its two view rectangles came in 4.19), so these passes take their size from the view
rectangle. Two ways, both untried:

1. Retarget each later pass by hand, as the last pass is retargeted now: recognise it, give
   it full-size targets of our own, patch its viewport and the rectangles in its constants
   (each pass has its own layout, found with `gpu trace` dumps). About 15 to 25 draws per eye,
   and the bloom, occlusion and reflection fixes of the right eye work on these rectangles
   too. Estimate: two to three days, with GPU traces and per-eye checks of every pass.
2. With `[stereo] render_scale` the scene buffers already have the full size: change the
   views' rectangles from the reduced to the full size between the anti-aliasing pass and
   the rest of post-processing (the `FViewInfo` rectangle and rows 121-126 of the view
   constants, the post-processing hook point the foveation code already has), so that the
   engine itself sizes the later passes for the full eye. Fewer patches, but it touches how
   the engine builds the post-processing chain of a frame (4.18 builds it from the view
   rectangle when post-processing starts, with the anti-aliasing pass inside that chain).
   Estimate: one to two days to find out whether it holds, more if passes cache the size.

What it would gain: DLSS would see the image before the grain and the colour grading, at
16-bit precision, and noise that the game's anti-aliasing averages before the tonemapper
(dithered shadows, hair) would reach DLSS as such and be resolved instead of upscaled as
detail; the later passes would cost their full-size price (about 0.9 ms of post-processing
at 3072x3264, `docs/engine-module.md`). Not attempted: the display-referred route turned
out usable (section "Picture at 50 to 67 %").

### Dynamic resolution

DLSS supports an input that changes size from frame to frame within a range NGX gives per
output size and mode (guide 3.2.2): the feature is created for the largest input, and each
evaluation names the current rectangle. The mod asks NGX for that range
(`dlss: input range for output ...` in the log) and uses it when the largest input of
`[stereo] render_scale` and the current input are inside it; a change of the scale then
changes only the rectangle. Otherwise each eye's feature is created for exactly the current
input and recreated when it changes.

On the test machine the NVIDIA App override (next section) forces the DLAA "performance
mode", NGX reports the range 3041x3231 to 3072x3264 for a 3072x3264 output in every mode, and
so every scale change of `[stereo] dynamic_resolution` recreates both features: measured
14 to 17 ms per eye on the RHI thread (`captures/dlss/r22`, `dynres` with min 0.5 and
`render_scale` 0.67: changes 0.67 -> 0.56 -> 0.50 and back up, each logged as two
`dlss: feature eye ...` lines), a longest frame of 31 to 43 ms in the 6 s windows with a step
(11 to 14 ms in windows without one, frames otherwise 10.5 to 12.5 ms), no failed evaluation,
the eye image handed to the runtime always the full 3072x3264. With the override off the
range would be NGX's own for the mode (not measurable here); NVIDIA notes that presets L and
M then cost as much as their largest input, whatever the frame's input is (guide 3.2.2.1).
So with the override as it is, use a fixed `input_scale` and leave `dynamic_resolution` off
while DLSS is on.

An earlier version created the features for the largest input without asking for the
range: NGX refused the smaller input ("Dynamic scaling disabled as Creation time res
outside of dynamic scale range. RenderSubrect (1536x1632) must match Creation time res
(2058x2186)") and the failed evaluation was followed by a GPU fault and a driver reset
(System log `nvlddmkm` event 153 at that second, `captures/dlss/r21`). The mod now never
evaluates outside the range NGX gives.

## GPU faults seen in the test runs

Three times during the headless runs of the upscale mode the GPU faulted, the driver reset
(System log: `nvlddmkm` event 153, `Error occurred on GPUID`), the game's device was
removed (`0x887A0005` in the next D3D call) and the game stopped presenting:

1. `captures/dlss/r21`, 02:02:50: an evaluation with an input smaller than the feature's
   creation size while NGX allowed no dynamic range (section "Dynamic resolution"). NGX
   logged the refusal on both eyes, then the fault. Fixed: the input range is now asked of
   NGX and never left.
2. `r22`, 02:14:27: 9 s after switching to `r.ScreenPercentage 50` with `render_scale` 1
   (features recreated twice in that second), during the second `capture` of a burst. No NGX
   message.
3. `r23`, 02:23:18: in the alley at `render_scale` 0.5, 15 s after the features were
   recreated for a test switch, during the fourth `capture` of a burst with `dlss nograin 1`.
   No NGX message.

### What the faults have in common, and what they do not

- The game's own crash report says which kind of device loss it was:
  `Documents\My Games\FINAL FANTASY VII REMAKE\Saved\Crashes\<id>\CrashContext.runtime-xml`,
  `<ErrorMessage>`. All three: "Unreal Engine is exiting due to D3D device being lost.
  (Error: 0x887A0006 - 'HUNG')", that is `DXGI_ERROR_DEVICE_HUNG`: the GPU stopped while
  executing this process's commands (a loss caused by another process reads `RESET`).
- Faults 2 and 3 came 0.80 s and 0.77 s after a `capture` was requested. The capture itself
  completed in both (its images were written, so the GPU was working at its read-back), and
  the reset followed in less than the 2 s TDR delay: a fault the driver detected, not a
  timeout.
- The same kind of fault happened once without DLSS: in a development run the day before
  (before any DLSS code existed), 1.1 s after a `capture` request, also `HUNG`, also on the
  Null backend with foveated rendering on. Counting all development runs: 1 such fault in
  about 490 captures without DLSS, 2 in about 160 captures in the DLSS runs, which is not a
  significant difference.
- Foveated rendering was on in all three faults and in the one without DLSS.
- No D3D11 debug layer was available (the Windows "Graphics Tools" feature is not installed
  and installing it needs administrator rights), so the hazards were looked for by reading
  the code and by the isolation runs below.

### Hazards looked for in the code

- A feature released while its last evaluation may still be on the GPU: found (every size
  change, flag change and switch-off released it at once, against the SDK guide) and fixed
  (see "Lifetimes" in "How it works"). NGX's own 10 s delay before it frees a released
  feature's resources probably covered it already; the faults do not line up with those
  10 s.
- Textures NGX reads or writes released or resized while in use: our textures are now kept
  until the GPU has finished the frame; the engine's depth texture is held the same way.
- The immediate context used from two threads: not found. The anti-aliasing and last-pass
  replacements, the frame end (`dlss::frame`), the Present hook, the XR submission and the
  capture's blit and read-back all run on the engine's RHI thread; the capture's worker
  thread only encodes PNG files; dev commands only set atomics. The game's device has no
  multithread protection (creation flags 0), so a second thread would be a real hazard.
- The capture's read-back (`CaptureEye` in `src/xr/src/capture.cpp`) maps its staging
  texture with a blocking `Map`: the RHI thread waits until the GPU has drained, then
  converts the pixels (20-40 ms with the GPU idle). That is legal D3D11, but it is the event
  the faults followed.

### Isolation runs

All on the Null backend at 3072x3264, upscale mode at 0.58 with changes between 0.5 and
0.58 (each one recreates both features under the NVIDIA App's override), walking between
the first room and the street with the emulated head turning (`xr.null_motion = yaw`). "Late"
means the run started at render scale 1 with DLSS off and switched both on by command,
which is what the earlier faulting runs did; started from the ini with `input_scale` below 1,
foveated rendering does not find the eye views and stays off (its log says "view rect ...
not found in the eye views").

| Run | Foveated rendering | Load | Length | Result |
|---|---|---|---|---|
| r31 | off (ini start) | capture every ~4 s (74), recreation every 20 s (24), scale change every 30 s (18) | 10.1 min | clean |
| r32 | **on** (late) | `dlss stall` every ~3 s (62), recreation every 30 s, scale change every 60 s | 2.9 min | **hang**: no GPU reset, the game stopped presenting; Unreal's 30 s render thread watchdog ended it |
| r33 | **on** (late) | as r31 | 2.7 min | **hang**, same place as r32 |
| r34 | off (`[foveation] enabled = 0`, late) | as r31 (79 captures, 26 recreations, 18 scale changes) | 10 min | clean; no warning or error in the log |
| r36 | **on** (late), DLSS off (NGX not started) | as r31 without DLSS (82 captures, 18 scale changes) | 10 min | clean |
| r35 (soak) | off (`[foveation] enabled = 0`, ini start as a player would have it) | capture every 15 s (64), scale change every 60 s (19, each recreating both features) | 20 min | clean: no driver event, no warning, error or NGX error in the log, no failed evaluation; frame times per 30 s window 10.3 to 12.6 ms average, 95th percentile 11.1 to 13.6 ms, no drift |

In both hangs the RHI thread was blocked inside the NVIDIA driver in the engine's own
`Flush` after its per-frame query (`docs/re/engine.md` section 13), with none of the mod's
code on the blocked call chain; the GPU had stopped finishing work without being reset.
With foveated rendering off, the same DLSS load ran clean for 40 minutes in all (r31, r34
and the 20-minute soak r35); foveated rendering with the same load but without DLSS ran
clean for 10 minutes (r36). Only the two together failed, both times within 3 minutes.

### Conclusion so far

The faults are not caused by DLSS alone: the same kind appeared once without DLSS, and
with DLSS the runs fail only while foveated rendering is on. Foveated rendering uses
NVIDIA's variable rate shading through NVAPI (`src/render/src/foveation.cpp`). It is not
simply "variable rate shading plus DLSS": the first DLSS runs (about 100 captures with
DLSS and foveated rendering both on, `r.ScreenPercentage` changes, few feature
recreations) had no fault. The faulting runs added changes of `[stereo] render_scale`
(view rectangles inside full-size targets, for which foveated rendering rebuilds its
shading-rate surface each time), frequent feature recreations, and a newer foveated
rendering. Which part is involved is not known yet. Until then, run DLSS with
`[foveation] enabled = 0`; started from the ini with an `input_scale` below 1, foveated
rendering is off anyway (see above).

## Camera cuts

Each eye's history is reset (`InReset`) when its camera jumps, decided per eye and frame
from the view constants the anti-aliasing pass reads (`docs/re/engine.md` section 12):

- **engine**: the camera the engine's motion is relative to (`PrevWorldCameraOrigin`, row
  103) is not the camera of the eye's previous frame (row 59 one frame earlier). The engine
  resets its previous-frame matrices on its own cuts (cutscene cuts, teleports, a new view
  state, and turns of more than 45 degrees in one frame); then the motion vectors of
  everything static are zero although the picture changed.
- **distance / angle**: the camera moved more than `cut_distance` (100 cm) or turned more
  than `cut_angle` (30 degrees, from `ViewForward`, row 52) since the eye's previous frame.
  This catches the mod's own jumps: a switch between the game's camera and the first or
  third person camera, recentering. The 0.35 s blend between first and third person stays
  far below it (about 8 cm per frame). The player module's camera mode changes are not
  passed to DLSS directly: they are decided on the game thread one or two frames before the
  RHI thread renders them, so a frame-exact signal would have to travel with the frame
  through the stereo device's frame queue; the view constants are already frame-exact.
- Switching stereo or DLSS on, and an eye that was not evaluated in the previous frame,
  already reset as before.
- Row 140 `.y` (a camera-cut flag in a flat-screen mod's reading) stayed 0 in every frame,
  also on the engine's own resets; it is only counted (`dlss cutflag 1` would use it).

Tested headless with the Null backend's emulated head (`captures/dlss/r21`, room, third
person, upscale from render scale 0.5, preset L): a 150 degree turn in one frame
(`xr-sim head 150`) was detected on both eyes as `engine angle` (the engine had reset its
previous camera itself), a 2.5 m jump of the head (`xr-sim head 0 0 0 0 2.5`) as
`distance` (the engine kept its previous camera, `engine` did not fire), and nothing else was
detected in the run. `dlss cuttest` wrote the left eye's image of the frames after each
cut, with and without the reset; mean difference to the settled frame 60 (0-255 levels)
and the share of pixels more than 16 levels off:

| Cut | Motion vectors of the cut frame | No reset: frame 1 / frame 2 | Reset: frame 1 / frame 2 |
|---|---|---|---|
| 150 degree turn | zero (as the engine gave them) | 1.12, 0.12 % / 0.94, 0.05 % | 1.04, 0.08 % / 0.90, 0.04 % |
| 2.5 m jump | zero (as on an engine cut) | 1.29, 0.49 % / 1.06, 0.27 % | 1.15, 0.25 % / 0.98, 0.17 % |

After a full change of view DLSS discards the old history by itself (no ghost either way,
`sheet_cut_first.png`). Where the old and the new view overlap and the motion vectors say
nothing moved, as on the engine's cuts, the frame after the cut shows a doubled, smeared copy
of the old view without the reset (the door frame and the sword in `sheet_cutpos_f1.png`)
and is clean with it. Not tested: cuts of real cutscenes (no cutscene is reachable from the
test save), the game-camera to first-person switch in a real conversation.

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

The override also takes away DLSS's dynamic input range (99 to 100 % of the output in every
mode), so a change of the input size recreates the features (section "Dynamic resolution").

**Letting the mod choose (steps in the NVIDIA App, not done on the test machine):** NVIDIA
App, `Graphics`, `Program Settings`, choose `FINAL FANTASY VII REMAKE INTERGRADE` in the
program list; in its settings find the DLSS override entries (in current versions
`DLSS Override - Model Presets` and `DLSS Override - Super Resolution Mode`, the names vary
between versions) and set both back to the application's choice (`Off` / "use the 3D
application setting"); apply. The NGX lines `Override Reported: ... Applied: Yes` and
`Using DRS Overridden Preset L` then disappear from `ff7vr.log`, and `[dlss] preset` and the
mode the mod asks for apply. Alternatively set the model override to the preset wanted for
both flat and VR play.

What that changes for flat play with Luma: the override applies to the program
`ff7remake_.exe`, which flat play and VR play share; there is no separate profile per mod.
With it off, Luma's DLSS uses Luma's own choices again: its `DLSS Preset` setting (in its
ReShade overlay menu; "Default" means NVIDIA's default for the mode, K for DLAA and Quality)
and the render resolution Luma asks for instead of the forced DLAA, and the model from the
`nvngx_dlss.dll` in the game folder (310.6.0, which has L and M) instead of the driver's
downloaded 310.9.0. To keep flat play as it is now (preset L at DLAA), select preset L and
DLAA in Luma's settings. Not checked in flat play here.

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

## Interactions

| With | Result | How it was checked |
|---|---|---|
| Foveated rendering (variable rate shading) | works; it shades the scene before the anti-aliasing pass, DLSS resolves the periphery like the game's pass does | `fov off` / `fov on` with DLSS on, `r3/sheet_fov.png` |
| Bloom and ambient occlusion fixes | still applied once per stereo frame at 100 % | `stereo bloomfix`, `stereo aofix` counters with DLSS on |
| Bloom and ambient occlusion fixes below 100 % | with `r.ScreenPercentage` 58 and 67 at eyes 3072 wide the right view's rectangle reaches 1-3 pixels past the scaled buffer; before `bbe071a` the fixes skipped those frames (`missed 4759`, `failed 4844` in `r9`, the right eye showed the left eye's bloom and occlusion ghost, `r8/sheet_up_R.png`), since then they clamp the rectangle. With `input_scale` / `render_scale` the rectangles never overhang | counters, captures |
| Light sort-key fix, UI layer | unaffected (the UI is drawn into its own layer; the light fix is in the scene) | captures show the HUD layer and lit scenes as before |
| Stereo off and on | the features keep their size; an eye not evaluated in the previous frame is reset; first frames clean | `r3/sheet_after_on.png` |
| Camera cuts | the eye's history is reset (section "Camera cuts") | `dlss cuttest`, `captures/dlss/r21` |
| The game's dynamic resolution | a change of the view size recreates the feature (a hitch of 15-100 ms); the view size never changed in any run, also not at 26 ms frames | feature creation lines in the logs |
| `[stereo] render_scale` / `dynamic_resolution` (the mod's own, view rects inside full-size targets) | upscale mode: DLSS reads the view's rectangle and writes the whole half, the runtime gets the whole half (section "Upscale mode"). Dynamic resolution: where NGX allows a range of input sizes the features are created once for the largest input of `render_scale` and evaluated with the current rectangle; under the NVIDIA App override (DLAA forced) it does not, and each change of the scale recreates both features (about 15 ms each on the RHI thread, a hitch per step) — see "Dynamic resolution" below. DLAA mode follows the view rectangle and recreates its features at every change | `captures/dlss/r21`, `r22` |
| Mono frames (menus, virtual screen) | nothing happens (the pass is only replaced while the engine renders in stereo) | - |

## What a player needs to try it

- An NVIDIA RTX GPU and a current driver (NGX ships with the driver).
- A mod DLL built with `-DFF7VR_DLSS=ON`: `tools\package\package.ps1 -Dlss` builds it into
  `build\release-dlss` and makes a package named `ff7vr-<date>-<commit>-dlss` (the default package
  is built explicitly without DLSS, and the script checks the DLL for NGX names either way).
- `[dlss] enabled = 1` in `ff7vr.ini`; `mode = upscale` and `input_scale = 0.5` (or 0.58, 0.67)
  are the defaults. Leave `[stereo] dynamic_resolution` off while the NVIDIA App override is set.
- `[foveation] enabled = 0` while DLSS is on (section "GPU faults seen in the test runs":
  with both on the GPU stopped within minutes in the stress runs).
- A DLSS model: if the NVIDIA App's override is set for the game, the driver's own copy is
  used and nothing else is needed; otherwise `nvngx_dlss.dll` (310.5.0 or later for presets
  L/M) next to the game's exe or in `[dlss] dll_dir`. The game folder may already have one
  from a flat-screen DLSS mod.
- Expect DLAA to cost about 8 ms per eye at 3072x3264 with preset L: too slow for 72 Hz on
  an RTX 5080. Upscale mode from 0.5 costs about what the game's own 100 % costs, with a
  picture close to it (section "The numbers").

A public release with DLSS would need: the NGX static library linked into the released DLL
(object code, allowed by the licence's grant 1.c), either NVIDIA's `nvngx_dlss.dll` shipped
next to it under NVIDIA's terms or a note that the player supplies one, the attribution and
NVIDIA marks of supplement 7.1(b), the DLL's third-party notices (guide 9.6), and a decision
by the project's maintainers on how an MIT-licensed project and NVIDIA's terms fit together
(below).

## Settings (`[dlss]` in `ff7vr.ini`)

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `0` | `1`: DLSS replaces the game's temporal anti-aliasing while the engine renders in stereo |
| `init` | `0` | `1`: initialise NGX and query DLSS at the first stereo frame even while `enabled = 0` (diagnostics) |
| `mode` | `upscale` | `upscale`: DLSS scales each eye up to its full size from the rendered part (see "Upscale mode"); `dlaa`: anti-aliasing at the rendered size, in place of the game's |
| `input_scale` | `0.5` | upscale mode: the rendered share of each eye's width and height. At start (with `enabled = 1` and `mode = upscale`) it sets `[stereo] render_scale`, which is also the upper bound of `[stereo] dynamic_resolution`; `0` leaves `render_scale` alone |
| `preset` | `default` | DLSS model: `default` (NVIDIA's choice for the mode), `j`, `k`, `l`, `m`. The NVIDIA App's override for the game wins |
| `auto_exposure` | `1` | DLSS computes the exposure itself |
| `mv_jitter` | `2` | how the camera motion vectors treat the jitter: `2` as the engine computes them, not flagged; `1` flagged as jittered; `0` the jitter difference removed |
| `camera_cut_reset` | `1` | reset an eye's history on a camera cut (see "Camera cuts") |
| `cut_distance`, `cut_angle` | `100`, `30` | a camera move of more than this many world units (cm) or degrees in one frame counts as a cut |
| `dll_dir` | empty | extra folder searched for `nvngx_dlss.dll` (searched before the mod's folder) |
| `log_ngx` | `1` | NGX's own messages in `ff7vr.log` (the first 400) |
| `log_verbose` | `0` | `1`: NGX's most detailed log level (read when NGX starts; the first 4000 messages). Shows its kernel allocations, feature creation and release, and its garbage collection |

## Dev commands

| Command | Effect |
|---|---|
| `dlss status` | NGX state, capability, feature library, the recognised pass, counters, features, GPU time per eye, jitter |
| `dlss on` / `dlss off` | switch while the game runs (the history is reset; off releases the features) |
| `dlss mode <dlaa\|upscale>` | switch the mode; with `upscale`, set the size with `cvar set r.ScreenPercentage <n>` |
| `dlss bench <out w> <out h> <in w> <in h>`, `dlss bench off` | one extra evaluation per frame of that size on blank textures, timed (cost of a mode without changing the engine) |
| `dlss init` | initialise NGX now (at the next stereo frame) |
| `dlss preset <default\|j\|k\|l\|m>` | change the model (features are recreated) |
| `dlss autoexp <0\|1>`, `dlss mvjitter <0\|1\|2>`, `dlss jitter <sx> <sy>` | tests |
| `dlss cutreset <0\|1>`, `dlss cutflag <0\|1>`, `dlss cutlimits <cm> <deg>` | the camera cut reset, the candidate flag of row 140, the limits |
| `dlss cuttest <path prefix> <reset 0\|1> <zero_mv 0\|1>` | at the next camera cut, with or without the reset and with the cut frame's motion vectors zeroed or not, write the left eye's upscaled image (a centred crop up to 2048x2048) of frames 0, 1, 2, 3, 5, 10 and 60 after the cut as raw `R10G10B10A2` files `<prefix>_f<n>_<w>x<h>.r10g10b10a2` |
| `dlss hdr <0\|1>`, `dlss sharpness <v>`, `dlss preexp <v>`, `dlss nograin <0\|1>` | upscale mode tests: the input flagged HDR, `InSharpness`, `InPreExposure`, the last pass at the reduced size without its noise texture |
| `dlss maxinput <full\|setting>` | upscale mode test: size a dynamic feature for render scale 1 instead of `[stereo] render_scale` |
| `dlss reset`, `dlss recreate` | reset the history, release and recreate the features |
| `dlss skip <0\|1>` | upscale mode fault test: everything runs (motion vectors, the graded copy at the reduced size) except the NGX evaluation; the game's own last pass scales the image up |
| `dlss stall [ms]` | fault test: at the next frame end, wait until the GPU has finished everything (what a blocking read-back such as `capture` does), then stay away `ms` milliseconds (default 40) with the GPU idle |
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
the mod under MIT is a question for the project's maintainers (4.e names "redistributable at no
charge" as an example of a licence condition it forbids for the SDK itself; the SDK parts
would have to be under NVIDIA's terms in such a release, not MIT). A release would also
need the attribution of 7.1(b) and the DLL's third-party notices.
