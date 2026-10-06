# DLSS in stereo (optional, `src/engine/src/dlss.cpp`)

NVIDIA DLSS in place of the game's temporal anti-aliasing, per eye, in the stereo rendering
of the engine module. Optional at build time and off by default at run time.

Status: two modes, upscale (DLSS scales each eye up to its full size from the rendered part,
`[dlss] input_scale`; the useful one) and DLAA (anti-aliasing at the rendered size; too slow
at headset resolution). The history is reset on camera cuts; upscale mode works with
`[stereo] render_scale` and dynamic resolution (under the NVIDIA App's DLSS override each
change of the size recreates the features, a hitch) and always hands the runtime the full
eye. Since 06/10 the engine renders at the input size and DLSS writes the runtime's size
(`[dlss] output = runtime`, section "Output at the runtime's size"), and the right eye's
shimmer under head motion is fixed (section "The right eye's shimmer"). The measurements here
were made headless (Null backend); the first headset sessions (06/10, Virtual Desktop) found
the right eye's shimmer and the video memory limit that these two changes address, and the
player has used the result since (06/10 evening, Virtual Desktop at 3264x3072 per eye,
`input_scale` 0.75: "working super super well"). Since then: a mip bias for the game's
textures while DLSS upscales (section "Texture mip bias") and what the dots he saw at that size
are (section "The dots at 3264x3072"). Not handled: mono frames. **GPU faults:**
the driver resets with DLSS on had one cause, found on 06/10 and fixed: the mod drew its
motion vectors inside a device context state of its own (`SwapDeviceContextState`) in the
middle of the engine's frame; at the title screen that hung the GPU within seconds, with or
without NGX. The draws now happen in the game's own pipeline state (section "GPU faults").

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

Steps 3-5 run in the game's own pipeline state: the motion vector draw saves the state it
changes (shaders, input layout, topology, the pixel shader's constant buffers 0-1 and
resources 0-1, render targets, viewports, rasterizer, blend and depth-stencil states) and
puts it back; NGX keeps the immediate context's state itself (programming guide 5.2.5);
copies need no state. A device context state of the mod's own (`CreateDeviceContextState`,
`SwapDeviceContextState`) is no longer used for any of this: a draw inside it in the middle
of the engine's frame hung the GPU (section "GPU faults"); `[dlss] context_state = own`
brings the old way back for comparison. Before every evaluation its inputs are checked
(section "Checks and diagnostics"); if anything is missing or a check fails (no captured
view rows, a rectangle outside a texture, a failed evaluation), the game's own draw runs
instead and `dlss status` says why.

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
**inferred, not measured** (measured later per eye at 0.58 with the override off: K 0.50
and M 0.82 of L's DLSS time, section "The NVIDIA App's DLSS override decides the model"): the frame time with the DLSS share scaled by NVIDIA's own cost
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
game's samplers (recreated with `MipLODBias`); the mod does that too since 06/10 evening
(section "Texture mip bias"). `render_scale` and
`r.ScreenPercentage` at the same input give the same texture sharpness
(`r22/sheet_rs_vs_sp.png`).

### The dots at 3264x3072 (06/10 evening)

The player, in the headset at Virtual Desktop's 3264x3072 per eye with `input_scale` 0.75,
preset default and foveated rendering `performance`: "a bit of that weird dot kind of effect
from DLSS", not at 4032x3648 with 0.76.

**Setup.** Null backend, eyes 3264x3072, `input_scale` 0.75 (the engine renders 2448x2304 per
eye), `output = runtime`, preset default (K: NGX picks Quality for 0.75), foveated rendering
`performance`, `r.BloomQuality 0`, the level-of-detail lines of the dev ini; the latest save,
which now starts in the street outside Seventh Heaven. Settings switched while the game runs,
three captures each; third person with a still camera (`captures/dlss/d2`; the first-person
camera sways with the idle animation, so first-person captures, `d1`, are only good for
looking). Frame times unpaced (`xr.null_pace = 0`, `t.MaxFPS 0`), 6 s windows; DLSS GPU time
per eye from timestamp queries.

**Source: foveated rendering's coarse shading, which DLSS keeps.** `performance` shades the
eye at full rate inside 0.45 of the ring radius, at 2x2 to 0.65 and at 4x4 beyond: 59 % of the
input's pixels at 4x4 (`foveation:` line of the log). A 4x4 block of the 2448-wide input is 5.3
pixels of the 3264-wide output, and the blocks sit on a fixed screen grid, the same in every
jittered frame, so DLSS sees them as stable detail and rebuilds them: a mosaic of blocks on
everything away from the view's centre, and on dithered hair the 2x2 and 4x4 blocks turn into
scattered bright specks. With foveated rendering off the same spots are clean
(`d1/sheet_fov_left.png`, people 26 to 41 % of the eye's width from its centre;
`d2/sheet_fov_npc.png`, a man's head near the left edge; `d2/sheet_fov_right.png`, people near
the right edge). The full-rate centre of the eye looks the same in all four foveation settings
(`d2/sheet_centre_fov.png`).

Why at 3264x3072 and not at 4032x3648: without DLSS (the game's anti-aliasing at 3264x3072,
`captures/dlss/d3`) `performance` leaves blocks of 4 output pixels, which the game's
anti-aliasing softens (`dcmp/sheet_npc.png`); this is the state the player could not tell from
foveated rendering off. With DLSS from 0.75 the same blocks are 5.3 output pixels and kept
sharp. At 4032x3648 with 0.76 (`d4`) the input is 3064 wide, so a block is again about the
angular size it has without DLSS (`d4/sheet_angular_npc.png`, both sizes cropped to the same
angle): smaller and less visible. That matches the player's report; whether it is all he saw
cannot be told headless.

A second, smaller source, in the full-rate centre: DLSS (preset K) from 0.75 draws gritty or
rusty surfaces with salt-and-pepper specks of one or two output pixels that the game's own
image at full size shows much softer (`dcmp/sheet_wall.png`, a rusted pillar;
`d4/sheet_angular_pillar.png`: finer at 4032x3648 / 0.76). Measured on four static, full-rate
regions (third person, three captures each, `dots.py`): mean luma gradient 10.6 to 17.2
(native) against 14.3 to 20.2 (DLSS, foveated rendering off), pixels more than 16 levels off
their 3x3 median 1.6 to 3.3 % against 3.2 to 6.3 % (about twice as many). Presets L and M
show fewer specks there (`dcmp/sheet_wall_presets.png`; gradient L 12.1 and M 12.9 against K
14.2 in the region of the pillar), at their cost. The specks are in DLSS's input: a frame dump
(`dlss frames`, `d5/dump_pillar_input.png` against `_output.png`) shows the pillar's dark pits
as single aliased pixels of the 2448-wide input, different in each jittered frame; the game's
anti-aliasing at full size averages them into a soft texture, K keeps them as specks. Not the
game's grain: DLSS's input drawn without the last pass's noise texture (`dlss nograin 1`, `d5`)
gives the same gradient and outliers (14.0 to 17.1 against 13.8 to 16.4; 2.8 to 5.1 % against
2.6 to 4.5 %).

| Foveated rendering with DLSS (0.75, K) | Frame ms avg (p95) | Against `performance` | Picture away from the centre (`d2/sheet_fov_*.png`) |
|---|---|---|---|
| `performance` (1x1 to 0.45, 2x2 to 0.65, 4x4 beyond) | 9.59 (11.46) | - | mosaic of blocks, specks in hair |
| `balanced` | 9.85 (11.47) | +0.26 ms | blocks mostly gone at the edge, specks in hair remain |
| `quality` | 10.12 (11.88) | +0.53 ms | no blocks seen; fine specks in dithered hair |
| off | 10.95 (12.55) | +1.36 ms | clean |

Other remedies measured in the same run (third person, still camera, foveated rendering
`performance`):

| Setting | Frame ms avg (p95) | DLSS ms per eye (L / R) | Picture (`d2/sheet_presets_*.png`, `d2/sheet_tb_*.png`) |
|---|---|---|---|
| preset default (K), the reference | 9.59 (11.46), 9.65 (11.23) with `k` named | 1.53 / 1.62 | - |
| `firefly = 1` (NGX's `Hint.UseFireflySwatter`, which the flat-screen Luma mod sets for this game) | 9.59 (11.10) | 1.53 / 1.62 | no visible change; the specks in hair stay |
| preset M | 13.94 (15.59) | 3.66 / 3.49 | the coarse blocks largely smoothed out, darker shadows (mean level of the shadowed ground 22 against 25) |
| preset L | 15.98 (17.73) | 4.63 / 4.47 | blocks remain, a little softer |
| texture bias `auto` (-0.41 at 0.75) | 13.36 (22.37): a disturbed window (p50 10.83); see `auto-1` | 1.67 / 3.36 (same window) | no visible change |
| texture bias `auto-1` (-1.41) | 9.74 (11.54); with foveated rendering off 11.06 against 10.95 | 1.53 / 1.62 | textures a little crisper where shaded at full rate; measured 10 to 20 % more fine specks, not visible in the crops (section "Texture mip bias") |

For comparison, the game's own anti-aliasing at the full 3264x3072 without DLSS (`d3`, same
spot): 9.14 ms with foveated rendering `performance`, 9.61 `balanced`, 10.18 `quality`, 11.76
off. So at this size and 0.75 DLSS costs about as much as rendering the full size (0.45 ms more
with `performance`, the same with `quality`, 0.8 ms less with foveated rendering off): it is a
picture choice here, and the cost of `quality` instead of `performance` is the same with or
without DLSS.

With the head turning (`d6`: the Null backend's emulated yaw sweep, third person, captures
taken during the motion, `d6/sheet_yaw_left.png`) the order is the same: blocks with
`performance`, fewer with `balanced`, a few at the outermost edge with `quality`, none with
foveated rendering off. Frame times in that run: `performance` 9.77 (`k` named 9.65),
`balanced` 9.94, `quality` 10.35, off 10.95 ms; preset M 13.76, L 15.94; texture bias `auto`
9.92, `auto-1` 9.88; no driver event.

The game's own dither and shadow settings, each switched in the first-person run (`d1`;
`r.SSS.Checkerboard 0`, `r.AmbientOcclusionLevels 0`, `r.DisableLODFade 1`,
`r.TemporalAASamples 16`, `r.Shadow.FilterMethod 1`, `r.Tonemapper.GrainQuantization 0`,
`foliage.DitheredLOD 0`, `r.StencilForLODDither 0`; all exist in this build): no visible
change to the blocks, which none of them touches (`d1/sheet_cvars_left.png`); the dots metric
(isolated bright pixels in shadow, `captures/dlss/dots.py`) could not separate them from the
camera's sway in that run. In a later run (`d5`, third person) `r.Tonemapper.GrainQuantization
0`, set while the game ran 10 s after the no-grain test, was followed 8 s later by a GPU hang
(System log `nvlddmkm` 153 at 20:50:12, the game's crash report `DXGI_ERROR_DEVICE_HUNG`); the
same switch in `d1` ran without a fault. Not repeated (a reproduction resets the driver for the
whole PC) and not explained; in UE 4 that variable selects a variant of the tonemapper's
shader (inferred for this build), and it is best not switched while the game runs.
`input_scale` 0.85 and 0.80 at 3264x3072 (`d1/sheet_nat_left.png`): smaller blocks (4x4 of a
2776-wide input is 4.7 output pixels), DLSS 1.70 ms per eye against 1.67 (K's cost hardly
follows the input here); the frame cost of rendering 28 % more pixels was not measured in a
clean window (that run was in a slow state).

### Texture mip bias (`[dlss] texture_bias`)

With DLSS the engine renders each eye at `input_scale` of the output and chooses texture mips
for that smaller size, so textures are softer than at the output size. The DLSS guide (3.5)
asks for a negative mip bias, `log2(render width / output width)`, and recommends 1 more
(`- 1.0 + epsilon`, "-2.0 for this Performance mode"), warning that a bias can bring flicker
or moiré on fine textures. `r.MipMapLODBias` did not reach the game's materials (`r14`): the
engine bakes it into each texture's sampler state when the texture is loaded (inferred from
UE 4's texture code; not traced here), and the materials sample through those states.

**How.** A hook on `PSSetSamplers` (immediate context slot 10, installed with the view buffer
hooks) replaces, on the RHI thread and only for the game's immediate context, each anisotropic
sampler state the game binds by a copy with the bias added to `MipLODBias`, while the frame
before was upscaled by DLSS in stereo. Copies are made once per sampler and bias
(`CreateSamplerState` with the original description) and kept in a map that holds a reference
to the original, so that its address cannot be reused while it is in the map; a change of the
bias (quantised to 1/32) clears the map. Point, bilinear and trilinear samplers
(post-processing, shadow maps, the UI) are left alone; `texture_bias_trilinear = 1` adds the
trilinear ones. The flat-screen Luma mod does the same at its sampler level (anisotropic
samplers, `log2(render / output) - 1`); no code was taken from it.

What the game binds (`dlss status`, "texture mip bias"; `d2`, about 75 s with a bias on, at
about 100 frames per second): 5.1 million `PSSetSamplers` calls, 4.2 million of them with a
sampler replaced (about 700 calls per stereo frame, a map look-up each); few
distinct sampler objects (D3D11 returns the same object for the same description): 6
anisotropic, 9 trilinear, 15 others, 3 comparison; 6 copies made, none failed.

**Measured** (3264x3072, 0.75, preset K, third person, still camera, `d2`; native = the game's
anti-aliasing at 3264x3072 without DLSS, `d3`; luma gradient and outliers as in "The dots at
3264x3072", full-rate regions):

| | Bias at 0.75 | Frame ms, foveation `performance` / off | Gradient, ground region A / B | Outliers > 16, A / B | Crops |
|---|---|---|---|---|---|
| native, no DLSS | - | 9.14 / 11.76 | 12.7 / 17.2 | 1.6 / 3.3 % | `dcmp/sheet_ground.png` |
| `off` | 0 | 9.59 / 10.95 | 15.6 / 20.2 | 3.2 / 6.3 % | - |
| `auto` | -0.41 | with the head turning (`d6`): 9.92 against 9.65 to 9.77 | A with foveation `performance`: 14.1, as `off` (14.1) | - | no visible change |
| `auto-1` | -1.41 | 9.74 / 11.06 | 16.3 / 20.6 | 3.8 / 6.8 % | `d2/sheet_tb_fovoff_*.png`: hardly visible; the ground a touch crisper |

So at this size DLSS's image from 0.75 already carries more fine detail (and more specks)
than the game's own image at full size, the bias moves it further that way, and the visible
gain is small. NVIDIA's warning about flicker and moiré was not tested under motion. The bias
is therefore off by default and left as an option (`auto` or `auto-1`) for players who find
textures soft; it costs 0.1 to 0.3 ms. Textures in the coarsely shaded part of foveated
rendering are limited by the shading rate, not by the mip (`d2/sheet_tb_sign.png`).

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

### Output at the runtime's size (`[dlss] output = runtime`, the default)

Until 06/10 the engine's eye target and every scene buffer had the runtime's eye size (the
XR swapchain's, `[xr] resolution_scale` included) and `input_scale` only shrank the views
inside them (`[stereo] render_scale`). A larger headset setting therefore cost the video
memory of a full-size render even when DLSS rendered 58 % of it; at Virtual Desktop's
4032x3648 per eye the card ran out of memory and stalled. With `output = runtime`:

1. **The engine renders at the input size.** The stereo device sizes its eye target (and
   with it `GSystemResolution`, so the scene buffers) at `input_scale` times the runtime's
   eye size, rounded to a multiple of 4 (`dlss::engine_eye_size`, called where the device
   takes the runtime's size). The views use the whole target; `[stereo] render_scale` keeps
   its meaning (a share of that target, 1 by default) and dynamic resolution still works
   inside it.
2. **The last pass draws 1:1.** The game's last pass (grading and grain) writes the eye's
   graded image into the eye target at the input size; it is copied into the texture DLSS
   reads (no second run of the pass, no patched constants; with `r.ScreenPercentage` below
   100 the pass scales and the earlier second run at the reduced size is used instead).
3. **DLSS writes the runtime's size** into a texture of its own, two eyes side by side
   (`R10G10B10A2_UNORM`, 2 x the runtime's width by its height), one feature per eye with
   the runtime's eye size as its output.
4. **That texture goes to the runtime.** At the end of the frame (`dlss::output_texture`,
   before the hand-over to the XR module and the desktop mirror) the image handed over is
   DLSS's texture with each eye's whole half as its rectangle, so the projection layer's
   sub-image is the full runtime size. An eye DLSS did not write in that frame (start-up,
   a failed check) is copied into its half at the engine's size and handed over with that
   rectangle; the runtime scales it up (as with a render scale below 1).
5. **Everything else follows the engine's target.** Foveated rendering finds the views at
   the input size and builds its shading-rate surface for that target; the UI is on its own
   layer and does not depend on the eye size; the desktop mirror shows DLSS's texture.

`output = engine` keeps the earlier way (the target at the runtime's size, the views at
`input_scale` of it, DLSS's result copied back into the target). If NGX cannot start, the
device goes back to the runtime's size. `dlss output runtime|engine [input_scale]` switches
while the game runs (the eye target is reallocated: a hitch).

**Measured** (06/10, Null backend, RTX 5080 with 16 GB, unpaced (`xr.null_pace = 0`,
`t.MaxFPS 0`), the first room of the test save, third person, still camera, foveated
rendering `performance`, preset default (K at Quality and Balanced), `r.BloomQuality 0`;
frame times are three 6 s windows of `stereo status`; video memory from the timing block's
line (the game's DXGI usage and budget; the whole card from DXGI as well);
`captures/dlss/p2-a-*`, `p2-b-rt4032`):

| Runtime eye (headset setting) | Path | Engine eye | Frame ms avg (p95) | DLSS GPU ms per eye | Game's video memory (budget) | Whole card |
|---|---|---|---|---|---|---|
| 3072x3264 | no DLSS (the standard path) | 3072x3264 | 8.35 (9.2-9.5) | - | 9.07 GB (15.2) | 11.0 GB |
| 4032x3648 | no DLSS | 4032x3648 | 11.0-11.2 (11.9-12.7) | - | 10.65 GB (13.7) | 12.6 GB |
| 4032x3648 | DLSS `output = engine`, 0.76 (the old way) | 4032x3648, views 3064x2776 | 12.5 (13.4-14.1) | 2.39 / 2.31 | 12.02 GB (13.5) | 13.9 GB |
| 4032x3648 | **DLSS `output = runtime`, 0.76** | **3064x2772** | **11.8-11.9 (12.4-12.7)** | 2.29 / 2.24 | **10.05 GB (15.2)**, DLSS's features 1.07 GB of it | 12.0 GB |
| 5376x4992 | DLSS `output = runtime`, 0.57 | 3064x2844 | 15.3 (16.5-17.0) | 3.89 / 3.77 | 11.21 GB (13.7) | 13.1 GB |

What it says:

- At Virtual Desktop's 4032x3648 the new path needs 2 GB less than the old one (10.05
  against 12.02 GB, the old one at 89 % of its budget with 1.4 GB left on the card) and less
  than the game at 4032x3648 without DLSS, and it is a little faster than the old path (11.9
  against 12.5 ms). Against the game at 3072x3264 without DLSS it costs 3.5 ms more per frame
  (11.9 against 8.35): DLSS writing two eyes of 4032x3648 is 4.5 ms. Headless that is inside
  72 Hz (13.9 ms) but not inside 90 Hz (11.1 ms).
- At 5376x4992 the engine still renders about 3064 wide, the memory stays at 11.2 GB, but
  DLSS writing two eyes of 27 megapixels costs 7.7 ms and the frame 15.3 ms: below 72 Hz
  headless.
- The slow phases of other runs (frames of 60 to 3000 ms for minutes after loading, the card
  at 60 to 90 W) appeared once in these runs, in the first `output = runtime` run at
  4032x3648 (`p2-a-rt4032`, the scene's own GPU time 58 ms median, with 3.7 GB of the card
  free); the same configuration started again 8 minutes later ran at 11.9 ms from the start
  (`p2-b-rt4032`). They are not explained here (`docs/benchmarking.md`).
- Captures of both eyes at 4032x3648 (`captures/dlss/p2-b-rt4032/cap_a_L.png`, `_R.png`,
  `sheet_LR.png`, `sheet_crop.png`): the whole eye, no seam between the halves, the
  parallax right, the HUD on its layer as before; foveated rendering found the views at the
  input size (`foveation: eye views: rect at +0x80 (3064x2772, right eye at x 3064)`, a
  surface of 384x175 tiles for 6128x2772); every frame went out from DLSS's texture (4385
  of 4385), no eye at the engine's size.
- **10 minutes with motion** at 4032x3648, `input_scale` 0.76, 72 Hz
  (`captures/dlss/soak-s1-p2-4032-runtime`: the head turning, walking, a capture every
  minute, frame dumps every 3 minutes): no driver event, no warning or error, 91,732
  evaluations, none failed, no failed check, every frame from DLSS's texture (45,865 of
  45,865), frames at the 72 Hz pace (13.89 ms; longest 296 to 312 ms in the windows with a
  frame dump), DLSS 2.31 / 2.24 ms per eye, the game's video memory 8.6 GB at the end
  (12.3 GB on the whole card). Right / left output change around the view axis in the three
  dumps: 1.03, 1.00, 1.02 (the motion vector fix holds in this mode too).

## The right eye's shimmer (06/10)

In the first headset sessions with DLSS (Virtual Desktop at 3264x3072 per eye, `input_scale`
0.75) the left eye looked right and the right eye "not temporally solid", with DLSS only.
Headless captures of a still camera had never shown it (the right eye was as steady as the
left: `captures/dlss/r21`, `r11`).

**How it was measured.** `dlss frames <prefix> <n> <crop>` writes, for n consecutive frames,
each eye's DLSS input (the graded image at the input size), its motion vectors and DLSS's
output, with the eye's rectangle, jitter, reset and all view constant rows. For consecutive
frames of one eye the previous frame is sampled where the motion vectors point and compared
with the current one (mean absolute difference, 0-1023 levels, over the whole eye and over a
square of 1400 input pixels around each eye's view axis, so that both eyes are compared on
the same part of the scene); `captures/dlss/mvcheck.py` does it. Null backend, eyes
3264x3072 at 72 Hz, `input_scale` 0.75 (the views at 0.75 of full-size buffers, as the
player had it), the emulated head turning (`xr.null_motion = yaw`), the street outside the
first room.

**What was ruled out** (`captures/dlss/p1-yaw`, `p1b`, `p1c`):

- The engine's data per eye are right: each eye's previous-frame matrices (rows 83-86) equal
  its own current matrices of the frame before (rows 0-3) exactly, the other eye's differ;
  each eye has its own jitter sequence with the previous frame's jitter in `zw`; the median
  motion vector matches the image's shift (phase correlation) in both eyes; no history reset
  fired in any dumped frame (also not in the player's log: 4 camera cut lines in 3 minutes).
- The inputs are equally steady: the motion-compensated change of the input is the same in
  both eyes (right/left 0.93 to 1.10).
- Not an interaction of the two evaluations: with the right eye the only one evaluated
  (`dlss eyes 2`) its output was as unsteady as with both (4.85 against 4.78).
- Not foveated rendering (the difference is there with it off) and not the view: with the
  views swapped between the halves (`stereo swap 1`) the right HALF stayed the unsteady one.

**Bisection** (`captures/dlss/p1d`, one run, `dlss copyinputs 1` + `dlss copymask`: each eye's
inputs copied to the origin of textures of its own, output to its own texture):

| DLSS reads from the eye's own textures at the origin | Output change L / R (around the axis) | R / L |
|---|---|---|
| nothing (shared double-wide textures, sub-rectangle bases) | 5.55 / 11.91 | 2.14 |
| the output only | 2.87 / 11.15 | 3.89 |
| depth (+ output) | 2.58 / 10.64 | 4.13 |
| colour (+ output) | 4.33 / 10.58 | 2.44 |
| **motion vectors (+ output)** | **2.59 / 2.69** | **1.04** |
| everything | 3.20 / 3.22 | 1.01 |

**Cause:** DLSS read the right eye's motion vectors wrongly from the shared double-wide
motion vector texture at the sub-rectangle base x = eye width (`InMVSubrectBase`); the left
eye, at base 0, was right. Colour and depth at the same base were read correctly. With a
still head every motion vector is 0, wherever DLSS reads them, which is why the still-camera
tests never showed it. Where exactly DLSS reads instead was not determined.

**Fix** (`[dlss] mv_textures = eye`, the default): each eye's motion vectors are drawn at the
origin of a texture of their own (the input's size; the shader loads the engine's depth and
velocity at the eye's own pixels) and DLSS reads them at base 0. Colour, depth and the output
stay on the shared textures with their bases. `mv_textures = shared` brings the old layout
back for comparison. The two textures together are smaller than the shared one was.

**Proof** (`captures/dlss/p1e`, one run of the fixed build in the player's configuration,
`output = engine`, switching the layout by command; output change, whole eye and around the
view axis, mean of 4 consecutive frame pairs):

| Motion vectors | Foveated rendering | Left | Right | Right / left (axis) |
|---|---|---|---|---|
| each eye's own (the fix) | `performance` | 2.76 (2.58) | 4.28 (3.30) | 1.55 (1.28) |
| shared, base x = eye width (as before) | `performance` | 2.35 (1.58) | 6.41 (6.84) | 2.73 (4.33) |
| each eye's own again | `performance` | 2.17 (2.03) | 2.83 (2.44) | 1.31 (1.20) |
| each eye's own | off | 1.31 (1.96) | 1.36 (1.98) | 1.03 (1.01) |

The remaining difference of up to 1.3 with foveated rendering on comes and goes between
frame sets (the inputs differ by up to 1.25 there too); with it off both eyes are equal.

**10 minutes with motion** (`captures/dlss/soak-s1-p1-3264-engine`: the player's
configuration, eyes 3264x3072 at 72 Hz, `input_scale` 0.75, `output = engine`, foveated
rendering `performance`, the head turning, walking forward and back, a capture every
minute and 5 frames dumped every 2 minutes): no driver event, no warning or error in the
log, 91,498 evaluations, none failed, no failed check, one history reset per eye (the
features' creation), frames at the 72 Hz pace (13.89 ms; the longest 218 to 230 ms in the
windows with a frame dump). Right / left around the view axis in the four dumps: 1.01, 0.94,
1.02, 1.02.

## GPU faults seen in the test runs

### Cause and fix (06/10)

**Fast reproduction.** The title screen in stereo with DLSS on: Null backend, eyes 3072x3264
at 120 Hz, `[dlss] enabled = 1`, `input_scale = 0.58`, `[foveation] enabled = 0`,
`[stereo] start_in_stereo = 1`, and no key pressed, so the game stays at "press any button"
(the first player session with DLSS hung there, 5 s after the features were created). Every
such run hung the GPU (System log `nvlddmkm` event 153, the game's device removed with
`DXGI_ERROR_DEVICE_HUNG`) between 0.05 and 125 s after the features were created, most
within 30 s. Earlier test runs never rendered the title in stereo (the harness switches
stereo off for the menus), and in gameplay the same fault took minutes to hours. The eye
image at the title is almost black (90 % of the pixels below one 10-bit step), the camera
is still, the motion vectors are zero.

**Bisection** (each line one run at the title, `captures/dlss` `b2`-`b16`, 180 or 300 s
unless it hung earlier):

| What ran | Result |
|---|---|
| DLSS as it was; DLAA mode at full size (inside NGX's input range); one eye only; a parameter map per feature; NGX in the game's state; each eye's inputs in textures of their own without sub-rectangles; evaluations at the end of the frame; a `Flush` after each evaluation; output not copied; jitter 0; auto exposure off; colour raised to at least 0.05; the frame rate capped at 45 (the card at 90 W instead of at its 360 W limit) | hang, every one (0.05 to 125 s) |
| NGX alone: two evaluations per frame of the same size on textures of the mod's own, at the frame end, with or without jitter, rewritten inputs or a copied output (about 80,000 evaluations at the power limit) | clean, 4 x 180 s |
| no DLSS, render scale 0.58; no DLSS at 4096x4352 per eye | clean, 300 s and 180 s |
| the DLSS path without the NGX evaluation (`test_skip`): motion vectors drawn and the colour passed through at the anti-aliasing pass inside the mod's own context state | **hang** (also at render scale 1, and with NGX never initialised) |
| the same, without the motion vector draw (pass-through copy only) | clean, 300 s |
| the mod's context state swapped in and out around the copy, no draw | clean, 300 s |
| the motion vector draw in the game's own state (saved and restored), no swap | clean, 300 s |

So the trigger was a draw (the motion vectors) made inside a device context state of the
mod's own, swapped in with `ID3D11DeviceContext1::SwapDeviceContextState` at the engine's
anti-aliasing pass and swapped out after it. The swap alone, and the same draw in the
game's own state, ran clean. NGX was not needed: with NGX never initialised the draw still
hung the GPU in 10 s. Every DLSS variant tried before still drew the motion vectors that
way, which is why none of them changed anything. Why the driver fails on this at the title
screen, and much more rarely in gameplay, is not known (the state object was created with
`CreateDeviceContextState` for `ID3D11Device1` at the device's feature level; the game's
device has no multithread protection; no other thread used the immediate context, checked
with hooks on `Map`, `Unmap` and `UpdateSubresource`).

**Fix** (`[dlss] context_state = game`, the default): the motion vector draw runs in the
game's own pipeline state and puts back what it changes; NGX creates and evaluates in the
game's state too. The mod's own context state is only used by test switches and by
`dlss bench`.

### Proof of the fix (06/10, headless)

Null backend, eyes 3072x3264 at 120 Hz, upscale mode at `input_scale` 0.58, preset L (the
NVIDIA App override), the dev ini's `[stereo_cvars]` (including the level-of-detail lines),
`r.BloomQuality 0`; after each run the System log was read for `nvlddmkm` events.

| Run | Length | Result |
|---|---|---|
| Title screen in stereo, upscale, three runs (`captures/dlss/b16-fix-a`, `-b`, `-c`) | 3 x 5 min | no fault; 63,442 evaluations in the first, none failed, no failed check, no warning or error in the log; frames 9.28 ms (p95 9.4), DLSS 3.04 ms per eye |
| Title screen in stereo, DLAA at 3072x3264 (`b16-fix-dlaa`) | 5 min | no fault |
| The same build with `context_state = own` (`b16-own`), run between them | - | GPU hang after 115 s |
| Gameplay, foveated rendering off (`e2-world-fovoff`): the latest save, walking forward and back, the emulated head turning, Insert (virtual screen) off and on every 2 minutes, a 150 degree head jump at 5 minutes | 20.1 min | no fault; 164,254 evaluations, none failed, no failed check, 22 history resets at cuts, no warning, error or NGX error in the log. Frames (30 s windows) after the level had streamed in: average 11.6 to 12.8 ms, 95th percentile 13.1 to 18.5 ms, longest 76 ms (at the Insert switches); the first 3 minutes after loading were slow (average 27 to 288 ms) while video memory was being moved (the System process's copy engine at 19 %) |

| Gameplay, foveated rendering on at `performance` (`e3b-world-fovperf`): as above, started at `input_scale` 1.0 so that foveated rendering finds the views, then the render scale switched between 0.5 and 0.58 every 60 s (each switch rebuilds the shading-rate surface and, under the NVIDIA App override, recreates both features): the combination that hung within 3 minutes twice in the runs of 06/10 night | 20.0 min | no fault; 19 scale changes, 40 feature creations, 20 shading-rate surfaces, every replaced feature released after its fence; 208,712 evaluations, none failed, no failed check, no warning, error or NGX error. Frames after the first minute (which ran at full size): average 9.7 to 12.0 ms, 95th percentile 10.7 to 13.7 ms, longest 68 ms |

The runs above were made while the NVIDIA App's override still forced DLAA and preset L.
From 13:51 the override was off (the application's choice: Balanced at 0.58 with preset K,
a dynamic feature), and the same tests were repeated with the range rule of "Modes and
ranges" (commit `8f47bed`):

| Run (override off) | Length | Result |
|---|---|---|
| Title screen in stereo (`n-n1-fix`) | 5 min | no fault; 69,644 evaluations, none failed, no warning or error; frames 8.33 ms (the 120 Hz pace), DLSS 1.49 ms per eye |
| The same with `context_state = own` (`n-n2-own`) | - | GPU hang after 27 s |
| Gameplay with a capture every 20 s (`f-captures`) | 6 min | no fault; 14 captures, all written; no warning or error; DLSS 1.61 ms per eye; frames 9.1 to 9.3 ms |
| Gameplay, foveated rendering off, as `e2-world-fovoff` (`e2n-world-fovoff`) | 20.1 min | no fault; 247,107 evaluations, none failed, no failed check, 23 history resets, no warning, error or NGX error; 2 feature creations; frames after the first minute average 8.8 to 9.4 ms, 95th percentile 9.8 to 10.8 ms, longest 148 ms (an Insert switch); DLSS 1.60 ms per eye |
| Gameplay, foveated rendering on at `performance`, render scale 0.5 / 0.58 every 60 s, as `e3b-world-fovperf` (`e3n-world-fovperf`); each change switches between Performance (M) and Balanced (K) and recreates both features | 20.1 min | no fault; 20 scale changes, 52 feature creations (all replaced ones released after their fence), 19 shading-rate surfaces, 177,414 evaluations, none failed, no failed check, no warning, error or NGX error; DLSS 1.94 ms per eye. Frames: steady 8.5 to 9.2 ms (95th percentile 8.8 to 10.3) from minute 9 on; slow phases (average 23 to 37 ms) in the first 3 minutes and in minutes 5 to 8, as also seen without DLSS (below) |

Slow phases: in several runs, with and without DLSS (a 5-minute run without DLSS at render
scale 0.58 on the same route averaged 32 to 47 ms throughout), the frame time rose to 20 to
50 ms for minutes while the System process's copy engine was busy (video memory being
moved), then fell back. They are not caused by DLSS; their cause was not investigated (the
level-of-detail `[stereo_cvars]` added to the defaults on 06/10 are one candidate).

Not covered: a loading screen between areas, a cutscene, a headset. Foveated rendering
still does not start when the render scale is below 1 at start (so with `input_scale`
below 1 from the ini it stays off, as before; a known problem of foveated rendering, not of
DLSS).

### History before the cause was found

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
| r37 | **on** (late), DLSS on with `dlss skip 1` (motion vectors, pass-through, the graded copy and the state swaps run; no NGX evaluation) | 77 captures, 18 scale changes | 10 min | clean: the NGX evaluation is the part of DLSS the hang needs |
| r38 | **on** (late), DLSS on | 79 captures, no scale change, no recreation (steady) | 10 min | clean |
| r39 | **on** (late), DLSS on | 78 captures, `dlss recreate` every 10 s (51), no scale change | 10 min | clean: feature recreation alone does not do it |
| r35 (soak) | off (`[foveation] enabled = 0`, ini start as a player would have it) | capture every 15 s (64), scale change every 60 s (19, each recreating both features) | 20 min | clean: no driver event, no warning, error or NGX error in the log, no failed evaluation; frame times per 30 s window 10.3 to 12.6 ms average, 95th percentile 11.1 to 13.6 ms, no drift |

In both hangs the RHI thread was blocked inside the NVIDIA driver in the engine's own
`Flush` after its per-frame query (`docs/re/engine.md` section 13), with none of the mod's
code on the blocked call chain; the GPU had stopped finishing work without being reset.
With foveated rendering off, the same DLSS load ran clean for 40 minutes in all (r31, r34
and the 20-minute soak r35); foveated rendering with the same load but without DLSS ran
clean for 10 minutes (r36), and so did everything DLSS does except the NGX evaluation
itself (r37). With both on, a steady size (r38) and feature recreations at a steady size
(r39) ran clean for 10 minutes each; both failing runs changed `[stereo] render_scale`
(between 0.5 and 0.58, every 30 or 60 s), which makes foveated rendering rebuild its
shading-rate surface for a new layout and recreates the features. The hangs did not come
right at a change (22 s and about 50 s after the last one), so the trigger is not pinned
down further; with two failures, a single clean 10-minute run is evidence (at the failing
runs' rate a clean 10 minutes would have a chance of about 4 %), not proof.

Later the same morning the picture changed. During the release check of the `-dlss`
package (06/10 05:16:16, `input_scale` 0.58, `[foveation] enabled = 0`, dynamic resolution
off, a steady size, no captures, no recreations) the GPU hung about two minutes after DLSS
started (`nvlddmkm` 153, `DXGI_ERROR_DEVICE_HUNG`; `captures/release-check/morning/dlss-fault`).
Three minutes after that reset, a run of the standard package without DLSS faulted too
(05:19:00, 14 s into walking; `captures/soak/morning-attempt1`), and a 20-minute soak of the
same standard package then ran clean. So "foveated rendering off is enough" no longer
holds, and the standard package is not clear of suspicion either: the PC's System log holds
about 35 such events since April, before this project, and MSI Afterburner applies a
custom voltage curve and a memory offset at start-up. A control run without the mod on the
same route, and a run at the card's stock settings, are the next tests; nothing on the
PC was changed.

### Conclusion before 06/10 (superseded by "Cause and fix" above)

The faults are not caused by DLSS alone: the same kind appeared without DLSS (once on
05/10 and once on 06/10), and with DLSS they came both with and without foveated
rendering, so the paragraph below describes the state of knowledge before the 05:16
hang; its advice (foveated rendering off) is not sufficient. (The two faults without
DLSS are not explained by the cause found later; the PC's System log has about 25 such
events from April to September, before this project.) Foveated rendering uses
NVIDIA's variable rate shading through NVAPI (`src/render/src/foveation.cpp`). It is not
simply "variable rate shading plus DLSS": the first DLSS runs (about 100 captures with
DLSS and foveated rendering both on, `r.ScreenPercentage` changes, few feature
recreations) had no fault, and neither did r38 and r39. What the failing runs had in
addition is changes of `[stereo] render_scale` (view rectangles inside full-size targets,
for which foveated rendering rebuilds its shading-rate surface) while NGX evaluated. Where
inside the driver, NGX or foveated rendering the GPU then waits is not known. Until it
is, run DLSS with `[foveation] enabled = 0`; started from the ini with an `input_scale`
below 1, foveated rendering is off anyway (see above; since fixed: it now starts at
any scale, `docs/render.md`, the view layout search). The deferred release ("Lifetimes")
is right by the SDK's rules but did not prevent the hangs, so it is not the explanation.

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

**With the override set back to the application's choice** (test machine, from 06/10 13:51):
NGX loads the model from the game folder's `nvngx_dlss.dll` (310.6.0), reports real ranges
for a 3072x3264 output (Quality 2048x2176, Balanced 1782x1893 and Performance 1536x1632, all
three accepting inputs down to 1536x1632 and Balanced up to the full size; Ultra Performance
1024x1088; DLAA 3041x3231 to 3072x3264) and follows the mode's default preset when
`[dlss] preset = default`: K for DLAA, Quality and Balanced, M for Performance, L for Ultra
Performance (NGX log: "App hint Preset Unspecified or Overridden, using title default
Preset K"). The features are then created dynamic (at `input_scale` 0.58: 1786x1898, any
input from 1536x1632 up to that), so `[stereo] dynamic_resolution` no longer recreates them.
The first feature creation with that model took 12.6 s on the RHI thread once (the game
stood still); later starts took 60 to 90 ms. DLSS cost at 0.58 (Balanced, K): 1.61 ms per
eye against 3.2 ms with the overridden L (`captures/dlss/f-captures`).

**Presets measured** with the override off (`captures/dlss/presets`: the first room, still
camera, eyes 3072x3264, Balanced at `input_scale` 0.58 = 1784x1896 per eye, unpaced; NGX's
log confirmed each request, "Using App hint Preset K"):

| Preset | DLSS GPU time per eye (motion vectors + DLSS + copy) | Against L |
|---|---|---|
| K | 1.57 ms (two measurements: 1.59, 1.57) | 0.50 |
| M | 2.56 ms | 0.82 |
| L | 3.13 ms | 1 |

So K saves about 3.1 ms per frame against L at this size (both eyes), M about 1.1 ms; the
ratios are close to NVIDIA's table for the RTX 5080 (K 0.58, M 0.78 of L at 4K
Performance). The frame times of that run are not comparable between presets (the level was
still streaming in during the first windows). Which preset looks better in the headset has
not been judged; `[dlss] preset = default` (K at Balanced and Quality, M at Performance) is
left as it was.

**Modes and ranges.** A feature is created only for a mode whose reported range holds its
creation size: the mode of the input's share of the output first (Quality from 0.66,
Balanced from 0.57, Performance from 0.49, Ultra Performance below), then the others. If no
mode accepts the input, there is no evaluation, the game's own pass runs and the log says so
(a warning at the first time and every power of two); under the override that is every input
below about 99 % of the output, so upscaling then needs the override's Super Resolution
setting back at the application's choice.

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
| Foveated rendering (variable rate shading) | works; it shades the scene before the anti-aliasing pass, DLSS resolves the periphery like the game's pass does. No GPU fault together with DLSS since the fix (20 minutes with render scale changes, section "Proof of the fix"). It also starts with `input_scale` below 1 from the ini (10 minutes at 0.58 with `performance`: foveation active, no failed evaluation, no driver event; `docs/render.md`, the view layout search) | `fov off` / `fov on` with DLSS on, `r3/sheet_fov.png`; `captures/dlss/e3b-world-fovperf` |
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
  are the defaults. With `output = runtime` (the default) `input_scale` is a share of the
  headset's resolution: to keep the cost of rendering about 3072 wide per eye and get a
  larger, sharper eye image, raise the headset's resolution and lower `input_scale` to match
  (Virtual Desktop's 4032x3648 with 0.76; section "Output at the runtime's size").
- The NVIDIA App's DLSS override for this game must not force the Super Resolution mode:
  under a forced DLAA no mode accepts a smaller input and upscaling does not run (section
  "The NVIDIA App's DLSS override decides the model"). A model preset override alone is fine.
- A build from commit `ef2688a` or later: before it, DLSS could hang the GPU (within seconds
  at the title screen; section "GPU faults seen in the test runs"). Foveated rendering may
  stay on (tested together with DLSS and render scale changes, section "Proof of the fix"),
  but it does not start when `input_scale` (the render scale) is below 1 at start, so with
  DLSS from the ini it is off for now.
- A DLSS model: if the NVIDIA App's override is set for the game, the driver's own copy is
  used (whether that works with no `nvngx_dlss.dll` anywhere was not tested); otherwise
  `nvngx_dlss.dll` (310.5.0 or later for presets L/M) next to the game's exe or in
  `[dlss] dll_dir`. The game folder may already have one from a flat-screen DLSS mod. The
  mod passes NGX `[dlss] dll_dir` and its own folder (beside the exe in a drop-in); the SDK's
  copy in the build tree is not used at run time and no package contains it. Without any
  model NGX reports DLSS as unavailable and the engine renders at the headset's full size
  with the game's own anti-aliasing (by the code; not tested on a PC without a model).
- With foveated rendering at `performance`, DLSS makes its coarse edges visible as blocky
  dots at input sizes around 2400 pixels wide; `quality` avoids that (section "The dots at
  3264x3072").
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
| `input_scale` | `0.5` | upscale mode: the rendered share of each eye's width and height. With `output = runtime` the engine's eye target is this share of the runtime's eye size; with `output = engine` it sets `[stereo] render_scale` at start (also the upper bound of `[stereo] dynamic_resolution`), and `0` leaves `render_scale` alone |
| `output` | `runtime` | upscale mode: `runtime`: the engine renders at `input_scale` of the runtime's eye size and DLSS writes the runtime's size into a texture of its own that goes to the runtime (section "Output at the runtime's size"); `engine`: the engine's buffers at the runtime's size, the views at `input_scale` of them, DLSS's result copied back (the way before 06/10) |
| `mv_textures` | `eye` | `eye`: each eye's motion vectors at the origin of a texture of its own; `shared`: one double-wide texture read at the right eye's sub-rectangle base, which made the right eye shimmer (section "The right eye's shimmer"; only to compare) |
| `preset` | `default` | DLSS model: `default` (NVIDIA's choice for the mode), `j`, `k`, `l`, `m`. The NVIDIA App's override for the game wins |
| `firefly` | `0` | `1`: NGX's `Hint.UseFireflySwatter` at feature creation (features are recreated when it changes). No visible change and no cost measured in this game (section "The dots at 3264x3072") |
| `texture_bias` | `off` | upscale mode: a mip bias added to the game's anisotropic sampler states while DLSS upscales: `off`, `auto` (`log2` of the input's share of the output), `auto-1` (one more, NVIDIA's recommendation), `auto+<x>`/`auto-<x>`, or a fixed negative number (section "Texture mip bias") |
| `texture_bias_trilinear` | `0` | `1`: also the game's trilinear samplers |
| `auto_exposure` | `1` | DLSS computes the exposure itself |
| `mv_jitter` | `2` | how the camera motion vectors treat the jitter: `2` as the engine computes them, not flagged; `1` flagged as jittered; `0` the jitter difference removed |
| `camera_cut_reset` | `1` | reset an eye's history on a camera cut (see "Camera cuts") |
| `cut_distance`, `cut_angle` | `100`, `30` | a camera move of more than this many world units (cm) or degrees in one frame counts as a cut |
| `dll_dir` | empty | extra folder searched for `nvngx_dlss.dll` (searched before the mod's folder) |
| `log_ngx` | `1` | NGX's own messages in `ff7vr.log` (the first 400, or `log_lines`) |
| `log_verbose` | `0` | `1`: NGX's most detailed log level (read when NGX starts; the first 20000 messages). Shows its kernel allocations, feature creation and release, and its garbage collection |
| `log_lines` | `0` | how many NGX messages go to the log; `0`: 400, or 20000 with `log_verbose` |
| `context_state` | `game` | `game`: the motion vectors are drawn and NGX runs in the game's own pipeline state (the fix of the GPU hangs); `own`: inside a device context state of the mod's own, the way that hung the GPU (only to compare) |
| `eval_at` | `pass` | upscale mode: `pass` evaluates at each eye's last pass; `frame_end` draws the game's last pass first and evaluates both eyes at the end of the frame (tested while looking for the hangs; no advantage found) |
| `validate` | `1` | check every evaluation's inputs first and run the game's pass if a check fails (section "Checks and diagnostics") |
| `input_stats` | `0` | `1`: GPU statistics of every evaluation's inputs, read back late (diagnostics; only together with `context_state = own`, inside whose state its compute pass runs) |
| `params` | `shared` | `shared`: one NGX parameter map for every call; `feature`: a map of its own per feature (`NVSDK_NGX_D3D11_AllocateParameters`) |

The fault-isolation keys (`test_eyes`, `test_zero_mv`, `test_mv_sanitize`, `test_flush`,
`test_skip`, `test_no_ngx`, `test_copy_inputs`, `test_color_floor`, `test_color_noise`,
`test_depth_const`, `test_no_copyout`, `test_jitter`, `test_bench`, `test_bench_jitter`,
`test_bench_write`, `test_bench_copyout`) set the tests of "Dev commands" from the first frame;
`dlss.cpp` describes each at its setting. They are for finding faults, not for play.

## Checks and diagnostics

- **Checks before every evaluation** (`[dlss] validate`, on by default): the feature exists
  and accepts the input size; the colour, depth and motion vector rectangles lie inside
  their textures and the formats are the expected ones; the output rectangle lies inside a
  texture with unordered access; the jitter is finite and at most a pixel; in upscale mode
  the eye's anti-aliasing pass ran this frame. A failed check skips the evaluation (the
  game's pass runs), is logged the first time and at every power of two, and is counted in
  `dlss status`. The engine's depth texture is compared with the previous frame's and a
  change is counted.
- **Event ring**: the last 400 DLSS events (the passes seen with their textures and
  rectangles, every evaluation with its parameters and result, feature creation and release,
  video memory every 2 s, failed checks, statistics) are written to `ff7vr.log` when the
  device is removed (`dlss: the D3D11 device was removed`) and by `dlss events`, which
  works also while the RHI thread is blocked. The GPU runs a few frames behind the RHI
  thread, so the evaluation a fault came from is among the last ones written.
- **Input statistics** (`[dlss] input_stats = 1`, only with `context_state = own`, the way
  used while the faults were hunted): a compute pass over each evaluation's
  input rectangle counts motion vectors that are not finite or larger than the input,
  depth that is not finite, outside 0..1, 0 or at least 0.999, black pixels of the
  display-referred colour (upscale) or non-finite HDR colour (DLAA), and the largest
  values; read back two frames later, logged when something is off. At camera cuts the
  motion vectors overflow FP16 (the engine keeps its previous camera across the mod's
  switches); those frames reset the history.
- **Other threads on the immediate context**: calls of `Map`, `Unmap` and
  `UpdateSubresource` on the game's immediate context from any thread but the RHI thread are
  logged and counted, separately if they came during an NGX call. None in the headless runs.
  The player's six sessions of 06/10 with Virtual Desktop logged such calls in every
  session: `UpdateSubresource` from the engine's render thread, and `Map`/`Unmap` from an
  unnamed thread, some of them during NGX calls. That check did not yet compare the device,
  so they may have been another D3D11 device's immediate context in the process (an XR
  runtime's); since then only the game's device counts. Not resolved.
- Video memory (local budget and use, DLSS's own share from `NGX_DLSS_GET_STATS`) is in every
  feature creation line and in the event ring.

## Dev commands

| Command | Effect |
|---|---|
| `dlss status` | NGX state, capability, feature library, the recognised pass, counters, features, GPU time per eye, jitter, the output mode with the runtime's and the engine's eye sizes, the latest video memory reading, the motion vector textures, and each eye's history resets by cause (a new feature, not evaluated in the previous frame, a camera cut, a request; more than 10 resets of an eye in 100 frames is also logged as a warning) |
| `dlss on` / `dlss off` | switch while the game runs (the history is reset; off releases the features) |
| `dlss mode <dlaa\|upscale>` | switch the mode; with `upscale`, set the size with `cvar set r.ScreenPercentage <n>` |
| `dlss bench <out w> <out h> <in w> <in h>`, `dlss bench off` | one extra evaluation per frame of that size on blank textures, timed (cost of a mode without changing the engine) |
| `dlss init` | initialise NGX now (at the next stereo frame) |
| `dlss preset <default\|j\|k\|l\|m>` | change the model (features are recreated) |
| `dlss autoexp <0\|1>`, `dlss mvjitter <0\|1\|2>`, `dlss jitter <sx> <sy>` | tests |
| `dlss cutreset <0\|1>`, `dlss cutflag <0\|1>`, `dlss cutlimits <cm> <deg>` | the camera cut reset, the candidate flag of row 140, the limits |
| `dlss cuttest <path prefix> <reset 0\|1> <zero_mv 0\|1>` | at the next camera cut, with or without the reset and with the cut frame's motion vectors zeroed or not, write the left eye's upscaled image (a centred crop up to 2048x2048) of frames 0, 1, 2, 3, 5, 10 and 60 after the cut as raw `R10G10B10A2` files `<prefix>_f<n>_<w>x<h>.r10g10b10a2` |
| `dlss hdr <0\|1>`, `dlss sharpness <v>`, `dlss preexp <v>`, `dlss nograin <0\|1>` | upscale mode tests: the input flagged HDR, `InSharpness`, `InPreExposure`, DLSS's input drawn by the last pass without its noise texture |
| `dlss firefly <0\|1>` | `[dlss] firefly` while the game runs (features are recreated) |
| `dlss texbias <off\|auto\|auto-1\|value> [trilinear 0\|1]` | `[dlss] texture_bias` (and `texture_bias_trilinear`) while the game runs; `dlss status` shows the bias in use, the input's share, the binds and replacements and the sampler kinds seen |
| `dlss maxinput <full\|setting>` | upscale mode test: size a dynamic feature for render scale 1 instead of `[stereo] render_scale` |
| `dlss reset`, `dlss recreate` | reset the history, release and recreate the features |
| `dlss skip <0\|1>` | upscale mode fault test: everything runs (motion vectors, the graded copy at the reduced size) except the NGX evaluation; the game's own last pass scales the image up. `[dlss] test_skip` from the ini also has levels 2 (no graded copy either), 3 (pass-through copy only), 4 (nothing replaced), 5 (the mod's context state swapped in and out, no draw), 6 (motion vectors drawn in the game's state) |
| `dlss events` | write the event ring to the log (section "Checks and diagnostics") |
| `dlss eyes <0\|1\|2>` | both eyes, left only, right only (the other eye runs the game's passes) |
| `dlss ownstate <0\|1>`, `dlss evalend <0\|1>`, `dlss params <shared\|feature>` | `[dlss] context_state`, `eval_at`, `params` while the game runs |
| `dlss zeromv`, `mvsanitize`, `flush`, `validate`, `stats`, `copyinputs` `<0\|1>` | tests: motion vectors zero or sanitised, `Flush` after each evaluation, the checks, the input statistics, per-eye input copies |
| `dlss stall [ms]` | fault test: at the next frame end, wait until the GPU has finished everything (what a blocking read-back such as `capture` does), then stay away `ms` milliseconds (default 40) with the GPU idle |
| `dlss dump [n]` | log the view uniform buffer rows 0-145 of the next n views (default 2) |
| `dlss frames <prefix> [n] [crop]` | for n consecutive frames (default 4) write each eye's DLSS input (graded colour), motion vectors and output as raw files (centre crops of `crop` input pixels, default 1024; 4096 = the whole eye), with the rectangles, jitter, reset and view constant rows in `<prefix>_meta.txt` (section "The right eye's shimmer") |
| `dlss output <runtime\|engine> [input_scale]` | `[dlss] output` while the game runs (the eye target is reallocated) |
| `dlss mvtextures <eye\|shared>` | `[dlss] mv_textures` while the game runs |
| `dlss copyinputs <0\|1>`, `dlss copymask <0-7>` | test: each eye's inputs copied to the origin of textures of its own and its output to its own texture; the mask chooses which copies DLSS reads (1 colour, 2 depth, 4 motion vectors) |
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
