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
| the game window's mode | `r.SetRes` | the first time stereo becomes active in exclusive or windowed fullscreen: switched to a normal window (`[stereo] vr_window`), back when stereo is switched off (see "Window modes") |
| light sort-key immediate | one byte in `FDeferredShadingSceneRenderer::RenderLights` | only while stereo renders, with `[stereo] light_fix = 1` (default): the skin lighting fix (below); the game's byte is put back in mono |
| Square Enix's bloom reduce pass (`Process`) | inline hook, render thread | `[stereo] bloom_fix` (default on): for the first level of a view that does not start at the origin, an RHI command arms the right-eye bloom fix (below) |
| Square Enix's distortion composite (`0x220e5b0`) | inline hook, render thread | always installed; runs only while heat haze or refraction renders: appends an RHI command with the view's rectangle that arms the right-eye distortion fix (below) on the composite's draw |
| D3D11 immediate context draw/dispatch/clear/copy functions | inline hooks, RHI thread | installed at the first stereo frame when the bloom fix or the ambient occlusion fix is on (they act on single DrawIndexed calls), or by the first GPU trace; otherwise not installed |
| `FRenderTargetPool::FindFreeElement` | inline hook, render thread | only after `gpu names on` (GPU trace labels) |
| the object array and name pool | read on the game thread | only with `[stereo] movie_screen = 1`: movie detection (below) |
| the controlled pawn, its location and the view target | reflected functions called through `ProcessEvent`, game thread, every frame | camera modes: level boom and first person (see "Camera modes") |
| the pawn's skeletal mesh components and the mesh components attached to them | `SetVisibility` through `ProcessEvent` | only while first person applies; put back afterwards |
| the battle signal | a reflected function called through `ProcessEvent` every frame (`[first_person] battle_signal`) | automatic third person in battles |
| XInput state | the game's import slot for `XInputGetState` (pointed at a wrapper in the loader) and a filter in the wrapper | the wrapper always (`[controls] pad_after_hooks`); the filter only with stereo enabled: the View/Back combinations and the chord of "Player controls" trigger their action and are removed from the state; View alone reaches the game as a short press on release |
| keyboard state | `GetAsyncKeyState`, game thread, once per frame, only while the game window has the focus | the keys of "Player controls" |

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
  is the XR host's (`recenter` command of the render module), triggered by the player with
  End or View/Back + left stick click (see "Player controls").

## Camera modes

`src/engine/src/player.cpp`. Every frame, at the start of `UGameEngine::Tick`, the module
finds the local player controller (`GEngine->GameInstance->LocalPlayers[0]->PlayerController`),
the pawn it controls (`Controller.K2_GetPawn`) and the view target
(`Controller.GetViewTarget`, which for a player controller is its camera manager's view
target), all through reflected functions called with `ProcessEvent` (`docs/re/engine.md`,
section 11). When the engine asks for the eye cameras it reads the pawn's location
(`Actor.K2_GetActorLocation`, after the world has ticked) and replaces the game camera's
location by the mode's eye base:

| Mode | Eye base | Applies when |
|---|---|---|
| third person, level boom (`[camera] boom = level`, default) | where the game camera would be at zero pitch around a pivot `pivot_height` above the pawn's location: the camera's offset from the pivot taken in the camera's frame and put back with its yaw only (`math::level_boom`) | decoupled pitch on, follow camera |
| third person, game boom (`boom = game`) | the game camera's location (the behaviour before the level boom) | always |
| first person | the point between the character's eye bones (or its head bone) plus `head_offset` (forward, right, up in cm, turned by the game camera's yaw); if they cannot be read, the pawn's location plus `eye_offset`. The view is level and faces the game camera's yaw | first person wanted, stereo on, follow camera, no battle |
| game camera | the game camera unchanged (decoupled pitch still levels the view) | anything else |

**Follow camera** means: the view target is the controlled pawn or the game's own camera
actor (the actor named `EndCameraActor`, of the engine's class `CameraActor` exactly, the view
target in normal play; a cutscene's `CineCameraActor` does not count), and the camera looks
at the pivot above the pawn (the pivot is in front of the camera, within `follow_distance`,
and no more than `aim_tolerance` from its line of sight; the follow camera misses it by
30 to 50 cm in the street, depending on the pitch). The test has a hysteresis: the camera must look at the pivot for
0.25 s before the modes apply again, and away from it for `[camera] miss_seconds` (1.5 s) before the game camera takes
over, so a short scripted move or a second of a battle shot does not flip the mode. A camera that looks elsewhere (an authored shot of a conversation or a
cutscene, a scripted pan) or another view target makes the module use the game's camera as
it is, in both modes. Whether every authored camera fails the test has not been checked:
no conversation or cutscene has been reached yet.

Why the level boom: the game's follow camera swings on a boom around the character. With
decoupled pitch the view stays level, so in the headset the only effect of pitch input was
that the player was lifted up to 2.4 m (camera pitched down) or lowered to counter height
(pitched up), sometimes behind objects. With the level boom the player stays at the pivot's
height behind the character at the boom's current length (shortened by the game's camera
collision as before) while the right stick or mouse orbits; pitch input only changes where
the game camera points, which decoupled pitch drops, so looking up and down is done with the
head. Measured: see "Camera modes: evidence" below.

### Battle camera

In a battle the game's camera stays `EndCameraActor` but frames the enemies as well as the
character, so it often fails the aim test; in play the mode flipped between the level boom
and the game camera several times per battle, and each flip moved the eyes by up to a couple
of metres in one frame. Two things change that:

- `[camera] combat = level` (default): while the battle signal (or `fp combat 1`) says a
  battle is in progress, the aim test is waived as long as the view target is still the
  game's camera actor or the pawn and the pivot is within `follow_distance`; the eyes stay on
  the level boom around the character. A cutscene camera in a battle (a `CineCameraActor`)
  fails the view-target test and still gets the game camera. `combat = game` keeps the aim
  test in battles. The status shows `in_battle` and `hold` (1 = the aim test failed and was
  waived this frame); the mode line reads `third person (level boom)` with the reason
  `battle, held through the battle camera`.
- Every change between the level boom, the game boom, first person and the game camera,
  while the view target stays the same game camera actor, moves the eyes over
  `[camera] blend_seconds` (0.35 s, smoothstep): the offset between where the eyes were and
  the new mode's position decays to zero while the new position keeps following the
  character (`move_between_modes`). A change of view target, or a switch to or from an
  authored camera, stays a cut, as the game cuts there. The log has one line per change,
  `player: camera move <from> -> <to> over <s> s (<cm> cm)` or `player: camera cut ...`;
  the status shows `cam_kind`, `cam_t`, `cam_off` (cm still to go), `blends`, `cuts` and
  `flips` (changes of the follow-camera test).

Dev commands: `fp combatcam level|game`, `fp camblend <s>`, `fp miss <s>`, and
`fp aim <cm>` (the aim tolerance; `fp aim 1` makes every frame miss, which is how the
transitions were tested without a battle).

### Camera collision

The game's own camera collision shortens its boom along the pitched direction only, so the
level boom's position (zero pitch) can be inside or right behind something the pitched
camera passed over. With `[camera] collision = 1` (default) the level boom is traced each
frame in third person: one `KismetSystemLibrary.LineTraceSingle` from the pivot to the eye
position plus `collision_margin` (20 cm), called through `ProcessEvent` on the library's
class default object with the pawn as world context and `bIgnoreSelf` (the pawn is
ignored), on the visibility trace channel (`TraceTypeQuery1`; see below). A hit puts the eyes on the line
`collision_margin` short of it; the boom shortens at once and lengthens back over about
0.3 s (an exponential with a 0.1 s time constant), as the game's camera does. Not traced in
full first person or with `boom = game` (the game's camera is collided by the game).

The function's parameter offsets and `FHitResult::Time` are read from reflection at start
(`docs/re/engine.md`, section 11, "Reflection layouts") and logged once:
`player: camera collision: ... parameters <n> bytes: world 0 start 8 end 20 ...`; if a
parameter is missing or the layout is not as expected the collision stays off with a
warning. The status shows `collision` (1 on, 0 off, -1 not available), `boom` (the level
boom's length), `hit` (distance of the hit from the pivot, -1 none), `applied` (the length
used), `traces`, `hits` and `trace_failures`. Dev command:
`fp collision <0|1> [margin cm] [channel: 0 visibility, 1 camera]`.

Measured (Null backend, street of the Sector 7 slums save, third person, `fp status`):
the function and its layout were found at start (log above: parameters 248 bytes, `OutHit`
at 64 with `Time` at +4, `ReturnValue` at 240); about 2000 traces over 30 s with no failed
call. At the normal pivot (55 cm) nothing was hit in the street, walking backwards 11 m and
turning the camera through 360 degrees in eight steps, on either channel (the street is
wide; the eyes stayed at the level boom's 369 to 396 cm). With the pivot lowered to 60 cm
below the pawn's location (`fp pivot -60`, the trace close to the ground) and a 300 cm
margin, the visibility channel hit kerbs and props at some yaws: the applied length went
406.0 -> 347.4 cm in one frame and back to 403.0 after 0.3 s and 405.5 after 0.6 s; in a
second sweep 406.0 -> 270.3, then 398.6 after 0.3 s and 405.6 after 0.6 s. The camera
channel (`TraceTypeQuery2`) hit nothing in the same sweeps, so the default trace channel is
visibility (`TraceTypeQuery1`). Not yet shown: a real wall or person behind the level boom
in play (no such spot was reached with scripted input), and how often visibility hits small
props or foliage in other areas.

### Camera modes: evidence

Street and first room of the Sector 7 slums save, Null backend, Quest 3 class asymmetric
FOV, 2064x2208 per eye; eye positions from `stereo views`, the pawn's from `fp status`.
Captures in `captures/camera/runE` (Null) and `captures/camera/runF` (SteamVR null driver).

| Game camera pitch | Game camera Z (`boom = game`) | Eye Z with the level boom | Eye distance behind the pawn (level) |
|---|---|---|---|
| -10.0 (rest) | 215.5 | 156.6 | 342 cm |
| +12.8 (looking up) | 123.2 | 166.0 | 194 cm (the game's camera collision shortened the boom near the floor; kept) |
| -35.9 (looking down from above) | 342.9 | 157.8 | 317 cm |
| -9.5 after walking backwards | 204.4 | 162.1 | |

With the game's boom the eyes moved over 2.2 m with pitch input; with the level boom they
stay within 10 cm of the pivot's height (156 cm here, about shoulder height) while the boom
length and the yaw follow the game. Pairs: `e04_level_pup` / `e03_game_pup`,
`e05_level_pdown` / `e06_game_pdown`, `e07_level_backed` / `e08_game_backed`. Seen in
`e05_level_pdown`: the eyes are where the game camera would be at zero pitch, so whatever
stands there (an NPC behind the character in the first room) is close in front of the
eyes, as it would be for the game's own camera at that angle. Whether the level position
can end up inside an obstacle that the pitched camera passed over (a counter, a low wall)
has not been seen; the game's collision only shortens the boom along the pitched direction.

First person (`e10_first_stand`, `e11_first_turned`, `e12_first_walking`): the eyes at Cloud's
eye bones, 74.8 cm above the pawn's location (175.9 standing in the first room), facing the
game camera's yaw; turning with the mouse turns the view, walking moves it with the
character. `e13_first_nohide` shows the view with nothing hidden. In `e10` (body hidden,
sword not yet) the Buster Sword's grip filled a third of the view: the sword is a separate
actor (`WE0000_01_Cloud_IronBlade_C`) attached to the body mesh, so it is now hidden with it
(`captures/camera/runH`: `h01_first_default` shows a clear view, the log line
`player: first person: hid 2 of 2 mesh(es) ...` names `CharacterMesh0` and the sword's
`SkeletalMeshComponent0`). Toggled eight times with Home, then a simulated battle and its
end: each exit logged `2 mesh(es) of the character shown again`, and the third-person
captures after the toggles (`h05_third_toggled`, `h08_third_after_toggles`,
`h09_battle_sim_third`) show Cloud with his sword; after `stereo off` nothing is hidden
(`fp status`: `hidden 0`). The same on SteamVR's null driver: `captures/camera/runF`,
`runK`.

### First person

- **Toggle**: the keyboard key `toggle_key` (virtual-key code, default 36 = Home, while the
  game window has the focus), the gamepad combination View/Back + right stick click
  (`pad_toggle`), or `fp toggle` on the dev pipe. A manual toggle holds until the next
  automatic switch.
- **Default**: first person outside battles (`default = 1`): stereo starts in first person
  and returns to it after every battle. `default = 0` starts in third person and returns to
  third person after a battle.
- **Eye**: the point between the character's eye bones (`L_Eye` and `R_Eye`; without them
  a bone named `head` or containing `head`, not an end or helper bone), found once per pawn
  by name over its skeletal meshes (`SkinnedMeshComponent.GetNumBones` / `GetBoneName`) and
  read every frame after the world has ticked (`SceneComponent.GetSocketLocation`). The
  pawn's own movement is followed without delay; only the eyes' offset from the pawn's
  location is filtered:
  - `head_bob = 0` (default): the offset goes through two first-order low-pass stages in a
    row, each with the time constant `steady_seconds` (0.3 s). The walk and run cycles move
    the head about 3 times a second (up and down, forward and back) and 1.5 times a second
    (side to side); the pair keeps about 3 % and 12 % of those, while a slow change of the
    offset (crouching, climbing, the run's lower and forward-leaning posture, idle sway)
    arrives about 0.6 s late. The filter works in world axes: when the character turns
    while running, its forward lean (about 20 cm) swings round with the same delay.
  - `head_bob = 1`: the first versions' behaviour, the offset smoothed over about 80 ms
    (animation jitter only), so the view follows every step.

  Both filters run all the time, so `fp bob 0|1` switches without a jump. `head_offset`
  (forward, right, up, in the camera's yaw frame) is added. When the bone cannot be read (no such bone, a call fails, a location more than
  2.5 m from the pawn) the eyes go to the pawn's location plus `eye_offset` for that frame.
- **Body**: with `hide = pass` (default since 08/10) the character's own skeletal meshes stay
  visible to the engine but are left out of the main pass
  (`PrimitiveComponent.SetRenderInMainPass(false)`, set again every 30 frames because there is
  no getter), so the body is not drawn but its shadow stays and the game's footstep sounds keep
  playing (confirmed in the headset on 08/10: "works perfectly"; with `meshes` the owner heard
  no footsteps). The attached meshes are hidden as below. With `hide = meshes` (the default
  before) every skeletal mesh component owned by the pawn
  that is visible is hidden (`SetVisibility(false)`, not propagated to attached components),
  and so is every mesh component of another actor attached to those meshes (Cloud's sword
  is one: `SceneComponent.GetChildrenComponents(true)`), when the first-person blend passes
  half way; exactly those are shown again when first
  person stops applying (toggle, authored camera, battle, stereo off) or when the controlled
  pawn changes. A mesh the game shows again while hidden is hidden again the next frame.
  Finding the meshes scans the object array each time first person starts (a one-off cost
  of a few milliseconds on the game thread), so meshes added since the last time (equipment)
  are included. When the game exits nothing needs restoring (visibility is not saved).
- **Other ways of hiding (experimental, dev pipe `fp hide <mode>` or `[first_person] hide`)**:
  - `pass` (now the default, above). Seen headless (`captures/fp/run2/p2_pass_down`): looking down,
    no body, but the character's shadow (the feet) stays on the floor; looking ahead
    (`p1_pass_ahead`) the same as `meshes`.
  - `bones`: the root bone (`Trans`) of each of the character's skeletal meshes is hidden
    with `SkinnedMeshComponent.HideBoneByName(name, PBO_None)` (`UnHideBoneByName` when
    first person ends, `IsBoneHiddenByName` checked every frame); attached meshes as with
    `meshes`.
  - `head`: only the head through its bones, body and sword shown: the highest ancestor of
    the eye bone with "head" in its name (`fp headbone <name>` picks another bone). The
    chain from the eye up, from `GetParentBone`: `L_Eye C_FaceBase_a C_Head_a C_Neck_a
    C_Spine_d C_Spine_c C_Spine_b C_Spine_a C_Hip_a Trans`. **Does not work yet**: with
    `C_Head_a` hidden the first-person view turns almost white (mean level 248 of 255 in
    `captures/fp/run1/h1_head_ahead`, `h2_head_down`, `run2/h4_head_first`; the cause is
    not known); with `C_FaceBase_a` hidden the view is normal but the hair (not under the
    face base) fills part of it (`run2/h6_facebase_first`). With nothing hidden the view
    ahead is clean (`run2/p4_none_ahead`) and looking down shows the shoulder armour and the
    chest (`run2/p3_none_down`), so the head is only in the way when the view faces away
    from where the character faces.

  In all three the eye bones stay readable: with the root bone or `C_Head_a` hidden,
  `fp bones eye` still gave the eyes 74.2 to 74.6 cm above the pawn, as without hiding.
  In the Sector 7 slums save the meshes attached to the character are three, not one: the
  sword, `FA0034_00_76idcard_Standard_C` and `FA0233_00_Town7PhotoFrame_Standard_C`
  (`player: first person: hid 4 of 4 mesh(es)` in the log).
- **Footsteps** (reported from a headset session: none heard in first person). Tried
  headless by recording the PC's audio output (WASAPI loopback) while the character runs
  4.5 s forward and back, in each hiding mode, and folding the envelope of three frequency
  bands at the step period that the head trace gives (0.33 s, 3.0 Hz): footsteps show as
  a peak at that period over the other periods between 0.25 and 0.45 s. The music and the
  street's ambience are louder than the steps, so the result is weak. Mean score (z, six
  values per recording; 0.4 standing still): nothing hidden 2.9, 2.3, 1.7 (three
  recordings); `meshes` 0.9, 1.3; `pass` 1.4, 1.3; `bones` 1.3; third person 1.0
  (`captures/fp/run1`, `run2`, `captures/fp/fold3.py`). That points to hiding the
  character, by any of the three methods, weakening the steps, but the spread between
  recordings of the same mode is as large as the differences: **not established**. A
  clearer test needs a quieter spot or the game's own footstep events (an object or
  function found through reflection) instead of the mixed audio.

  Where the game's footsteps come from (reflection, `fp classes Foot`, `fp props foot`,
  `fp classes AnimNotify`; `captures/fp/run3/reflection.txt`): not from animation
  notifies (`EndAnimNotifyPlayCharacterFootSound` was seen only in fall animations such as
  `N_Fall01` and `B_Fall01`; the listing stops at 60 objects, so others may exist), but from
  Square Enix's automatic motion sound system: the
  structures `SQEXSEADAutoSeDetectorSettingFootStep` (`bEnableFootStepWalkRun`,
  `FootStepWalkVolumeRangeMin/Max`, `FootStepRunVolumeRangeMin/Max`, `bEnableFootShuffle`,
  ...), `SQEXSEADAutoSeAnalyzerSetting` (`AutoCalcFootGroundedThresholdRatio`,
  `AssumeFootMotionlessMoveLenInWorld`), `SQEXSEADAutoSeComponentFootInitParams.FootName`
  and `SQEXSEADAutoSeMotionSoundFilter` (`bMSFilterFlag_FootStep`) describe a component that
  derives step, shuffle and cloth rustle sounds from the foot bones' motion. If the
  silence is real, the likely place is that component skipping a character that is not
  rendered; the next step is to find the component on the pawn (its class name contains
  `AutoSe`) and its properties, not the animation notifies. `Actor.WasRecentlyRendered`
  exists; no function with "foot" in its name does.
- **Head bob, measured** (`captures/fp/run1`, Null backend, Sector 7 slums street, 90 fps;
  `fp trace` writes each first-person frame's pawn location and the eyes' raw, 80 ms and
  steady offsets to a CSV; analysis after removing the 0.5 s moving average). Standing:
  the eyes 75.1 cm above the pawn's location, still within 0.01 cm. Running (W held, 540 to
  590 cm/s, two segments of about 4 s): the pawn's location does not bob (its height
  varies by 0.00 cm); the eyes' raw offset bobs 15.4 / 18.1 cm peak to peak vertically
  (RMS 3.2 / 3.5 cm) at 3.0 Hz, 8.7 / 13.1 cm sideways (RMS 1.7 / 1.9) at 1.4 Hz and
  10.4 / 19.9 cm forward and back (RMS 2.0 / 3.0); the run also lowers the eyes from 75 to
  about 60 cm and puts them about 22 cm ahead of the pawn's location. What reached the view:

  | | vertical p-p (RMS) | sideways p-p (RMS) | forward p-p (RMS) |
  |---|---|---|---|
  | raw eye bones | 15.4 / 18.1 (3.2 / 3.5) cm | 8.7 / 13.1 (1.7 / 1.9) cm | 10.4 / 19.9 (2.0 / 3.0) cm |
  | `head_bob = 1` (80 ms) | 9.6 / 11.5 (2.0 / 2.2) cm | 6.3 / 8.7 (1.3 / 1.5) cm | 7.8 / 14.4 (1.4 / 2.3) cm |
  | `head_bob = 0` (2 x 0.3 s) | 1.9 / 2.1 (0.29 / 0.31) cm | 0.9 / 1.2 (0.18 / 0.21) cm | 2.1 / 2.4 (0.23 / 0.48) cm |

  The remaining peak-to-peak values are mostly the start and stop of the run (the change
  of posture), not the steps. Dev commands: `fp bob 0|1`, `fp steady <s>`,
  `fp trace <frames> <csv path>`.
- **Blend**: switching between third and first person moves the eye base over
  `blend_seconds` (smoothstep); a switch to or from an authored camera is a cut, as the
  game's own camera cuts there.
- **Combat**: with `auto_combat = 1` the mode switches to third person while a battle is in
  progress and back to the default afterwards (a manual toggle holds until then). The
  signal is `battle_signal`, read once per frame on the game thread; every change of its
  value is logged (`player: battle signal 0 -> 1`) together with the switch it causes
  (`player: battle started: third person`). `fp combat 1|0|auto` overrides it for tests.

  The default signal is `EndBattleAPI.GetBattleSceneID` (Square Enix's static battle
  function library, `/Script/EndGame`): no parameters, returns an FName, the ID of the
  current battle scene; the module treats a non-zero name index (anything but `None`) as a
  battle. **Confidence: moderate, not verified in a battle.** What is verified: the function
  exists, is called without faults every frame, and returns `None` in exploration (street
  and first room of the Sector 7 slums). What is inferred: that it returns a battle scene
  ID during a battle and `None` again afterwards. It was chosen over the other candidates
  found in the object and name tables because it needs no object of a battle class (it is
  static) and its name and the game's data tables (`BattleSceneID`, `BattleScenePhase`,
  `GetBattleSceneCount(Name)`) describe battles as "battle scenes". Other candidates, all 0
  in exploration, if this one turns out wrong: `EndBattleAIController.GetBattleInSituation`
  (enum, on the party members' AI controllers such as `PC0000_00_Cloud_Standard_AI_C`) and
  `EndBattleAIController.IsInDummyBattle` (bool); `EndMenuAPI.SetFieldMenuInBattle(bool)` is
  the game telling its menus that a battle started (a setter, so not readable). A different
  function can be tried without rebuilding: `battle_signal = Class.Function [world]
  [result=<offset>:<size>]` (`world` passes the pawn as a world-context argument; the result
  offset and size in the parameter block default to 0 and 1 byte), or `fp signal ...` on the
  dev pipe.

### Audio listener at the head

`[first_person] audio_listener = 1` (default): the game's audio listener follows the game
camera (behind the character) and ignores the headset. While first person applies (stereo,
follow camera, the blend past half way), `audio_listener.cpp` calls
`PlayerController.SetAudioListenerOverride(nullptr, location, rotation)` once per frame
through `ProcessEvent`: the location is the centre between the last stereo frame's two eye
cameras, the rotation the left eye camera's (headset yaw, pitch and roll included;
`device::last_listener_pose`, one frame old). The parameter block is
`{USceneComponent*, FVector at 8, FRotator at 20}` (UE4.18 float vectors). It calls
`PlayerController.ClearAudioListenerOverride` when first person stops applying, stereo goes
off, `audio_listener` is switched off, or the controller or pawn changes (log line
`audio: listener back at the game camera (<reason>)`). Both functions are found by name
(`audio: ... found`). Counters: `fp status` (appended) or `fp audio [on|off]`.

Status (08/10, Null backend, headless): both functions found at start, 1164 and 2867 calls
without a failure over the first runs, the pose at the head (about 1 m above the pawn's
location, yaw of the eyes), cleared on the first/third person toggle, on `fp audio off` and
on stereo off, set again when first person returned. Whether the sound now turns with the
head can only be heard in a headset.

### Gamepad toggle

The loader passes every successful `XInputGetState` result the game receives through
`ff7vr::engine::filter_pad` (registered only when the stereo device is enabled; every user
index, each with its own state). See "The real pad path" below for where the filter sits. View/Back (`0x0020`) held and the right stick click (`0x0080`) pressed requests a
toggle; the filter is shared with the other combinations of "Player controls", which
describes how the buttons are hidden from the game. The game polls XInput only while a pad
is connected; `fp pad <hex buttons>` feeds a button state through the same filter for a
test without a pad. The test below was made with the earlier filter (View passed through
until the combination formed); the current one is tested in "Player controls: tests".

Tested without a pad (`fp pad`, `captures/camera/runE`): `0x0020` (View alone) passes
unchanged; `0x00a0` (both) toggles once and reaches the game as `0x0000`; `0x00a1` while
still held gives `0x0001` (A passes, no second toggle); `0x0080` (View released, stick still
clicked) gives `0x0000` until both are released; `0x0001` afterwards passes. The toggle
counter rose by exactly one. **Not tested**: a real pad (none is connected to the test
machine, and without one the game never calls XInput), so that the game's own use of View
and the stick click is fully suppressed while the combination is held is inferred from the
filter's output, not seen in the game. The keyboard toggle (Home) was tested in the game:
every press toggles once.

## Player controls

`src/engine/src/controls.cpp`. What a seated player can do without leaving the game:

| Action | Keyboard (default) | Gamepad (hold View/Back, then press) | What it does |
|---|---|---|---|
| first / third person | Home (`[first_person] toggle_key`) | right stick click; or, without View, both stick clicks together (`[controls] fp_toggle_chord`) | switches the camera mode (see "First person") |
| recenter | End (`[controls] recenter_key`) | left stick click | the direction the head faces now becomes forward, and the head's position the origin, for the view and for the floating panels: the UI panel and the virtual screen are placed in front of the head again |
| stereo off / on | Insert (`[controls] stereo_key`) | Menu/Start | stereo off: the game is shown on the virtual screen (the same fallback as for menus and loading screens); on again: back to 3D. The game window keeps its size and mode either way. While no XR session runs (the runtime asked the game to let go of the headset, for example after SteamVR or the streaming app was closed and opened again; the render module then waits for `xr-restart`), the same key reconnects instead (`xr-restart`) and keeps stereo on |
| UI panel nearer | Page Down (`[controls] ui_nearer_key`) | D-pad down | the HUD/menu panel `ui_step` (0.25 m) nearer, down to `ui_min` (0.75 m); its size in metres stays, so it looks larger |
| UI panel farther | Page Up (`[controls] ui_farther_key`) | D-pad up | `ui_step` farther, up to `ui_max` (8 m) |

Keys are virtual-key codes (35 End, 36 Home, 45 Insert, 33 Page Up, 34 Page Down; `0` = no
key), read once per engine frame with `GetAsyncKeyState` and acted on when pressed, only
while a window of the game has the focus. The game itself does not see them differently
(they are not removed from its input); the defaults are keys the game does not use in
exploration or in its menus as far as tested (Space, m, F1, Escape, W/A/S/D, the mouse,
Enter and Backspace are the game's).

Gamepad combinations: while View/Back is held, pressing one of the buttons above triggers
its action, once per press (holding View and pressing D-pad up three times moves the panel
three steps). From the first combination until View and every combination button are
released, all of them are removed from the state the game receives. View alone is held
back from the game while it is down and handed to it as a press of about 120 ms when it is
released without a combination (`[controls] pad_hold_view = 1`), so the game's own View
action (the map) does not open on the way to a combination; it then happens on release
instead of on press. `pad_hold_view = 0` passes View through until a combination is
pressed (the game then sees View go down). `[controls] pad = 0` switches the recenter,
stereo and panel combinations off; `[first_person] pad_toggle = 0` the first-person one.

Every trigger is logged with its source (`controls: recenter (keyboard): ok recenter
requested ...`, `controls: stereo on/off (gamepad): stereo off ...`, `controls: UI nearer
(keyboard): UI panel 3.00 m -> 2.75 m`), and the render module logs the recenter itself
when it is applied (`xr: recentered: yaw ... deg, position ...`). Recenter and the panel
distance go through the render module's registered commands (`recenter`, `ui status`,
`ui distance`) on a short-lived worker thread, so a slow command never stalls the game
thread. `controls status` on the dev pipe shows the keys and counters, `controls pad <hex>`
feeds a button state through the gamepad filter, `controls recenter|stereo|nearer|farther`
triggers an action.

### Player controls: tests

In the game (first room of the Sector 7 slums save, Null backend, the player ini with the
dev pipe on, `captures/controls/runA`): End logged `controls: recenter (keyboard): ok
recenter requested` followed 11 ms later by the render module's `xr: recentered`; Insert
switched stereo off (`stereo status`: `wanted=0 active=0`, the eye capture `p02_stereo_off`
shows the game on the virtual screen) and on again (back to first person within 30 ms,
meshes hidden again); four Page Down presses moved the panel from 3.00 to 2.00 m and six
Page Up presses to 3.50 m (`ui status`, captures `p04_ui_default`, `p05_ui_nearer`,
`p06_ui_farther`); Home toggled twice. In the game's main menu Page Up, Page Down and End
changed nothing on screen (`p08` to `p11`; the menu footer shows that F2 is the game's
photo mode key). Reconnect (`captures/controls/runR`): after `xr-stop` the render module
logged `staying off until 'xr-restart'` and the engine rendered mono; Insert logged
`controls: stereo on/off (keyboard): no XR session, reconnecting (stereo on): ok`, the
session was created 8 ms later and recentered, `stereo status` showed `wanted=1 active=1`
and first person again (`r01_reconnected`); the next two presses switched stereo off and
on as usual.

Gamepad, without a pad (`controls pad`): `0x0020` (View down) gives `0x0000`, then
`0x0000` (released) gives `0x0020` twice within 120 ms and `0x0000` after 300 ms (the View
press handed to the game on release); `0x0020`, `0x0060` (View + left stick click) gives
`0x0000` and logs `controls: recenter (gamepad)` and `xr: recentered`, the following
`0x0020` and `0x0000` stay `0x0000` (no View press after a combination); `0x0030` (View +
Start) switched stereo off, the next `0x0030` on again; View held with D-pad up, up, down
moved the panel 3.00 -> 3.25 -> 3.50 -> 3.25 m; `0x00a0` toggled first person;
`0x0001` (D-pad up alone) and `0x0080` (stick click alone) pass unchanged. **Not tested**:
a real pad (none is connected to the test machine, and without one the game never calls
XInput), so what the game does with the delayed View press, and that it never sees the
hidden buttons, is inferred from the filter's output.

`[controls]` keys:

| Key | Default | Meaning |
|---|---|---|
| `recenter_key` | `35` (End) | recenter |
| `stereo_key` | `45` (Insert) | stereo off / on |
| `ui_nearer_key`, `ui_farther_key` | `34` (Page Down), `33` (Page Up) | UI panel distance |
| `ui_step`, `ui_min`, `ui_max` | `0.25`, `0.75`, `8` | metres per press and the limits |
| `pad` | `1` | the gamepad combinations for recenter, stereo and the panel |
| `pad_hold_view` | `1` | View alone reaches the game on release (see above) |
| `fp_toggle_chord` | `L3+R3` | buttons pressed together that toggle first/third person (see below); empty = off. Names: `L3 R3 A B X Y LB RB Back Start Up Down Left Right` (also `LS RS View Menu L1 R1 DPadUp ...`) |
| `fp_toggle_chord_ms` | `150` | how close together the chord's buttons must go down |
| `pad_log` | `0` | log the first N changes of the pad state in and out of the filter (see "Diagnostics") |
| `pad_after_hooks` | `1` | point the game's `XInputGetState` import at the loader's wrapper, so the filter sees what the game receives even when the Steam overlay hooks the export; `0` = filter inside the export, as before |

### The first/third person chord

`fp_toggle_chord` (default both stick clicks) is a second gamepad toggle next to View +
right stick click, in `controls::filter_pad` before the View logic (skipped while View
is down or a View combination is latched; a chord under way finishes). States: idle;
pending (some of the chord's buttons down: they are held back from the game); fired
(all down within `fp_toggle_chord_ms` of the first: one toggle, the chord's buttons
hidden until all are released, so holding never repeats); passed (the window ran out:
the buttons reach the game as they are, from then on). A press released while pending
is handed to the game afterwards as a 120 ms press. Cost for a single stick click: it
reaches the game up to the window late. The toggle goes through
`player::request_pad_toggle` (the same as View + R3) and is logged as
`controls: first/third person (gamepad chord): toggle requested`.

Dev pipe: `controls status` shows the chord, its counters (started, fired, passed late,
replayed short) and the last state in and out of the filter; `controls padlog <n>|on` logs
the next n changes (see "Diagnostics" below); `controls chord <buttons|off> [ms]` changes
it live; `controls pad <hex> [user]` feeds one state through the filter.

Tested in the game without a pad (`captures/chord`, Null backend, first room of the
Sector 7 slums save): button states fed through the filter every 15 ms over the pipe
(`controls pad`), forwarded state per change:
- L3 tapped 80 ms: game `0x0000` while down, then `0x0040` for 124 ms, then `0x0000`
  (replayed short); no toggle.
- R3 held 500 ms: `0x0000` for the first 156 ms, then `0x0080` until released; no toggle.
- L3, R3 50 ms later, both held 1.5 s, R3 released first, L3 300 ms later: `0x0000`
  throughout; one toggle (`player: toggled to third person`, camera mode first person
  (head bone) -> blend -> third person (level boom)); `c01_before` shows first person
  without the body, `c02_after_chord1` third person with Cloud.
- both in the same poll, held 600 ms: `0x0000`, one toggle back to first person
  (`c03_after_chord2`).
- R3 300 ms after L3: L3 reaches the game after 150 ms, then `0x00c0`; no toggle.
- View, then View + R3: `0x0000`, toggles as before (`c04_after_view_r3`).
The game itself never reached the mod's XInput code in this run (`ping`:
`xinput_calls=0`, also with `[dev] virtual_pad = 1`): the Steam overlay's hook on the
export answered (see below), so what the game does with a late stick click is untested.

### The real pad path

The game reads pads two ways (`docs/re/engine.md`, "Gamepad input paths"): UE4's
`FXInputInterface` calls `XInputGetState` (import by ordinal 2 from `xinput1_3.dll`, one
call site, users 0 to 3 every engine frame while a pad is connected), and Square Enix's own
DirectInput/HID code (`dinput8.dll`, `HID.DLL`) reads DualShock and other DirectInput pads.
Only the first path can carry the View combinations and the chord: a pad the game reads
through DirectInput (a DualSense without Steam Input, for example) never reaches the filter.

The Steam overlay (`gameoverlayrenderer64.dll`, in the process also when the exe is started
directly) patches the entry of the mod's exported `XInputGetState` for Steam Input, and its
hook may answer without calling the mod's code. Before 2026-10-07 the filter ran inside the
export, so with the overlay present the game's polls could bypass it entirely. Now the loader
points the game's import slot at a wrapper (`[controls] pad_after_hooks = 1`, default) that
calls the export through its current entry (the overlay's hook, if any, then the system DLL)
and applies the filter to what comes back, i.e. to exactly what the game receives.

### Diagnostics (for a session with a real pad)

Always on, cheap (counters on the call path, lines written once or per change):
- at start: `controls: XInput: import slot ff7remake_.exe+0x40941f8 now calls the pad filter
  after any hook on the export`;
- at the game's first `XInputGetState` call: `controls: the game's first XInputGetState call:
  user 0, thread N, called from ff7remake_.exe+0x1d2f884; the system reports a pad|no pad`
  and where the game's imports point and whether the export is patched (and by which module);
- after 3 s: `controls: the game polls XInput on thread N every X ms (user U; ...)`;
- `controls: XInput user N has a pad` / `lost its pad` on every change;
- two lines in every 10-second timing block: `timing:   controls: xinput: <calls> ...; pads at
  user ...` (calls per user index with the poll interval, how many reached the system DLL when
  a hook answered the rest) and `timing:   controls: pad filter: <polls>, <state changes>;
  chord L3+R3 (150 ms): started, fired, passed late, replayed short, ignored while View held;
  View combinations; now <chord state per pad>; last pad N in 0x.... -> game 0x....`.

`[controls] pad_log = N` (default 0) also logs the first N changes of the state in or out of
the filter: `controls: pad 0 in 0x0040 -> game 0x0000 (poll +13.9 ms, previous change
812.0 ms before; chord idle -> pending)`. Dev pipe: `xinput status`, `xinput timing`,
`xinput probe` (calls the export's current entry for users 0 to 3 and says whether each call
reached the system DLL).

Tested 2026-10-07 (Null backend, Sector 7 save, no physical pad; `captures/chord`):
- Before the wrapper: the export patched by `gameoverlayrenderer64.dll+0xd0580`, `ping`
  `xinput_calls=0` with the virtual pad on. With it: the game polls on its game thread from
  `ff7remake_.exe+0x1d2f884`, user 0 every 1.6 to 11 ms (one poll per engine frame), users 1
  to 3 once; 11583 of 11583 calls answered by the overlay's hook without reaching the mod's
  export or the system DLL (`xinput probe`: "reached the system XInput no" for all four).
- End to end through the game's own polls (virtual pad, `captures/chord/vpad-test.ps1`):
  L3 then R3 78 ms later: game `0x0000` throughout, one toggle (`player: toggled to third
  person`); L3 tapped 100 ms: replayed `0x0040` for 123 ms; R3 then L3 230 ms later: R3
  reaches the game after 157 ms, no toggle; both together: one toggle back to first person.
- Through the filter (`captures/chord/chord-test.ps1`, `filter-cases.txt`): the cases above
  plus a chord on user index 1, a 220 ms poll gap (L3 seen, next poll has both: fires), 2 ms
  polling with 90 ms skew (fires), R3 released and pressed again while L3 held (no second
  toggle), A held through the chord (A passes, the stick clicks do not).
**Not tested**: a physical pad (none connected); what the overlay's hook returns for a real
XInput pad, and whether Steam Input is on for the game on the player's PC.

## Snap turn

`src/engine/src/snap_turn.cpp`, `[comfort]` in `ff7vr.ini`:

| Key | Default | Meaning |
|---|---|---|
| `snap_turn` | 0 | degrees per step (30 or 45 typical); 0 = off, the right stick turns the game camera smoothly as in the flat game |
| `snap_turn_deadzone` | 0.6 | right stick X deflection (0.1 to 0.95) that makes a step; the next step needs the stick back under half of it |
| `snap_turn_repeat_ms` | 0 | above 0: another step every that many ms while the stick stays pushed |
| `snap_left_key`, `snap_right_key` | 0 | optional virtual-key codes for a step left / right |
| `snap_log` | 0 | log the next N left stick rotations (at most one line per 500 ms) |

How it works. The loader's XInput wrapper calls a stick filter after the button filter
(`xinput::set_stick_filter`, the same call path as the pad filter, so it also applies
after the Steam overlay's hook). While snap turn is on and 3D renders (menus, movies and
the virtual screen get the stick unchanged), the right stick's X reaches the game as 0, so the game camera never yaws from the stick (its Y, the camera pitch, passes).
A push beyond the deadzone asks the render module for one step
(`render::RequestSnapTurn`, lock-free; the `snap <deg>` dev command does the same). The
XR library applies it in the next `WaitFrame` as a yaw on top of the recenter
(`IXrBackend::AddSnapYaw`): the eye views turn about the vertical axis through the
recenter origin; the quad layers (HUD panel, virtual screen) keep the recenter without
the turn, so the HUD stays in front of the player. The game moves the character
relative to its own camera, so the left stick is rotated by the turn in effect
(`render::GetSnapYawDeg`, positive = right): forward on the stick moves the character
where the player looks. Keyboard movement (W/A/S/D) is not rotated; snap turn is for pad
players. Any recenter (End, View + left stick click, `recenter`, the runtime's own
recenter) makes the current heading forward and clears the turn (the left stick is no
longer rotated from the next poll).

Dev commands: `snapturn status` (setting, turn in effect, steps, polls with the right
stick hidden, last left stick in/out), `snapturn snap <deg>|off`, `deadzone`, `repeat`,
`log <n>|on`, `snapturn stick <lx> <ly> <rx> <ry>` (one stick state, -1..1, through the
filter without a pad), and in the render module `snap <deg>`.

Checked on the Null backend (08/10, `snap_turn = 45`, `[dev] virtual_pad = 1`, first
area): the virtual pad's `stick R 0.9 0 300` went through the game's own XInput poll and
gave one step (`comfort: snap turn +45 deg (right stick)`, then
`xr: snap turn: -45.0 deg -> view turned -45.0 deg from the recenter`, the library's
yaw being positive to the left) and `right stick X hidden in 55 polls (last 29490)`. A
second `stick R` sent straight after the first gave no step: the game never polled a
released stick between them, so the step stayed disarmed; `snapturn stick 0 0 0.9 0`
then `0.95` gave one step, not two. With the view turned 45 degrees right, `snapturn
stick 0 1 0 0` (forward) came out as `(23170, 23170)` (forward-right); at 90 degrees the
pad's `stick L 0 1` reached the game as `(32767, 0)`. A recenter set the turn back to 0.
Eye captures before and after a 45 degree step: the scene moved left in the image (the
view turned right), the HUD panel stayed at the same place in the image. A first build
had the turn's sign reversed; the capture showed it. Not tested: a real pad, the snap
keys, comfort in a headset.


| Key | Default | Meaning |
|---|---|---|
| `enabled` | `1` | `1`: install the stereo device; `0`: nothing is hooked, the game runs unmodified |
| `start_in_stereo` | `1` | stereo switched on from the start (frames are still mono until the host can show them) |
| `host` | `render` | `render` or `fixed` (see above) |
| `world_scale` | `1.0` | multiplies WorldToMeters for the head offset and IPD |
| `decoupled_pitch` | `1` | drop the game camera's pitch and roll |
| `positional` | `1` | apply head position |
| `mirror` | `crop` | desktop window in stereo: `crop` (left eye, centre crop at the window's aspect), `left`, `right`, `both` (side by side, letterboxed), `off` |
| `light_fix` | `1` | light sort-key patch while stereo renders: white blocks on skin indoors (see "Skin lighting fix"; `docs/re/engine.md` section 6) |
| `bloom_fix` | `1` | right-eye bloom fix (below) |
| `ao_fix` | `1` | right-eye ambient occlusion fix (below) |
| `ssr_per_eye` | `1` | each eye's screen-space reflection run limited to its half of the target, and the per-view colour copy for it halved (see "Reflections per eye"); same image, about 0.45 ms less per frame at 2 x 3072x3264 |
| `ssr_fix` | `2` | screen-space reflections per eye. `2` (default since 08/10): none in either eye, the eyes match. `1`: the right view's run is moved into place, but what it computes is wrong ("The right eye's reflections are not its own"). `0`: the right eye has none (see "Right-eye reflections fix") |
| `distortion_fix` | `1` | heat haze and refraction per eye: without it the left view's distortion composite covers both eyes and the right view's draws nothing (see "Right-eye distortion fix") |
| `hzb_skip` | `0` | `1` leaves out the further mips of the hierarchical depth chain nothing reads, while `r.HZBOcclusion` is 0 (see "Volumes and hierarchical depth per view"); gain within noise |
| `tonemap_shift` | `auto` | with ReShade's Luma add-on loaded, the right view's bloom-combine input shifted to the origin (Luma's shader reads it there; without it both eyes show the left eye's image). `auto`: only while Luma is loaded; `0` off; `1` always (breaks the right eye without Luma). See "ReShade and Luma: the tonemapping shift and Luma's DLSS" |
| `luma_dlss` | `0` | `0`: while stereo renders, Luma's own DLSS calls into NGX are refused (Luma's DLSS takes the double-wide target for a 50 % frame and leaves the right eye a squeezed quarter); `1`: allowed, for comparison. No effect without Luma; the mod's own DLSS is not affected |
| `render_scale` | `1.0` | share of each eye's target the views render, per axis (0.3 to 1); the runtime scales the smaller image to the display. Also the upper bound of the dynamic resolution (see "Render scale and dynamic resolution") |
| `dynamic_resolution` | `0` | adjust the render scale every few frames to hold the GPU frame time below `dynamic_resolution_target` of the display's frame period |
| `dynamic_resolution_min` | `0.75` | lowest scale the dynamic resolution may use (per axis; 0.75 is 56 % of the pixels) |
| `dynamic_resolution_target` | `0.85` | GPU frame time to hold, as a share of the display's frame period (0.85 at 72 Hz: 11.8 ms) |
| `vr_window` | `1280x720` | window size the game is switched to while VR renders in a fullscreen mode; `0` keeps the mode (see "Window modes") |
| `movie_screen` | `1` | movie detection: stereo is held off while a pre-rendered movie plays, so the virtual screen shows it (below) |
| `allow_unknown_build` | `0` | try a game build other than 1.0.0.7 if every signature and layout check passes |
| `log_frames` | `0` | log the eye cameras of the first N stereo frames |
| `eye_width`, `eye_height` | `1280`, `1440` | fixed host: per-eye render size |
| `fov_left_deg`, `fov_right_deg`, `fov_up_deg`, `fov_down_deg` | `-45`, `45`, `45`, `-45` | fixed host: left eye FOV (the right eye mirrors left/right) |
| `ipd_mm` | `64` | fixed host |
| `head_motion` | `static` | fixed host: `static`, `yaw`, `sway`, `yawsway` (same scripts as the XR Null backend) |
| `comfort_cvars` | `1` | switch off the flat-screen camera effects while in stereo (see "Flat-screen camera effects") |

`[camera]` (see "Camera modes"):

| Key | Default | Meaning |
|---|---|---|
| `boom` | `level` | `level`: eyes at the zero-pitch boom position around the character; `game`: the game camera's position |
| `pivot_height` | `55` | cm above the pawn's location: the pivot of the game's camera boom (measured 55.4, `docs/re/engine.md` section 11) |
| `follow_distance` | `1500` | cm; a camera farther than this from the pivot is not treated as the follow camera |
| `aim_tolerance` | `75` | cm; largest distance of the pivot from the camera's line of sight for the follow camera |
| `miss_seconds` | `1.5` | seconds the camera must look away from the pivot before the game camera takes over |
| `combat` | `level` | `level`: in a battle the level boom holds while the battle camera frames the enemies (see "Battle camera"); `game`: the aim test as outside battles |
| `blend_seconds` | `0.35` | seconds a change between the camera modes takes (0 = cut); a change of view target is always a cut |
| `collision` | `1` | the level boom is shortened in front of what is between the character and the eyes (see "Camera collision") |
| `collision_margin` | `20` | cm kept between the eyes and what the collision trace hit |

`[first_person]`:

| Key | Default | Meaning |
|---|---|---|
| `enabled` | `1` | first person can be used at all; `0` switches it off completely (toggle, pad combination and automatic switching) |
| `default` | `1` | `1`: first person outside battles, from the start and after every battle; `0`: third person |
| `auto_combat` | `1` | third person while `battle_signal` reports a battle |
| `eye` | `head` | `head`: the eyes between the character's eye bones (`L_Eye`, `R_Eye`), or at its head bone if it has no eye bones; `offset`: at the pawn's location plus `eye_offset` |
| `head_offset` | `2 0 0` | forward, right, up in cm from the eye bones (or head bone), turned with the camera's yaw |
| `eye_offset` | `10 0 75` | forward, right, up in cm from the pawn's location (capsule centre), turned with the camera's yaw; used with `eye = offset` and whenever the head bone cannot be read |
| `battle_signal` | `EndBattleAPI.GetBattleSceneID result=0:4` | `Class.Function [world] [result=<offset>:<size>]`: a reflected function read every frame on a live object of that class (or its class default object for a static function); a non-zero result means a battle; empty = no automatic switch (see "Combat") |
| `hide` | `pass` | `pass`: the character's own meshes are left out of the main pass (shadow and footsteps kept) and the mesh components of other actors attached to them (the sword) are hidden, while in first person; `meshes`: hide all of them (footsteps go silent); `none`: hide nothing; `bones`, `head`: experimental |
| `toggle_key` | `36` | virtual-key code of the keyboard toggle (36 = Home); `0` = none |
| `pad_toggle` | `1` | View/Back + right stick click toggles, and is hidden from the game |
| `blend_seconds` | `0.35` | duration of the move between third and first person |

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
| `stereo lightfix <0\|1>` | skin lighting fix on/off (the light sort-key patch, in place while stereo renders); replies whether the patch is in place now |
| `stereo log <n>` | log the eye cameras of the next n stereo frames |
| `stereo bloomfix [0\|1]` | right-eye bloom fix on/off, with its counters (reduce passes seen, commands queued, draws fixed, misses) |
| `stereo aofix [0\|1]` | right-eye ambient occlusion fix on/off, with its counters (draws fixed, failures); one per stereo frame |
| `ssr [on\|off]` | reflections per eye on/off, with its counters (runs limited, colour copies halved; two each per stereo frame) |
| `ssr poison <0\|1\|2\|3>` | test of reflections per eye: `1` fills every half the fix skips with a loud colour (nothing of it may reach the eye images), `2` fills the half each reflection run writes, `3` the half each colour copy writes (controls: the colour must show) |
| `ssr fix <0\|1\|2>` | right-eye reflections fix off/on, `2` both eyes without screen-space reflections (`runs cleared` counts them); `ssr` shows `applied` (one per stereo frame), `failed`, the x where the right view's result was placed, how often that x changed (`moved`) and results no draw read (`not read`) |
| `hzb [0-4]` | the unread hierarchical depth chain: `0` built (default), `1` further mips left out, `2` / `3` the whole chain filled with near / far depth, `4` the read chain's further mips filled (control); shows the view chains seen and the mips left out |
| `stereo framelog start` / `stop <csv>` | frame log: per frame the start, host return and end of `UGameEngine::Tick`, the render thread's end of the scene and the frame-end command on the RHI thread, each with the thread's CPU time (cycle count) and, at the start of Tick, the latest GPU frame time (see "Turning: where the slow frames come from") |
| `dynres [on\|off]`, `dynres scale\|min\|target <value>` | render scale and dynamic resolution: state, current scale and rect, last GPU frame time and budget, number of changes |
| `stereo movie [on\|off]`, `stereo movie menu <0\|1>`, `stereo movie simulate <on\|off>` | movie detection on/off and its state; `menu 1` counts the menu background players too (test); `simulate on` behaves as if a movie played (stereo off, the virtual screen) until `simulate off`, to measure the switch without a movie |
| `stereo window [<w>x<h>\|0]` | window size while VR renders in a fullscreen mode, and the state |
| `stereo frametime <s>` | frame time window length in seconds; restarts the window (for A/B measurements) |
| `stereo swap <0\|1>` | test: render the right eye into the left half of the target and the left eye into the right half (the eyes then come out swapped). Tells a bug that follows a view's position in the target from one that follows the view |
| `gpu names on`, `gpu trace <prefix> [dump fullscreen \| dump <from> <to>] [scale <n>]`, `gpu status` | one-frame GPU trace (`docs/re/engine.md`, Tools); every bound pixel-shader constant buffer slot is listed, and for read-back events slots 12 and 13 (add-ons such as Luma) are dumped |
| `tonemapshift [0\|1\|2]`, `lumadlss [0\|1]` | with Luma loaded: the right view's bloom-combine input shift (2 = auto) and Luma's own DLSS in stereo (0 = refused); both print their counters (see "ReShade and Luma: the tonemapping shift and Luma's DLSS") |
| `re peek <rva> <n>`, `re poke <rva> <hex bytes>` | read or patch the game image (to try a patch in a running game) |
| `stereo host <render\|fixed>` | switch the source of eye size and views (for tests; switching away from `render` leaves the render module in stereo mode) |
| `fp status` | controlled pawn, view target, follow camera, mode, blend, hidden meshes, toggles (keyboard/dev and pad), pawn location, which eye base the last frame used |
| `fp toggle`, `fp first`, `fp third` | switch first person (manual: holds until the next automatic switch) |
| `fp available <0\|1>`, `fp hide <none\|meshes>`, `fp offset <fwd> <right> <up>`, `fp eye <head\|offset>`, `fp headoffset <fwd> <right> <up>`, `fp blend <s>` | first-person settings |
| `fp bones [text]` | bones of the character's skeletal meshes whose name contains the text (default `head`), with world location and offset from the pawn's location |
| `fp funcs <text> [class text]`, `fp props <text> [class text]` | reverse engineering: reflected functions or properties whose name contains the text, with their class |
| `fp call <object address \| class name> <Class.Function> [hex parameters]` | reverse engineering: calls a reflected function on the game thread and prints the first 48 bytes of its parameter block afterwards (return values follow the arguments) |
| `fp combat <1\|0\|auto>` | test: pretend a battle is or is not in progress |
| `fp signal <Class.Function> [world] [result=<offset>:<size>] \| none` | switch the battle signal while the game runs (same form as `battle_signal`); `fp status` shows its raw value |
| `fp pad <hex buttons>`, `controls pad <hex buttons>` | test: feed an XInput button state through the gamepad filter, prints what the game would get |
| `controls status`, `controls recenter\|stereo\|nearer\|farther` | player controls: keys, counters; trigger an action as its key would (see "Player controls") |
| `fp boom <level\|game>`, `fp pivot <cm>` | third-person camera settings |
| `fp find <name> [outer] [class]`, `fp classes <text>`, `fp chain <hex address \| pawn \| view \| pc>` | reverse engineering: objects by name, objects whose class name contains a text, the class chain of an object |
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

### At headset resolution: 2 x 3072x3264 and 2 x 3600x3600

The player's configuration (`tools/package/ff7vr.ini`: foveation `quality`, UI layer, first
person, light and occlusion fixes) with `r.BloomQuality 0`, Null backend without pacing,
`t.MaxFPS 0`, RTX 5080, measured with `tools/bench/vr-perf-session.ps1` and
`vr-perf-steps/scenes.ps1` (`docs/benchmarking.md`). Frame time = the engine's frame interval
(average over 5 s windows, two windows each); "before" = `ssr_per_eye` off (the state before
2026-10-06), "after" = the defaults. Runs `captures/perf/005341-final-3072x3264`,
`005805-final-3600x3600`.

| View | 3072x3264 before | after | 3600x3600 before | after |
|---|---|---|---|---|
| start of the latest save (doorway, Tifa) | 9.35 ms | 8.98 ms | 11.57 ms | 11.09 ms |
| alley after walking out | 7.88 ms | 7.35 ms | 9.72 ms | 9.05 ms |
| open street | 9.61 ms | 9.18 ms | 11.73 ms | 11.18 ms |
| turning in the street (p95) | 10.1 ms (13.0) | 9.25 ms (12.8) | 12.3 ms (16.0) | 11.1 ms (14.9) |

Headroom of "after" against the display period: at 72 Hz (13.9 ms) 4.7 to 6.5 ms at
3072x3264 and 2.7 to 4.8 ms at 3600x3600 standing; at 90 Hz (11.1 ms) 1.9 to 3.8 ms and
-0.1 to 2.0 ms. Turning raises the 95th percentile by 3 to 4 ms; about half of that is GPU
time (2 x 3600x3600: GPU p95 of scene plus post-processing 13.1-13.9 ms while turning against
11.4 standing) and the rest frames whose CPU side is late (frame p95 14.6-15.8 ms): the engine's
render thread, not the mod ("Turning: where the slow frames come from"). These are GPU frame times
of the game alone: Virtual Desktop's compositor and encoder take GPU time on top, which cannot
be measured without the headset (the player's session at 3072x3264 / 72 Hz ran at 64 to 72
fps before these changes).

Where the frame goes (2 x 3072x3264, start of the save, `fov trace`: GPU time per render
target binding, 9.58 ms from the scene's start to Present):

| Group | GPU ms |
|---|---|
| depth prepass and shadow depths | 1.1 |
| hierarchical depth (two mip chains per view, each view's build reading only its own rectangle; "Volumes and hierarchical depth per view") | 0.6 (0.45 in a later trace) |
| volumetric fog per eye, the shared translucency lighting volume (about 0.07) and other compute | 1.1 |
| G-buffer (base pass, velocity) | 0.9 |
| ambient occlusion, subsurface setup and blurs (per eye) | 0.9 |
| lights, shadow masks, sky and fog into scene colour | 2.6 |
| reflections, subsurface recombine, scene colour and depth copies | 1.4 (about 0.45 of it removed by `ssr_per_eye`) |
| post-processing: temporal AA and tonemapper per eye | 0.93 |
| the mod's own work: two eye blits (0.07 ms each), UI quad (0.013), desktop mirror (0.02) | about 0.2 |

The scene window is 8.6 ms, of which 5.1 ms is shaded with the foveation mask; switching
foveation off costs 1.8 ms (9.4 -> 11.2 ms). CPU: over a whole run (start, load, 3 minutes of
play) the busiest threads are the RHI thread (about 40 % of a core), an unnamed engine thread
(37 %), the game thread (31 %) and the render thread (24 %): the frame is GPU-bound at these
sizes.

### Turning: where the slow frames come from

`stereo framelog start` / `stop <csv>` records, for every engine frame, the start of
`UGameEngine::Tick` (with the latest GPU frame time of the foveation module's timestamps), the
return of the host's frame call (XR frame wait and poses), the end of the device's own work at
the start of Tick, the end of Tick, the render thread's end of the scene
(`RenderTexture_RenderThread`) and the frame-end command on the RHI thread (start and end: the
hand-over to the XR layer and the desktop mirror), each with the calling thread's cycle count
converted to milliseconds. Off by default; when off each point costs one relaxed load.

Street, 2 x 3072x3264, Null backend unpaced, `t.MaxFPS 0`, standing and turning with the
mouse (6 pixels every 10 ms, 6 s windows), run `captures/perf/20261006-032356-diag1-3072x3264`
(`fl_*.csv`). The GPU time of a frame shows up two or three ticks later; aligned at the lag with
the best correlation:

| | frame interval p50 / p95 / p99 | GPU time p50 / p95 | interval above the GPU time, p95 |
|---|---|---|---|
| standing (start, street, street again) | 9.06-9.18 / 9.63-9.92 / 10.2-10.4 ms | 8.84-8.98 / 9.06-9.23 ms | 0.56-1.04 ms |
| turning (left, right) | 8.76-8.82 / 12.64-12.74 / 15.0-16.0 ms | 8.80-8.96 / 10.95-11.16 ms | 2.13-2.21 ms |

So of the 2.8 ms that turning adds to the 95th percentile, about 2 ms is GPU time (the GPU time
of individual frames rises to 10.5-12 ms while the view sweeps) and about 1.2 ms is frames that
arrive later than the GPU could have finished them. In the sequence the late ones come in pairs:
one frame of 13-19 ms, then one of 7.5-9 ms, while the render thread's interval between two
scenes jumps to 16-19 ms for one frame and the next frame catches up (the GPU had a queued frame
to work on). The mod's own points take nothing measurable: the host's frame call returns in
under 0.01 ms (the Null backend unpaced does not wait), the device's per-frame work at the start
of Tick under 0.01 ms, the frame-end command 0.01 ms; the game thread's Tick takes 2.6 ms
standing and 2.7 ms turning (p95 3.5 ms) of a 9 ms frame.

With the threads' cycle counts (run `captures/perf/20261006-041050-turn2-3072x3264`, a 5 s
turn to the left and back to the right per window, the same path every time, two rounds of
each setting in turns):

| Turning | frame interval avg / p95 | GPU time avg / p95 | interval above the GPU time, p95 | frames over 12.5 ms |
|---|---|---|---|---|
| defaults | 9.60-9.81 / 13.52-13.56 ms | 9.34-9.52 / 11.45-11.62 ms | 2.75-2.87 ms | 100-105 of 1119-1143 |
| per-eye fixes off (`ssr off`, `ssr fix 0`, `aofix 0`, `stereo lightfix 0`) | 10.31-10.33 / 13.94-14.14 ms | 10.05-10.07 / 12.04-12.08 ms | 2.80-2.88 ms | 146 of 1063 |
| foveation off | 11.17-11.40 / 14.82-15.81 ms | 10.93-11.11 / 13.03-13.50 ms | 2.83-3.34 ms | |
| `r.HZBOcclusion 1` | 10.11-10.27 / 12.63-12.95 ms | 9.89-10.06 / 12.04-12.29 ms | 1.37-1.58 ms | 60-74 of 1067-1084 |

Standing in the same street before and after: 9.16 and 9.33 ms, p95 9.99 and 10.13 ms, the
interval above the GPU time p95 0.97 ms.

On the frames whose interval exceeds 12.5 ms, per thread (CPU time in the interval, defaults):
the game thread 2.9 ms (2.3-2.4 ms on normal frames), the render thread 6.2-6.4 ms (2.9-3.2 ms;
up to 12.4 ms in a single frame), the RHI thread 6.3 ms (4.5 ms). No thread is busy for the whole
interval (render thread 45 %, RHI thread 45 %), so a slow frame is the render thread doing about
twice its usual work for the newly visible part of the view and the pipeline then waiting on
each other and on the GPU. With the mod's per-eye fixes off the late part is unchanged, and
foveation off only adds GPU time: **the late frames are the engine's, not the mod's**.

`r.HZBOcclusion 1` (the engine's occlusion culling by the hierarchical depth instead of
hardware occlusion queries) halves the late part (p95 above the GPU time 2.8 -> 1.4-1.6 ms,
40 % fewer frames over 12.5 ms) and lowers the turning p95 by 0.6 to 0.9 ms, but costs about
0.5 ms of GPU time on every frame (average frame +0.5 ms while turning). It culls less:
one-frame traces standing in the street (run `captures/perf/20261006-044530-occ-3072x3264`,
`tr_occ0`, `tr_occ1`, `tr_occ0b`) have 358 / 338 and 316 / 334 base-pass draws (left / right
view) with 0 and 586 / 543 with 1 (depth prepass 513 / 483, 461 / 480 and 510 / 474), which is
where its GPU time goes. Whether its culling shows anywhere (objects appearing late at the edges
while turning, a difference between the eyes) was not checked. `r.NumBufferedOcclusionQueries 2`
(the game runs with 1) changes nothing while turning: p95 13.28-13.74 ms against 13.30-13.44 ms,
interval above the GPU time p95 2.61-2.63 against 2.58-2.66 ms (same run). Not applied: a trade, and not fidelity-neutral
until checked. Note that with it on, `hzb_skip` builds the chain again (its guard).

Options measured but not on by default (they change the picture):

| Setting | Effect | Visible cost |
|---|---|---|
| `fov hidden cull` | -0.19 ms (2 x 3072x3264) | pixels the lenses never show are left with stale content, which temporal AA, exposure and bloom can carry into the visible image ("Foveated rendering", `docs/render.md`) |
| `r.HZBOcclusion 1` | turning p95 -0.6 to -0.9 ms (late frames halved), every frame +0.5 ms GPU | occlusion culling by the hierarchical depth instead of queries; popping or a difference between the eyes not checked ("Turning") |
| `fov passes all` (mask on post-processing too) | post-processing 0.93 -> 0.66 ms | temporal AA and tonemapper shaded coarsely in the periphery |
| `render_scale 0.9` / `0.8` / `0.75` | -12 % / -26 % / -31 % at 2 x 3600x3600 | softer image (fewer pixels; the runtime scales up) |
| `r.ScreenPercentage` below 100 | about as above | same softening, and every change reallocates the scene buffers (hitches of 50-140 ms) |
| candidate preset below (fog, shadows) | -21 % at 2 x 2500x2600 | not checked in sunlit or hazy scenes |

### Outdoors at 2 x 2500x2600: what the frame time is made of

The street outside the first room (Sector 7 slums market, under the plate), Null backend
with explicit eye size 2500x2600 and the Quest 3 class FOV, `[xr] null_pace = 0` (no frame
pacing), `t.MaxFPS 0`, 1280x720 window. Frame times are the engine's game-thread frame
interval over windows started right after each change (`stereo frametime 5` or `6`), each
value the average of 750-1000 frames. Run-to-run spread of the baseline in one session:
6.53-6.65 ms, so differences below about 0.15 ms are noise. Logs and captures:
`captures/stereo/runQ`, `captures/stereo/runS1`.

| Configuration | Frame time avg | GPU utilisation | Busiest threads (% of one core) |
|---|---|---|---|
| stereo 2 x 2500x2600 | **6.53-6.65 ms** (p95 6.9) | 100 % | 59 %, 59 %, 34 %, 31 % |
| mono 1280x720 (stereo off) | 1.93 ms | 86 % | 81 %, 63 %, 59 %, 55 % |
| stereo with the candidate preset below | 5.16 ms | 100 % | 82 %, 59 %, 42 %, 38 % |

Stereo at headset resolution is **GPU-bound** (the GPU is busy all the time, no thread is
near a full core). With the preset the busiest thread reaches 82 %: below about 5 ms the
game and render threads start to limit. At 90 Hz (11.1 ms) the scene leaves more than
4 ms of headroom.

Effect of single console variables, all measured at once against the same baseline
(`ab` = changed, then put back):

| Variable (game value -> tested) | Frame time | Change | Visible at headset resolution? |
|---|---|---|---|
| `r.VolumetricFog` 1 -> 0 | 5.55 ms (5.69 in another run) | **-0.98 ms (-15 %)** | not in this scene (`runS1/crop_volfog_pair.png`, differences are the character's idle animation); volumetric fog matters in hazy, shafted scenes not yet seen |
| `r.ScreenPercentage` 100 -> 80 | 5.93 ms (6.07) | -0.6 ms | yes, softer image (`runS1/s_sp80_*`); same lever as the XR resolution scale |
| `r.ScreenPercentage` 100 -> 65 | 4.89 ms | -1.7 ms | yes (`runQ/ab_r_ScreenPercentage_65_*`) |
| `r.PostProcessAAQuality` 4 -> 2 (TAA off) | 6.18 ms | -0.46 ms | yes, aliasing (`runQ/ab_r_PostProcessAAQuality_2_*`) |
| `r.Shadow.MaxCSMResolution` 4096 -> 2048, `r.Shadow.CSM.MaxCascades` 5 -> 3 | 6.18 ms | -0.36 ms | not in this scene (under the plate, mostly indirect light: `runS1/crop_shadow_pair.png`); expected in sunlit areas |
| `r.Shadow.MaxCSMResolution` 4096 -> 1024 | 6.38 ms | -0.26 ms | not checked |
| `r.Shadow.CSM.MaxCascades` 5 -> 2 | 6.36 ms | -0.28 ms | not checked |
| `r.BloomQuality` 5 -> 0 | 6.30 ms | -0.34 ms | yes, no glow at all (`runQ/ab_r_BloomQuality_0_*`) |
| `r.CapsuleShadows` 1 -> 0 | 6.47 ms | -0.17 ms | character soft shadows, not checked |
| `r.TranslucencyLightingVolumeDim` 64 -> 32 | 6.50 ms | -0.14 ms | not checked |
| `r.ViewDistanceScale` 1 -> 0.6 | 6.54 ms | -0.1 ms (noise) | |
| `r.Shadow.DistanceScale` 1 -> 0.6 | 6.57 ms | noise | |
| `r.AmbientOcclusionLevels`, `r.ContactShadows`, `r.MotionBlurQuality`, `r.DepthOfFieldQuality`, `r.LightShaftQuality`, `r.DynamicRes.OperationMode` 2 -> 0 | 6.63-6.70 ms | none | |
| right-eye bloom fix on / off | 6.54 / 6.52 ms | none measurable | |

`sg.ShadowQuality` was already 1 and `r.SSS.Quality` already 0 in the game's settings
(they served as controls: no change). The game's dynamic resolution
(`r.DynamicRes.OperationMode 2`, budget 16.7 ms) never lowered the resolution here: the
frames were far below its budget.

Candidate VR preset for `[stereo_cvars]` (measured together: 6.54 -> 5.16 ms, -21 %; pair
`runS1/s_base4_*` / `s_preset_*`, no difference visible in this scene):

```
[stereo_cvars]
r.VolumetricFog = 0                     ; -1.0 ms
r.Shadow.MaxCSMResolution = 2048        ; with the next line -0.36 ms
r.Shadow.CSM.MaxCascades = 3
r.TranslucencyLightingVolumeDim = 32    ; -0.14 ms
```

It is **not** applied by default: the only evidence is one scene under the plate, where
neither fog nor sun shadows matter much. It should be checked in a sunlit outdoor area and
a hazy interior (reactor, sewers) before it becomes the default. For comfort rather than
speed, `r.MotionBlurQuality = 0` (no cost difference) removes the game's camera motion blur,
which in a headset smears the image while the camera turns; not applied either (no capture
of it in motion yet).

## Graphics profiles

`[graphics] profile = quality | balanced | performance | custom` (default `custom`: nothing
applied). The bundles are tables in `src/core/graphics_profile.cpp`; the loader applies
them right after reading the ini (`graphics_profile::apply`), filling only the keys the ini
does not set, so every module reads them as ordinary ini values and an explicit key always
wins. The log says `graphics: profile <name>: applied ...` and `... left to the ini ...`.

| Key | quality | balanced | performance |
|---|---|---|---|
| `[foveation] preset` | quality | performance | performance |
| `[stereo] render_scale` | 1.0 | 1.0 | 0.9 |
| `r.StaticMeshLODDistanceScale`, `r.SkeletalMeshLODBias`, `foliage.LODDistanceScale`, `r.ViewDistanceScale` | 0.5, -1, 2, 1.5 | 0.5, -1, 2, 1.5 | 1, 0, 1, 1 (engine defaults) |
| `r.Shadow.MaxCSMResolution`, `r.Shadow.CSM.MaxCascades` | - | 2048, 3 | 2048, 3 |
| `r.TranslucencyLightingVolumeDim` | - | 32 | 32 |
| `r.VolumetricFog` | - | - | 0 |

The console variables go into `[stereo_cvars]` (held only while stereo renders).
`r.BloomQuality` is left alone by every profile.

Live: `graphics profile <name>` (dev pipe) holds the profile's console variables through
`cvar::hold_in_stereo` (saved and put back when stereo stops, like `[stereo_cvars]`), and
sets foveation and render scale through `fov preset` and `dynres scale`. A variable the
previous profile held (live, or filled at start-up) and the new one does not list is
released: its saved game value is put back at once (`cvar::release_in_stereo`). The live
command overrides explicit ini keys for the session. `graphics status` names the last
profile applied live.

Measured (`tools/bench/vr-perf-steps/profiles.ps1` through `vr-perf-session.ps1`, Null
backend at 3072x3264 per eye without pacing, the player's ini plus `r.BloomQuality 0`, the
latest save in the slums street, standing, profiles switched live in one session; captures
for each profile beside `results.json` in the run's folder under `captures/perf/`):

| Window | Frame p50 / p95 (ms) | Scene GPU p50 (ms) |
|---|---|---|
| custom (the ini: foveation performance, LOD lines) | 8.43 / 14.39 (first window, start-up spikes) | 7.01 |
| quality | 8.86 / 9.95; repeat 8.85 / 10.20 | 7.68, 7.67 |
| balanced | 7.91 / 9.14; repeat 8.10 / 11.44 | 6.77, 6.79 |
| performance | 6.38 / 7.37 | 5.42 |

Checked with `cvar get` after each switch: performance sets `r.VolumetricFog` 0,
cascades 3, `r.StaticMeshLODDistanceScale` 1; switching back to balanced puts the fog back
to 1, and then to quality the cascades back to 5 (the game's value). The start-up path is
covered by `ff7vr_core_tests` (explicit key wins, case-insensitive names, custom applies
nothing); a start with a profile set in the ini was not run in the game.

## Flat-screen camera effects

`[stereo] comfort_cvars = 1` (default) holds these console variables while the engine
renders in stereo (saved when stereo starts, put back when it stops, the same mechanism as
`[stereo_cvars]`; an entry of the same name in `[stereo_cvars]` replaces the built-in value):

| Effect | What was done | Why |
|---|---|---|
| camera motion blur | `r.MotionBlurQuality = 0` | in a headset it smears the image while the view turns, and the head's own motion is not part of it. Costs nothing to remove (`r.MotionBlurQuality` in "Measured"). The game runs with 4 |
| chromatic aberration | `r.SceneColorFringeQuality = 0` | an imitation of lens fringing around the image centre; each eye's image centre is not its lens centre (asymmetric projection) and the headset's lenses add their own. The game runs with 1 |
| depth of field | not changed (`r.DepthOfFieldQuality` stays 2) | no depth of field was seen in gameplay in the scenes tested; in cutscenes it is part of the authored look. Candidate: hold it at 0 only while the follow camera applies (the camera-mode signal), once a scene with gameplay depth of field has been seen |
| camera shake | not a console variable | shakes reach the eyes through the game camera's point of view: decoupled pitch drops their pitch and roll, the level boom their up-and-down motion along the boom, and first person uses only the camera's yaw; the yaw part and the sideways part remain in third person |
| film grain | not changed | `r.Tonemapper.GrainQuantization 1` is the tonemapper's dithering against banding, not visible grain; grain intensity is a post-process setting without a console variable |
| vignette | not changed | it is part of the tonemapper (`r.Tonemapper.Quality 5`); lowering the quality also changes other parts of the look. A community mod removes it for flat play, so it is a matter of taste rather than a VR problem |

Verified in the street (`captures/camera/runD`, `cvar get` through the dev pipe): before
stereo `r.MotionBlurQuality` 4 and `r.SceneColorFringeQuality` 1 (set by the engine's
defaults, priority `0x0`); while stereo renders both 0 (console priority `0x9000000`); after
`stereo off` 4 and 1 again. The values are put back with console priority, so a later change
of the same variable by the game at a lower priority (for example its options menu, if it
uses one) is ignored until the game restarts; this is how every `[stereo_cvars]` entry
behaves. Whether the blur is visibly gone was not checked with a capture in motion: a still
capture after a mouse move does not show it reliably.

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

At 3072x3264 per eye (Null backend, street, fix off and on with `r.BloomQuality 5`): the fix
is applied once per stereo frame (`applied` = `queued`, `missed 0`), and switching it off adds
a blend of the left eye's image to the right eye (correlation of the right eye's change with
the left image 0.51, estimated blend weight 0.12; the left eye changes by capture noise only):
`captures/stereo/runZ/c06_bloom_fix_*.png`, `c07_bloom_nofix_*.png`.

## Right-eye ambient occlusion fix

Square Enix's screen-space ambient occlusion has the same fault as its bloom. Per view it runs
a full-size setup pass at the view's rectangle, then three half-size passes with the view at
the origin of their targets, then a full-size resolve back at the view's rectangle. The first
half-size pass reads the setup texture relative to the origin, so for the right view it
computed the occlusion from the left view's setup; the resolve then combined that with the
right view's own depth and normals. The right eye's occlusion buffer was a double image: its
own objects plus a dark copy of everything near the camera at the left eye's image positions
(with the Quest 3 projection the two eyes' images are about 750 px apart at 3072 px width, so
the copy sits "about a metre" to the side of a nearby object). The occlusion darkens the
ambient and reflected light, so the copy is strongest in shade, where that light is all there
is; in direct sunlight it is faint. `ShowFlag.AmbientOcclusion 0` does not switch this
occlusion off. Details: `docs/re/engine.md`, section 10.

`src/engine/src/bloom_fix.cpp` (`ao_fix`): the pass is recognised on the RHI thread from the
draw sequence, without an address: a full-screen draw (one triangle) at the origin, at most
half as wide as the previous full-screen draw, whose shader resource 0 is the target that
previous draw wrote at a rectangle not starting at the origin. That draw runs with a scratch
copy of the rectangle at the origin, the same mechanism as the bloom fix (the two share the
scratch textures). Stock Unreal passes keep each view at its own rectangle in every
intermediate target, so they never match. `stereo aofix` shows `applied` growing by one per
stereo frame and `failed 0`.

Cost: one copy of the eye's rectangle of the setup texture per frame (3072x3264 RGBA16F at the
Quest 3 size, about 80 MB of copy traffic); no extra video memory when the bloom fix's scratch
texture has the same size and format (it does in the scenes tested), otherwise one more
texture of the scene buffer's size.

Evidence (Null backend, eyes 3072x3264, street next to the sandwich board, one-frame GPU
traces): before, the right view's first half-size pass was numerically the left view's (mean
difference 0.33 of 255, `captures/stereo/runX/tr1` events 2700 and 2705) and its occlusion
buffer showed both eyes' objects (`captures/stereo/runX/tr1_02708.rgba`, right half); with the
fix the two views' first passes differ by the eyes' parallax (mean difference 8.3,
`captures/stereo/runZ/tr2` events 2969 and 2974) and the right half of the occlusion buffer
holds only the right eye's objects (`tr2_02977.rgba`). Final images with the fix on and off:
`captures/stereo/runZ/c01_fix_*`, `c02_nofix_*` (board ahead), `c04_off_fix_*`,
`c05_off_nofix_*` (board off-centre), `d01_fix_*`, `d02_nofix_*`, `d03_fix_*` (camera turned
towards the stall, roughly the direction of the headset session). These spots are sunlit, and
there the difference the fix makes in the final image is small, at the level of the
animation between two captures; the ghost in the headset session was seen in shade. The left
eye is the same with the fix on and off (differences at the level of two captures without
any change).

## Right-eye distortion fix

Square Enix's distortion (heat haze over fire and hot air, refraction through glass) ends with
a composite into the scene colour, once per view. Its draw (UE's `DrawRectangle`) is given the
view's size as the target size, while its viewport is the whole double-wide scene colour
target. In mono the two are the same. In stereo the left view's rectangle is stretched over
both eyes (its pixel shader ran for all 4128x2208 pixels at eyes 2064x2208) and the right
view's rectangle lands at clip x 1 to 3, outside the viewport (no pixels). So whenever
distortion renders, the right eye shows the left view's composite stretched over it, and its
own distortion never appears. Details: `docs/re/engine.md`, section 10, "Distortion". With
the composite made visible (below) the right eye is entirely horizontal blurry lines, as
reported from the headset for a right eye near a real-time fire. No fire or heat haze was
reachable in the save used for testing, so it was reproduced with the pipeline forced.

`src/engine/src/distortion_fix.cpp`: a hook on the composite function (render thread, signature
"Distortion composite") appends an RHI command with the view's rectangle (`FViewInfo+0x70`).
On the RHI thread the next `DrawIndexed` is checked (3 or 6 indices, render target of the scene
buffer's size, viewport covering the whole target) and drawn with the viewport and scissor at
the view's rectangle; for a view that does not start at the origin the vertex shader's
`cb0` is replaced by a constant buffer of the same size holding the `DrawRectangle`
parameters without the position bias (`PosScaleBias = w h 0 0`, `UVScaleBias = w h x y`,
`InvTargetSizeAndTextureSize = 1/w 1/h 1/buffer`), so the view's rectangle maps onto its own
viewport. The engine's buffers are not changed and the state is put back after the draw.
Nothing happens while no distortion renders (the composite function is not called).
`stereo distortfix [0|1]` switches it and shows its counters, including the number of pixels
the composite shaded per eye (pipeline statistics queries, also with the fix off).
`stereo distortfix opaque 1` (test only) draws the composite without blending, so its output
is visible even when there is nothing to distort.

Cost: none without distortion; with it, one small constant buffer update per frame.

Evidence (Null backend, eyes 2064x2208, no distortion in the scene, the pipeline forced with
`re poke 2202f62 90 90 90 90 90 90` and `re poke 22034a2 90 90 90 90 90 90`;
`captures/fire/r3/session.txt`):

| | Left composite shaded | Right composite shaded |
|---|---|---|
| fix off | 9 114 624 px (4128x2208, both eyes) | 0 |
| fix on | 4 557 312 px (2064x2208) | 4 557 312 px (2064x2208) |
| fix off, `dynres scale 0.65` | 9 114 624 px | 0 |
| fix on, `dynres scale 0.65` | 1 924 608 px (1344x1432) | 1 924 608 px |

Shape mismatches 0, failures 0 over 2988 corrected composites; before forcing, the composite
never ran (`composites 0`), so the fix is idle in scenes without distortion. Both eyes'
pictures with the fix on and off differ by capture noise only (mean 1.0 to 2.3 of 255, the
same in both eyes): `captures/fire/r3/*.png`. With the composite's output made visible
(`stereo distortfix opaque 1`, `captures/fire/r4`, side by side in `*_LR_small.png`): fix off,
the right eye is entirely horizontal blurry lines (`b_opaque_off_R.png`, and at
`dynres scale 0.65` `d_opaque65_off_R.png`); fix on, both eyes show their own blurred scene
with the normal parallax (`c_opaque_on_*.png`, `e_opaque65_on_*.png`). Not yet seen with real
heat haze in a headset; `r.DisableDistortion 1` (under `[stereo_cvars]`) switches the whole
pipeline off as a fallback.

## Skin lighting fix

Indoors, exposed skin on characters (faces, arms, legs: the subsurface materials) was covered
in white, square blocks in the eye images. Outdoors it was not seen (also not with the
fix off, checked in the street). The same pass also left the bottom third of every eye
without some of the lights: below row 2160 (the height of a
3840x2160 screen) the lamp light on the counter of the first room stopped at a hard
horizontal line (`captures/skin/r3/row2160_L_off_on.png`; mean brightness step at that row
-21 and -15 levels in the left and right eye with the fix off, -1 with it on, the same as
between two neighbouring rows elsewhere). With the light sort-key patch
(`docs/re/engine.md` section 6) both are gone in both eyes. At eyes 2064x2208, the size
used in earlier tests, only the last 48 rows were affected, which is why the line was not
noticed then.

What happens (one-frame GPU traces of the first room, `gpu trace`): the lights that get
Square Enix's sort-key bit `0x40` are rendered by a tiled lighting compute pass, one
`Dispatch 192 204 1` per view into `SceneColorTiled` (16x16 pixel groups covering a
3072x3264 eye), each after a one-group dispatch that writes a 60-byte buffer, with two
unordered-access buffers of fixed size (691200 and 2764800 bytes) bound. With the patch
those lights also get bit `0x20`, so they leave that group: the tiled dispatches are gone
from the frame and the same lights are drawn one at a time as light volumes per eye
(`DrawIndexed 2376` at each eye's viewport, 7 for the left eye and 8 for the right in the
traced frame). The edges of the white blocks lie on a 16-pixel grid in eye coordinates
(edge positions modulo 16 cluster at 0, 15 and 1 in both eyes), the tile size of that pass.
The read-back of `SceneColorTiled` after the tiled pass (`captures/skin/r1/tr1_02701.png`,
taken after the next light's draw, event 2701; the dispatches are 2680 and 2682, their own
read-backs failed) already holds the white blocks on the right eye's characters, and both
eyes are black from row 2160 down, although the dispatch covers 3264 rows. So the pass is
limited to a 3840x2160 screen somewhere (its fixed-size buffers or the shader); which part
produces the blocks on subsurface pixels was not established. The same patch is the
community fix for screens that are not 16:9.

In the street outside the tiled pass runs too (`captures/skin/r5/tr_street_lf0`, events
2928 and 2930) but nothing of the fault shows: no near-white pixels on the characters, no
step at row 2160, and the fix on and off differ less (mean 1.1 of 255) than two captures
with the fix on (1.7 to 2.1; `r5/g01`-`g03`). Why it only shows indoors is not known;
presumably the lights of that group outdoors are weak or do not reach the characters and
the lower part of the view.

`src/engine/src/fixes.cpp` (`set_light_fix`, `light_fix_stereo`): with `[stereo] light_fix =
1` (default) the byte is patched when the engine starts rendering in stereo and put back
when it renders mono again (the virtual screen, menus, loading screens, stereo switched
off), so the flat game runs the game's own code. `stereo lightfix 0|1` switches the
setting; the log shows `fixes: light sort-key patch on` / `off (game default)` at every
change.

Cost: in the first room the fix makes the frame slightly faster. Null backend unpaced,
`t.MaxFPS 0`, `r.BloomQuality 5`, first person facing the door, 6 s windows alternating:
fix on 10.23, 9.90, 9.95 ms; fix off 10.49, 10.39 ms (average frame time from `stereo
status` after `stereo frametime 6`). A scene with many lights of that group may behave
differently; not measured.

Evidence (Null backend, eyes 3072x3264 with the Quest 3 class asymmetric FOV, foveation
`quality`, `r.BloomQuality 0`, the player's ini; first room of the latest save, Cloud and
Tifa in view; `captures/skin/`):

| Captures | Light fix | Result |
|---|---|---|
| `r1/a01_third`, `b01_fovoff`, `b02_aofix0`, `b04_bloomfix0`, `b05_base_again` | off | right eye: white blocks on Cloud's arm and on Tifa's face, arms, belly and legs; left eye clean (third person). Foveation off, the occlusion fix off or the bloom fix off do not change it (`r1/sheet_bisect.png`) |
| `r1/b03_lightfix1` | on | both eyes clean |
| `r1/b06_swap` (`stereo swap 1`) | off | the blocks move to the eye rendered into the right half of the target |
| `r2/c01`-`c04` (third person), `c05`-`c07` (first person) | off, on, off, on / off, on, off | blocks with the fix off every time, none with it on; in first person both eyes have blocks with it off (`r2/sheet_third_lf.png`, `sheet_first_lf.png`) |
| `r3/d01`-`d06` (the default build) | on by default, off, on; first person on, off, on after `stereo off` and `stereo on` | blocks only with `stereo lightfix 0` (`r3/sheet_third_fix_nofix_fix.png`, `sheet_first_fix_nofix_fix.png`); near-white pixels in Tifa's region: 83815 with the fix off, 3404 with it on (her white top) |
| traces `r2/tr_lf0`, `r2/tr_lf1` | off / on | the tiled dispatches (events 2708, 2710) only with the fix off; per-light volumes (2702-2716 and later) only with it on |
| row 2160 in `r1/a01`, `r3/d02` / `r3/d01`, `d04`, `r2/c06` | off / on | mean brightness step from row 2150-2159 to 2160-2169: -21 (left) and -15 (right) with the fix off, 81 to 91 % of the columns darker by more than 3 levels; -0.6 to -1.3 with it on (rows 2130-2149: -0.3 to -2.6). First person facing the floor (`r2/c05`, off): -3 and -4 |

The occlusion fix kept working with the light fix on (`stereo aofix` in run `r3`: `applied`
2670, then 3219 after a `stereo off` / `stereo on`, `failed 0`; whether it is still exactly
once per frame was not counted in that run). The bloom fix was idle in those runs
(`r.BloomQuality 0`), so switching it off in `b04` changed nothing either. In a later run
with `r.BloomQuality 5` both fixes applied once per stereo frame (to within one: the
counters are read one after the other) with the light fix on, off and on again (5 s each:
bloom fix `applied` +557, +523, +546, occlusion fix +556, +523, +546, stereo frames +556,
+523, +546; `missed 0`, `failed 0`). The flat game (`stereo off`) with the patch removed
looks as before (`r3/flat_after_off.png`). Not confirmed in a headset yet.

## Reflections per eye

Square Enix's screen-space reflections run once per view, each time as one full-screen
triangle over the whole side-by-side target (`DrawIndexed 3`, viewport 6144x3264 at 2 x
3072x3264, into an `R16G16B16A16_FLOAT` target, reading the view's hierarchical depth
(`R16_FLOAT` with mips), velocity, GBuffers and the previous frame's colour). The next pass of
that view (the reflection composite, at the view's own viewport) reads only its half; the next
view's run then overwrites the whole target. So half of each run is computed and thrown away.
After the scene's lighting, the whole scene colour is also copied once per view
(`CopyResource`, 6144x3264 RGBA16F, about 0.17 ms each) into the texture that view's next
reflection run reads as the previous frame's colour.

`src/engine/src/fixes.cpp` (`ssr_draw`, `ssr_copy`; `[stereo] ssr_per_eye = 1`, default):
on the RHI thread, a draw of that shape (one triangle over a whole R16G16B16A16 target at
least 1.5 times as wide as high, no depth, a hierarchical depth texture among the pixel
shader's inputs) is run with a scissor rectangle on its view's half: the first such draw of a
frame on the left half, the second on the right (`stereo swap` reverses this), any further one
untouched. A `CopyResource` whose destination one of this frame's reflection runs read (a
texture of the scene colour's size and format) copies only that view's half. Viewports,
shaders and inputs are unchanged, so every pixel that is computed is computed as before.

Evidence (Null backend, 2 x 3072x3264, `captures/perf/`): the two runs and two copies are seen
once each per stereo frame (`ssr`: runs limited 16044 and copies halved 16044 in 8022 frames,
no extra runs) at the start of the latest save and in the street. Because two captures of the
game a few frames apart already differ a lot (camera sway, animation), equality was tested with
`ssr poison`: filling every skipped half with a bright magenta (value 50) adds no magenta pixel
to either eye (start and street, render scale 1 and 0.8: 0 to 18 magenta pixels, the same as
with the fix alone, none in the 64 columns next to the eye boundary), while filling the halves
the fix does compute shows it (reflection output: 37-50 % of the pixels magenta in both eyes;
colour copies: 120 000 pixels in the left eye). Runs `004408-poison2`, `004901-copy`.

Cost (frame time, frame cap lifted, no pacing, ssr per eye off / on in turns, 5 s windows):

| 2 x 3072x3264 | off | on | |
|---|---|---|---|
| start of the latest save | 9.30-9.39 ms | 8.98 ms | -0.37 ms |
| alley after walking out | 7.85-7.91 ms | 7.32-7.37 ms | -0.54 ms |
| open street | 9.57-9.65 ms | 9.13-9.23 ms | -0.43 ms |
| turning in the street | 10.0-10.2 ms | 9.2-9.3 ms | -0.85 ms |

At 2 x 3600x3600 the same views give -0.47, -0.67, -0.55 and -1.15 ms (table under
"Measured").

**The right eye has no screen-space reflections at all (a fault of the game's pass in
stereo, independent of this change).** The colour-copy control above showed nothing in the
right eye, so both runs were read back with per-eye reflections off (`gpu trace <prefix> dump
fullscreen scale 4`, 2 x 3072x3264, start of the save and the street; run
`captures/perf/20261006-012224-ssrcheck-3072x3264`, sheets `tr_ssr_sheet.png`,
`trs_ssr_sheet.png`). After the left view's run (event 2656) the target holds reflections in
both halves (left half: mean colour 1.73 of 255, alpha 8.7; right half 7.14 / 24.8). After the
right view's run (2658) the right half is exactly 0 in every channel and the left half holds a
shifted image of the right view's reflections (1.93 / 9.9); the right eye's composite (2659,
viewport at x 3072) reads the right half, so it adds nothing. The same in the street (3471 /
3473: right half 0.0 after the right run). So the right view's reflection pass works at the
origin of the target instead of at its view rect, the fourth pass of Square Enix's with this
flaw after bloom, ambient occlusion and the UI composite. Visible as reflections (wet floors,
metal, glass) present in the left eye and missing in the right. Fixed by `ssr_fix` (next
section). The left half after the right run holds the right view's reflections at the right
view's own image positions (not a shifted copy: compared with the right eye's capture, Tifa
sits at 0.42 of the half's width in the read-back and 0.40 in the eye image; at 0.58/0.59 in
the left view), so the pass reads its inputs at the view's rectangle and only the output is
placed relative to the origin. The draw's vertex constants (`DrawRectangle`: position size
6144x3264 at 0,0, UV size 3072x3264 at 3072,0) are those of a stock full-target draw.

### Right-eye reflections fix

`src/engine/src/fixes.cpp` (`ssr_draw_shifted`, `ssr_before_draw`; `[stereo] ssr_fix = 1`,
the default from 06/10 to 08/10, now `2`, see "The right eye's reflections are not its own";
`ssr fix 0|1|2`): the reflection run of the view in the right half (the second run of
a frame; with `stereo swap` the first) is drawn into a scratch render target of the same
format, half the target's width plus 16 columns; the scratch target clips the full-target
triangle, so only the part at the origin is computed. The result is then copied into the
target at the x where the right view's rectangle starts. That x is half the width at full
resolution; with `r.ScreenPercentage` below 100 the engine rounds the scaled rectangles (a
4116-wide target at 67 % has the right view 2059 wide at x 2060; at 58 % 3564 wide, x 1784),
so the x is taken from the viewport of the next draw that reads the reflections (the right
view's composite) and remembered: a frame copies at the remembered x right after the run and,
if the composite starts elsewhere, again at the composite's x before it draws (`moved` in
`ssr`; once per change of the percentage). Viewport, scissor (an engine scissor is shifted with
the output), shaders and inputs are the engine's. It works with `ssr_per_eye` on or off; the
per-eye scissor and colour copy reach 16 columns past the middle so that a rounded left view
that ends a pixel or two past it is still covered.

Evidence (Null backend, 2 x 3072x3264, `r.BloomQuality 0` unless noted, start of the latest
save and the street; runs `captures/render2/013535-ssrfix`, `captures/render2/014336-scales`):

| Check | Result |
|---|---|
| read-back after the right run, fix on / off (`tr_start_fix1` event 2612 / `tr_start_fix0` 2660; street 3288 / 3315) | right half mean colour 1.87, alpha 9.4 / exactly 0 (street 2.21, 6.7 / 0); the left half (left view, 1.64 / 8.5) is the same before and after the right run. Sheet `013535-ssrfix/sheet_rb_start_fix1.png`: left view's and right view's reflections side by side, the right one matching the right eye's image |
| `ssr poison 1` (the parts no run or copy writes filled with magenta) | 0 to 26 magenta pixels per eye at 100 %, 67 %, 58 %, render scale 0.9 and 0.8, the same as with no poison (0 to 123) |
| `ssr poison 2` (each run's own part, right view: after the fix's copy) | 72 to 75 % magenta in both eyes at every scale: the right eye's composite reads what the fix placed |
| `ssr poison 3` (the half of the previous frame's colour each copy writes) | 1.4 to 2.0 % magenta in BOTH eyes (before the fix: left eye only); so the right view's reflections now come through and read the right half of their colour copy |
| counters, 5 s windows with `r.BloomQuality 5` | `ssr`: `applied` = stereo frames, `failed 0`, `not read 0`; x 3072 at 100 % and render scale 0.9 / 0.8, 2060 at 67 %, 1784 at 58 % |
| read-backs at reduced rectangles (`014336-scales/tr_sp67`, `tr_sp58`, `tr_scale0.9`, `tr_scale0.8`) | after the right run, the right view's rectangle holds reflections (mean colour 1.6 to 2.2, alpha 8.8 to 9.5) |

In the final eye images the difference is small in these two scenes (the reflections are
weak and the camera sways between captures); `013535-ssrfix/crop_start_R_fix0_left_fix1_right.png`
shows the metal wall above the door in the right eye with the fix off (left column) and on
(right column).

Cost: the right view's reflections are now really computed (before, that run produced zeros
almost for free) plus one copy of a half (3072x3264 RGBA16F, about 80 MB each way) and a
scratch target of 3088x3264 RGBA16F (81 MB of video memory, released after about 900 stereo
frames without a reflection run). Street, frame cap lifted, 5 s windows alternating: fix off
9.20, 9.22, 9.33 ms; on 9.46, 9.46, 9.65 ms (scene GPU p50 8.01, 7.97, 8.05 against 8.24,
8.22, 8.31): about +0.25 ms, roughly the cost of the left view's own run.

### The right eye's reflections are not its own (08/10)

Reported in the headset: some reflections (a puddle) show in one eye only, and many
reflections of lights show in the right eye only. Checked headless at the layout of the
DLSS package (`[dlss] enabled = 1`, `input_scale = 0.65`, `output = runtime`, Null
backend eyes 3436x3468, so the engine's eye target is 2 x 2232x2256 = 4464x2256;
foveation `performance` with `dlss_finer`), first person in a street with a puddle
(pawn at -52075, 20198, 743), runs `captures/refl/r1` to `r5`.

**The fixes apply at this layout.** Over 5 s (`r1/run.txt`): `ssr` runs limited 2 per
stereo frame, right-eye fix `applied` = stereo frames, `failed 0`, `not read 0`, at x 2232
(half the target, the right view's rectangle); bloom fix and occlusion fix `applied` =
stereo frames, `missed 0`, `failed 0`. Poison tests at this layout: `ssr poison 1` 0
magenta pixels in either eye, `2` 44 % / 42 % (left / right), `3` 0.10 % / 0.16 %
(`r1/p*_L.png`, `p*_R.png`): every part a run or copy skips stays unread and each
composite reads what its run wrote, as at the standard layout.

**But what the right view's run computes is wrong.** One-frame traces with read-backs of
the reflection target (`r1/tr1`, events 4550 / 4552) give, for the left view's part after
its run, mean colour 0.42 and 3.9 % of the pixels non-zero; for the right view's part
after the fixed run 1.23 and 18.9 %. Overlaid on each view's scene colour
(`r1/overlay_fix1.png`, reflections in red): the left view's reflections sit on the puddle
and along the wall edges and grating; the right view's are a scatter of specks over the
whole floor, a dense band at the left edge of the view, and almost nothing on the puddle.
The same holds in the old standard-layout trace (2 x 3072x3264, first room,
`captures/render2/014336-scales/tr_sp100` events 2630 / 2632; `captures/refl/overlay_sp100.png`):
left view on the door frame, beams and table edges; right view on bright bars of wall
that are not edges, and floor specks. So the earlier check ("the right view's result
holds the right eye's objects") was too coarse: objects are at the right positions, but
where the pass finds reflections is wrong. In the eye images the right eye gets
reflections of lights on the floor that the left eye does not have, and loses the
puddle's own screen-space reflection. Without the fix (`ssr_fix = 0`) it has none at all.

What decides it, from tests in the running game (each a trace of the same frame layout):

| Test | Right view's part after its run |
|---|---|
| fixed run as shipped (`r3/tr_s0`, `r4/tr_uv0` baseline) | 17.4-18.9 % non-zero, specks |
| `stereo swap 1` (the left eye rendered into the right half, `r2/tr_swap_m1`) | the eye in the right half (now the left eye) has the specks (20.3 %); the eye at the origin (now the right eye) is clean (3.6 %): **the fault follows the position in the target, not the view** |
| the run drawn in place with its viewport moved to x 2232 (`r2/tr_m2`) | exactly 0: the pass treats its pixel position as relative to the origin |
| inputs replaced by copies holding the right view's part at the origin only (`r3/tr_s*`): depth (t4), GBuffers (t1-t3), previous colours (t7, t8) | 0 (colour 0 with t7/t8, alpha unchanged): all of these are read at the right view's rectangle, correctly |
| same for velocity (t6) | unchanged: not read, or read correctly |
| copies holding the right view's part at the origin and at its rectangle, all of t1-t4 and t6-t8 (`r4/tr_dup`) | unchanged (18.4 %): none of the full-size inputs is read at the origin |
| vertex constants with the texture rectangle at x 0 instead of 2232 (`r4/tr_uv0`) | unchanged (18.6 %): the interpolated texture coordinate does not matter |

Left: the hierarchical depth (t5, built per view over the whole 4096x2048 texture from the
view's own rectangle; its mip-0 constants are correct for both views), the per-view view
buffer (cb1) and the pass's own constants (cb0). The pass's cb0 is the same for both runs
except one value (row 20 w: a finite number for the first run, `3.4e38` for the second; it
follows the run order, not the position, so it is not the cause) and holds a 4x4 matrix
(rows 28-31) that is identical for both views. The likeliest cause is the ray march's
mapping from the view's screen position to the hierarchical depth texture: correct only
for a view whose rectangle starts at the origin. Not established; a fix needs the pass's
pixel shader (disassembly of its bytecode) or the view buffer's rectangle fields patched
for the right view's run.

`[stereo] ssr_fix = 2` (`ssr fix 2`) removes the inconsistency instead of fixing it: both
views' reflection runs are replaced by a clear of the target, so neither eye has
screen-space reflections (wet floors, puddles and metal keep their reflection captures and
light highlights; by the binding audit of the traces the reflection composite reads per-view light grids,
reflection capture buffers and view constants, not checked with read-backs). `2` is the default
since 08/10: the eyes match, which the headset reports were about; `1` (the right eye's own but
wrong reflections) and `0` (left eye only) remain for comparison, and a real fix is the shader-level
task described under "Next". The ini accepts `0`, `1`, `2` and the words `off` / `on`.
Checked at the DLSS layout (`r5`): after both runs both views' parts of the reflection target
are exactly 0 (`tr_f2`, events 4692 / 4694); `runs cleared` grows by two per stereo frame;
frame time unpaced (`xr.null_pace = 0`, `t.MaxFPS 0`), 5 s windows alternating: `2` 8.22 and
8.25 ms, `1` 8.43 and 8.45 ms, so about 0.2 ms less.

### Bloom and occlusion fixes at reduced view rectangles

With `r.ScreenPercentage` below 100 the right view's rectangle can overhang the scene buffer
by 1 to 3 pixels (67 %: 2059 wide at x 2060 in a 4116-wide buffer); the bloom and occlusion
fixes rejected such a rectangle and the right eye got the left eye's bloom and occlusion
again on most frames. `draw_with_shifted_input` (`bloom_fix.cpp`) now copies the part of the
rectangle that lies inside the buffer. Counters over 5 s with `r.BloomQuality 5`
(`014336-scales/counters.txt`): at 100 %, 67 %, 58 % and render scale 0.9 and 0.8, bloom fix
`applied` and occlusion fix `applied` grow by the number of stereo frames to within one (the
counters are read one after the other: +526 at 100 %, +602 at 67 %, +601 at 58 %, +600/+601 at
0.9 and 0.8), `missed 0`, `failed 0`. The build before the change (`c1a6f44`), same spot and
windows (`captures/render2/030051-before`): at 100 % and at render scale 0.8 both fixes applied
on every frame (+552, +600), at 67 % on none (bloom `missed` +537, occlusion `failed` +537 in
537 frames). The render scale path was not affected (its right view starts exactly at the
middle and fits the buffer). Right eye at 67 % with all three fixes off and on:
`014336-scales/sheet_sp67_R_allfix0_allfix1_L.png` (off: the orange streak and the doorway's
copy from the left eye's bloom; on: gone).

## ReShade and Luma: the tonemapping shift and Luma's DLSS

With ReShade and the Luma add-on installed as the game's `dxgi.dll` (start-up with it:
`docs/render.md`, "ReShade and Luma"), two of Luma's behaviours break the right eye. Both are
handled while stereo renders, so a player can leave Luma in place.

**Luma's replacement for the bloom combine ("Apply Bloom", game pixel shader `4D6F937E`, with
Luma's vertex shader `6667BA2`) reads its input relative to the origin.** Without a
correction both eyes show the left eye's image. `[stereo] tonemap_shift` (`auto` by default:
only while a module named `Luma-Final Fantasy VII Remake.addon` is loaded) runs the right
view's draw with its input 0 copied from the view's rectangle to the origin of a scratch
texture, the bloom fix's mechanism (`bloom_fix.cpp`, `tonemap_shift`; recognised by shape: a
full-screen draw whose viewport starts at the middle of an `R16G16B16A16` target, input 0 of
the target's size, input 1 a quarter to a half of it).

**Luma's own DLSS takes the double-wide target for a 50 % frame.** When Luma's super
resolution is on (`SRUserType` in the `[Luma]` section of `ReShade.ini`; on in the setup
tested: NVIDIA's `nvngx_dlss.dll` is loaded with the mod's own DLSS off), Luma
runs DLSS on the game's anti-aliasing draw. It reads that draw's viewport as the render
resolution and the target's size as the output resolution: in stereo the viewport is one
view, half the target, so Luma takes the frame for 50 % dynamic resolution, upscales the
first view's rectangle to the whole target (one evaluation per frame) and sets its own
constant `DrewUpscaling` (`LumaData.GameData`, constant buffer slot 12) for the rest of the
frame. Its replacement shaders (Apply Bloom and the output pass) then use
`OutputResolution` = the whole target (2 x eye width) in place of the view's size, so every
coordinate of the right view is scaled by two: the right eye showed only its leftmost quarter
(516 of 2064 columns) with the shift on, and a squeezed pair of images with smeared columns
after the middle with the shift off. Fix (`bloom_fix.cpp`, `hook_ngx_for_luma`): once Luma
and NVIDIA's NGX library (`_nvngx.dll`, the driver's) are loaded, inline hooks on NGX's
`NVSDK_NGX_D3D11_CreateFeature` and `NVSDK_NGX_D3D11_EvaluateFeature` refuse the calls whose
return address lies in Luma's module while stereo renders (result `0xBAD00000`, the NGX
failure code). Luma then treats DLSS as failed for that frame and the game's own
anti-aliasing draw runs, as on a card without DLSS; `DrewUpscaling` stays 0. Calls from any
other module (the mod's own DLSS, `docs/dlss.md`) pass; outside stereo (menus in the virtual
screen) Luma's calls pass too. `[stereo] luma_dlss = 1` (dev command `lumadlss 1`) lets
Luma's calls through, for comparison only. Without Luma no hook is installed. Cost: one
refused call per frame; not measured separately (frames stayed paced at 11.1 ms, 90 Hz, in
the runs below).

Found with the GPU trace, which now lists every constant buffer slot and dumps slots 12 and
13 (where Luma binds its constants) for the read-back events: Luma's `LumaData` row 2 holds
`RenderResolution` 2064 2208, row 3 `OutputResolution` 4128 2208, row 4 `ViewportRect`
0 0 4128 2208, row 5 the resolution scale 0.5 2 and `DrewUpscaling`, which turned 1 after
the left view's anti-aliasing draw (`captures/render2/luma/r2/tr.txt`, events 6389 to 6423)
and stays 0 with the calls refused (`r3/tr.txt`). Not a regression of the mod: the build
from the night of 06/10 (`c4ff799`), whose capture with Luma had a complete right eye then,
shows the same strip today in the same spot (`captures/render2/luma/old1`). What changed in
between is outside the mod; the likeliest candidate (not proven) is the NVIDIA App's DLSS
override, set back to the application's choice on 06/10 at midday: before that, NGX accepted
only DLAA input sizes, so Luma's DLSS at the views' sizes probably never ran.

Evidence (Null backend, eyes 2064x2208, the street outside Seventh Heaven, Luma loaded unless
stated; `stat2.py` in the run folder measures each eye's last non-black column, columns
without vertical detail, and the horizontal shift that best matches the right eye to the
left):

| Run | Result |
|---|---|
| `r1` (HEAD before the fix) | right eye: last non-black column 958 of 2064, 1277 columns flat; shift off: whole width but the right half smeared |
| `old1` (`c4ff799`) | the same numbers: not a regression |
| `r3` (fix) | refused: both eyes complete, parallax 368 px (as without Luma), `evaluate calls 501 refused 501` in 501 stereo frames; `lumadlss 1`: the strip again (last column 958); `lumadlss 0`: complete again; shift off with the calls refused: both eyes show the left image (mean difference 1.1), so the shift is still needed |
| `r4dlss` (DLSS build, the mod's DLSS on at `input_scale` 0.75) | both eyes complete, parallax 368 px; the mod's NGX calls pass (`other callers` 2114 after 1056 frames, 0 evaluation failures); Luma made one evaluation call in the whole run |
| `r5noluma` (`dxgi.dll` set aside) | hooks off, shift inactive, both eyes complete with 368 px parallax; image means 45.6/45.8 against 45.8/46.1 with Luma and the calls refused |
| `r6soak`, `r8soak` (5 minutes each, emulated head yaw) | every minute bloom fix, occlusion fix, right-eye reflections fix and tonemapping shift applied once per stereo frame (`r8soak`: 25773 each at the end, `missed 0`, `failed 0`), Luma's evaluations all refused; no warning or error in the log; both eyes complete at the start and the end. Frame times: see below |

**Frame times in the 5-minute runs (open, not caused by the refusal).** In every 5-minute run
with emulated head yaw in which the game's own anti-aliasing pass ran for both views, the GPU
scene time stepped up a few minutes in and kept rising: with Luma and its calls refused
`r6soak` 6.2 -> 22 to 29 ms after 3.0 min, `r8soak` 6.1 -> 19.5 ms after 4.3 min, `r11ab`
6.3 -> 20 to 53 ms after 3.6 min; without Luma `r9noluma` 5.4 -> 10.5 to 12 ms after 3.3 min,
`r14noluma` 5.5 -> 47 to 72 ms after 2.2 min. `nvidia-smi` in `r9noluma`: the same clock and
power (2700 MHz, about 150 W) with the load going from about 50 % to 99 %, like state 3 in
"Video memory and slow phases" (`docs/benchmarking.md`). The two runs with
Luma's DLSS allowed (`r12allowed`, `r13allowed`: Luma's DLSS replaces an anti-aliasing draw,
the right eye broken) stayed at 5.8 ms for 5 minutes. `r7soak` was slow from the loading
screen on, before stereo and before the hooks were installed. Allowing Luma's calls in the
middle of the slow phase (`r11ab`) did not bring the time back down. So the refusal leaves
Luma's frame work like that of a game without Luma, including this slowdown, which is a
separate fault of the standard path to be traced (one-frame GPU trace in the slow phase,
compared with the fast phase).

Not seen: the interior where the save started on 06/10 (the save now starts in the street);
Luma's HDR output path (`Output_HDR`, used when the game's HDR is on); other Luma versions
(tested: the add-on installed in the game folder, 19/06/2026, with ReShade 6.7.1). If a later
Luma takes the view rectangle into account, the shift would double-shift the right eye: set
`tonemap_shift = 0` then.

## Volumes and hierarchical depth per view

Two passes looked like per-view duplicates worth sharing between the eyes. One-frame GPU
traces (`gpu trace`, 2 x 3072x3264) and `fov trace` (GPU time per render target binding)
show otherwise. The trace audit below covers 33 saved traces: first room, street,
`r.ScreenPercentage` 58 to 100, render scale 0.8 and 0.9, DLSS runs, ReShade loaded
(`captures/` `stereo/runK`-`runZ`, `skin/`, `perf/`, `render2/`, `dlss/`, `gaze/`).

### Translucency lighting volume: already shared

The translucency lighting volume (two cascades, each an ambient and a directional
`R16G16B16A16_FLOAT` 64x64x64 volume) is built **once per frame**, not per view: one clear of
all four volumes (one draw, four targets), one injection draw per light into each cascade (small
viewports: the light's bounds in the volume), eight full-volume draws that read the shadow
depth atlas, one filter draw per cascade. All of them use the first view's uniform buffer, so the volume is
placed around the left eye and the right eye reads the same one (6 cm apart, against a volume
several metres across). Every one of the 33 traces has exactly one volume clear. GPU time
(`fov trace`, start of the save): about 0.07 ms in total. Nothing to remove.

What does exist per view is the **volumetric fog**: a 110x117x96 froxel grid per view (about
3072/28 x 3264/28 cells), built for each eye by one dispatch, three draws, a clear, four draws
and two more dispatches; in `fov trace` its volume-target draws take 0.34 ms per eye, plus the
compute work around them. The grid lies in each eye's own frustum, so the
right eye cannot reuse the left eye's grid without misplacing the fog. Its cost is what
`r.VolumetricFog 0` removes ("Measured", -1.0 ms at 2 x 2500x2600); a coarser grid
(`r.VolumetricFog.GridPixelSize`, `GridSizeZ`) would be a quality trade, not measured.

### Hierarchical depth: each view reads only its own half

Per view the engine builds two R16_FLOAT mip chains (the trace's pool names: `HZB`, `IHZB`;
the names swap between pooled textures, the binding order does not): one draw writes mip 0 of
both (two targets), then one draw per further mip and chain (11 each). Mip 0's
`DrawRectangle` constants place the source rectangle at the view's own rectangle (UV size
3072x3264 at x 0 for the left view, at x 3072 for the right one): **the build does not read
the whole double-wide depth**, so there is nothing to limit to a half. The target is 4096x2048
for a 3072x3264 view: its size follows the scene buffer's width (6144 rounded down to a power of
two), where one sized from the view would be 2048x2048, half the pixels. Changing that would
change what every reader samples; not attempted.

Readers, from the audit: the second chain (the second target of the mip-0 draw) is read by
the ambient occlusion (setup, half-size passes, resolve), the screen-space reflections and the
ray traced shadows of its view. The **first chain is read by nothing** in any of the 33
traces, other than its own mip builds (`r.HZBOcclusion` is 0 in this game, so occlusion
culling does not use it either). Its 22 further-mip draws per frame are dead work.

`[stereo] hzb_skip = 1` (`hzb 1`; `fixes.cpp`, `hzb_draw`) leaves those draws out: on the RHI
thread a full-screen draw into two R16_FLOAT mip chains at mip 0 marks the first target as this
view's unread chain, and a later single-target draw into mip 1 or above of that texture is
skipped. Mip 0 is still written (one draw for both chains). It only acts while `r.HZBOcclusion`
is 0 (read on the game thread every 120 frames; with 1 the chain is built as usual and the
log says so). Test modes: `hzb 2` / `hzb 3` fill the whole first chain with near / far depth,
`hzb 4` fills the further mips of the read chain instead (control).

Gain: `fov trace` at the start of the save, the first chain's further mips 0.02 to 0.03 ms per
view; frame time over 5 s windows, off / on alternating: start 9.32 / 9.17 ms (one clean round;
the two others were disturbed by a build running on the same machine), street 9.06 / 9.08 and
9.11 / 8.98 ms (run `captures/perf/20261006-032356-diag1-3072x3264`). That is within the
noise of about 0.1 ms. Off by default: the gain is too small to measure, and a scene with a
reader of the first chain (not seen in the traces) would get stale data. Image tests were not
usable as proof here: two captures of the same view without any change differ by a mean of 10
levels (camera sway and animation), more than any of the test modes (`start_h*` in that run);
the evidence is the binding audit.

## Render scale and dynamic resolution

The game's own dynamic resolution (`r.DynamicRes.*`, active in the player's settings) does
nothing in stereo: with `r.DynamicRes.FrameTimeBudget 6` (operation mode 1 or 2) at
2 x 3600x3600, 11.5 ms per frame, the frame time and image stayed as they were. Lowering
`r.ScreenPercentage` works (75: 11.5 -> 9.0 ms), but this build sizes its scene buffers from
it, so every change reallocates them: toggling between 90 and 100 once a second gave frames of
54 to 138 ms and short drops to the virtual screen, also with
`r.SceneRenderTargetResizeMethod 2`.

`[stereo] render_scale` therefore works through the views instead: each eye renders into the
top-left `scale x scale` part of its half of the eye target (`AdjustViewRect`), the eye target
and the scene buffers keep their size, and the eye rects handed to the XR layer are the
smaller ones, so the runtime receives the image as the projection layer's sub-image and scales
it to the display (one resampling, in the runtime's own distortion pass). A change takes
effect at the next frame and reallocates nothing (`dynres scale 0.9` and `1` alternated once a
second: no frame above 20 ms). Foveation follows the smaller rects (its surface is rebuilt
for the new rects; `docs/render.md`). What temporal anti-aliasing does with its history when
the rect changes was not examined (nothing was seen in still captures; not checked in motion).

`[stereo] dynamic_resolution = 1` adjusts the scale from the GPU time of the stereo frames
(scene start to Present, the foveation module's timestamps, so it needs foveation to be
initialised; without it the scale stays at `render_scale`). Every 10 measured frames: if
their median is above the budget (`dynamic_resolution_target` x the display's frame
period), the scale drops at once to the estimate that fits (pixel count proportional, at most
0.10 per step); if it stays below 90 % of the budget for 60 frames, it rises by at most 0.04.
Steps of 0.02, between `dynamic_resolution_min` and `render_scale`. The 20 frames after a
change are not counted: the engine reallocates buffers sized by the view rect then, and those
frames take up to 30 ms of GPU time (with the average instead of the median and 4 frames
skipped, the scale oscillated between 0.75 and 0.82 on exactly those frames). The log has one
line per change (`dynres: scale 0.92 -> 0.86 (GPU median ... ms of 10 frames, budget ...
ms)`). The GPU median is also kept while the scale is fixed: `dynres` shows it, which gives
the game's GPU frame time in a headset session.

Measured (2 x 3600x3600, start of the latest save, Null backend unpaced; frame time / GPU time
of the scene):

| render scale | frame | scene GPU | eye pixels |
|---|---|---|---|
| 1.00 | 11.53-11.61 ms | 10.07-10.11 ms | 3600x3600 |
| 0.90 | 10.16 ms | 8.83 ms | 3240x3240 |
| 0.80 | 8.63 ms | 7.70 ms | 2880x2880 |
| 0.75 | 8.05 ms | 7.23 ms | 2704x2704 |

With the median and the 20 skipped frames (run `captures/perf/20261006-010653-scale-3600x3600`,
start of the save, reflections per eye on): at a target of 0.85 x 13.9 ms (72 Hz) = 11.8 ms
the scale stayed at 1.0 (GPU 10.6 ms, nothing to do); at 0.70 x 13.9 = 9.7 ms it went
1.00 -> 0.94 -> 0.92 -> 0.90 within 0.7 s and held (frame 9.37 ms); turning for 5 s moved it
between 0.84 and 0.88 (frame 9.74 ms, p95 12.7), and it settled at 0.86 afterwards (9.56 ms):
8 changes in 25 s, none below 0.84. The first version (average of 10, 4 frames skipped)
oscillated between 0.75 and 0.82 in the same situation. Captures at 0.75
(`captures/perf/002659-scale2-3600x3600/s075_*`): geometry and framing as at 1.0, softer;
nothing at the edges of the sub-image.

Not on by default: it lowers the resolution exactly when the scene is heavy, and how a
resolution change looks in motion (TAA reset, the runtime's scaling) and how the budget should
be set against Virtual Desktop's own GPU work can only be judged in the headset.
`render_scale` below 1 is the same trade as `[xr] resolution_scale`, but without
reallocation and adjustable while playing (`dynres scale`).

Video memory: because the scene buffers keep the full size, a render scale (or `[dlss]
input_scale`) below 1 saves GPU time but no memory. At 4608x4224 per eye with DLSS at 0.58
the game held 12.7 GB, against 8.8 GB for the same input through `r.ScreenPercentage 58`,
and the card filled up (`docs/benchmarking.md`, "Video memory and slow phases").

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
hitch at the start and end of a movie). When a movie ends the log gets
`movie: stopped <path> (after S s, N engine frames, F fps)`: the game's frame rate over the
movie, and every 10 s while it plays `movie: F fps over the last 10.0 s`, marked slow below
30 fps (the movie runs at 59.94 fps; far below that, its frames were slow to reach the card,
see below).

Status: on by default since 08/10. Detection was seen working in a headset session on
08/10 (`MV_TOWN7_2250_US_MediaPlayer_VP9`: `movie: playing` when the movie started,
`movie: stopped` 112 s later, the virtual screen in between, stereo back afterwards). The
movies are WebM files (VP9 video at 1920x1080 and 59.94 fps, Opus audio, read from the file
header of `MV_TOWN7_2250_US.emov`; `.emov` is only the extension): the exe carries the engine's
WebMMedia plugin, so every movie frame is most likely decoded on the CPU and uploaded to the card
(inferred from the plugin name; the decoder was not traced).

### Cutscenes on the virtual screen

`[stereo] cutscene_screen = 1` (default 0) treats a scripted camera shot like a movie.
At the end of every `player::tick` (game thread) `audio_listener::player_frame` hands the
watcher the frame's camera state: the view target counts as an **authored camera** when it
is neither the pawn nor the game's `EndCameraActor` (a `CineCameraActor` or any other
actor; no pawn is needed, since a cutscene may leave the controller without one), plus the
battle flag (`player`'s combat state, which follows the battle signal while
`[first_person] auto_combat = 1`). The watcher (`movie_watch.cpp`, start of the next frame):

- an authored camera outside a battle for `cutscene_screen_delay_ms` (default 500) starts
  a cutscene: `cutscene: authored camera <name> (<class>) for N ms (delay D ms): stereo held off`;
  shorter runs (the opening shot after a load, brief misses) log
  `cutscene: authored camera for N ms only (delay D ms): stays in 3D`;
- the cutscene ends once the view target has been the pawn or `EndCameraActor` (or none)
  for `cutscene_screen_hold_ms` (default 300): `cutscene: follow camera back for N ms ...`;
  a battle or `cutscene_screen = 0` ends it at once;
- one arbitration owns `device::request_active` for movies and cutscenes: stereo goes off
  when either starts (only if it was on) and comes back only when neither holds it; a movie
  that starts during a cutscene takes over (`movie/cutscene: ...` lines), and the cutscene
  state waits while a movie plays.

The scripted moves of the game's own `EndCameraActor` (the camera turned away from the
character) are not detected: in flat mode the camera modes' aim test does not run
(`adjust_camera` is only called for stereo views), so only the view target can tell.
Live: `stereo cutscene on|off|status|delay <ms>|hold <ms>`; `stereo cutscene simulate on|off|<ms>`
treats the view target as an authored camera (test; `<ms>`: for that long).

Status (08/10, Null backend, headless): with the simulated authored camera, a 297 ms run
at the default delay stayed in 3D (`authored camera for 297 ms only`), a 3 s run switched
to the screen after 500 ms and back 313 ms after the camera returned, delay 0 switched at
once, and a movie started during a cutscene took over the switch and released it when it
stopped; a capture while held off showed the flat game on the virtual screen. Frame-time
windows around a switch pair: max 72 to 76 ms (the eye target's reallocation, as for
movies). The game's opening shot after Continue is not detected: it is the game's own
`EndCameraActor` moved by script (the view target never changed in three runs). No real
conversation or cutscene was reached headless.

### Frame rate during movies

His two sessions with a movie (07-08/10, DLSS package at 0.65, Virtual Desktop at 72 Hz):

| Session | `movie_screen` | Before the movie | During the movie | After it |
|---|---|---|---|---|
| A | 0 (movie inside the stereo frame) | 72 fps, then slowing for 50 s before the movie (GPU scene 4 -> 14 ms, Present 13-16 ms) | 22, 12, 12, 22, 2.9 fps | quit |
| B, started about 15 s after A's exit | 1 (virtual screen) | 5-12 fps for the first minute (the slow state after a quick restart), then 72 fps | 12 fps for 110 s | 6-14 fps for about 40 s with GPU scene 45-96 ms, then 72 fps again for 40 s until the session ended |

Session A's windows without any 3D scene (00:07:51 and 00:08:01, GPU scene 0 ms) ran at 11.9 and
12.1 fps with Present at 58 to 61 ms, the same as B's movie windows; its 22.5 fps window still
rendered the scene (GPU scene 13 ms) and its 2.9 fps window had the scene rendering again (GPU
scene 167 ms average). So the movie itself ran at 12 fps in both sessions, with `movie_screen` 0
and 1 alike, and session A was not a quick restart: whatever slows the uploads can also start
during a session (A had been slowing for 50 s before the movie).

Where the time went in session B (10-s windows during the movie, screen mode): the game's
own `IDXGISwapChain::Present` 42-65 ms on average (p95 110-340 ms); the mod's work before it
0.2 ms at the median (its average 1-10 ms comes from a few `xrEndFrame` calls of 100-670 ms in
Virtual Desktop's runtime); the mod's GPU work (the copy to the virtual screen) 0.02-0.06 ms.
The game presents with sync interval 0 and `ALLOW_TEARING` into a two-buffer flip-discard
swap chain, so its Present waits only when the GPU's queue is full: the frame time was GPU
work the game itself had queued while the mod's 3D work was nil, that is the movie frames'
uploads. In the slow state after a quick restart (`docs/benchmarking.md`, slow state 3)
uploads to the card run at 0.03-0.2 GB/s instead of 7-14 GB/s; a 1080p movie frame is 3 MB
(YUV) to 8 MB (RGBA), 15 to 270 ms per frame at those rates: the order of the 80-350 ms
frames seen (inferred; the copy engine's load was not logged by that package).
Session B started about 15 s after A's exit (the package predates the 90-s wait). Stereo rendering
uploads little and recovered to 72 fps inside the state, so the state went unnoticed until
the movie. Session A was not a quick restart; what slowed it before the movie is not known
(that package did not log the copy engine's load). The upload-state explanation is therefore
inferred for both sessions, not proven; the next movie session with the `movie: ... fps` lines
and the `game copy engine N %` figure will tell.

The movie path is not slow by itself: in a later session (normal start) the battle tutorial
players (VP9 as well) were loaded at 13:00:08 and 13:01:34 while stereo held 72.0 fps in every
window (inferred to have played: menu players are not asked `IsPlaying`).

The switch itself, measured without a headset (Null backend, standard build, eye
2064x2208, `stereo movie simulate on` for 36 s, then off): in screen mode the game ran at the
Null runtime's pace (120 fps; Present 0.03 ms, the mod's work 0.02 ms); back to stereo one
66-ms frame, then 90 fps with the GPU scene at 4.1 ms and the copy engine at 0 %. The same
with the DLSS build at his settings (eye 3436x3468, `input_scale` 0.65, `output = runtime`):
120 fps in screen mode, then stereo at 90 fps from the first 10-s window with every frame
upscaled in both eyes (791 of 791, then 903 of 903), GPU scene 4.3-4.4 ms, no failure. So the
30-s slow phase after the movie in session B is not the switch: a movie usually ends with a
new area streaming in (20-40 s of uploads after a load even in the normal state, slow state 2
in `docs/benchmarking.md`), and in the slow state those uploads crawl.

What helps: wait 90 s after quitting before starting again (the launcher and
`ff7vr-start.cmd` now do this); `movie_screen = 1` (now the default): the movie is shown flat on
the virtual screen, the way the game draws it, and stereo is held off while it plays (in the two
sessions the frame rate was the same either way, so this is about the picture, not the speed). In a slow movie,
the timing block's `game copy engine N %` at 40 % or more and the warning after it identify
the slow state: quit, wait a minute and a half, restart.

Not reproduced without a headset: no movie player exists in the levels the dev saves load,
and the mod has no way to load a movie asset; the game without the mod was not measured
during a movie.

## Window modes

| Mode (`GSystemResolution.WindowMode`) | In stereo |
|---|---|
| windowed (2) | correct; the tested configuration |
| windowed fullscreen (1) | **both eyes broken**: Square Enix's renderer checks `WindowMode == 1` in about twenty places (post-processing parameter setup and more) and replaces rectangles with the full screen (`captures/stereo/runS1/g_modes.png`: left eye black, right eye shrunk into a corner) |
| exclusive fullscreen (0) | correct while the window has the focus (`runS1/w03_exclusive_*`); losing focus minimises it and the game stops presenting (the headset then has no new frames); when it is activated again the engine re-requests `GSystemResolution`, which holds the eye target size while stereo renders, as a display mode (`RestoreSystemResolution`, `0x287d8e0`) |

So while VR renders the game runs in a normal window: the first time stereo becomes active
in either fullscreen mode, `r.SetRes` is set to `<vr_window>w` (default 1280x720), and the
game's own value (`<w>x<h>f` or `wf`) is set again when stereo is switched off with
`stereo off`. Temporary drops to mono (loading screens, movies) do not switch back. Verified
from both modes (`captures/stereo/runWin`: `stereo window` shows mode 1 or 0 before, 2
while stereo renders and the old mode after `stereo off`; the eye captures `wb_stereo_*`
and `wx_stereo_*` are correct). The desktop window is only a mirror in VR, and a small one
also costs less to draw.

### What the game remembers

The window mode the mod sets is not saved. The game keeps its display options (display
mode, resolution) in its own options save, `ff7remakedevice.sav` in the Steam save folder
(next to `ff7remakecommon.sav` and the slots; the function at RVA `0xb27700` builds both
names from the strings `device` and `common`, inferred, not traced further). The engine's
`GameUserSettings.ini` is not used: `Saved\Config\WindowsNoEditor` holds only the
player's own `Engine.ini`. `r.SetRes` changes the live `GSystemResolution` and the window,
and the game did not write the options save (or any other file) on any exit below, so the
switched window is not carried over to the next start.

Checked with the game started the way the launcher starts it (`-d3d11` only, no window
switches, so in the saved mode: windowed fullscreen, `fixes: game window 1920x1080wf ->
1280x720w`), the Null backend (stereo is active from the title screen, so the switch
happens there), and every file in the save folder and `Saved\Config` hashed before and
after each run (`captures/window-persist/`, `steps.txt` and `snap-*.txt` per run):

| Run, in this order | How it ended | Files changed | Mode at the next start |
|---|---|---|---|
| A | window closed in 3D (`WM_CLOSE`, as Alt+F4 or the close button): the game's own shutdown, 2 s (`render: ExitProcess: ending the XR session` from the game thread) | none | `1920x1080wf` (run B's log) |
| B | 3D switched off with Insert (`stereo status`: wanted 0, active 0; the virtual screen; window still 1280x720), then window closed: the same shutdown, 2 s | none | `1920x1080wf` (run C's log) |
| C | process killed in 3D (as a crash or Task Manager) | none | see D |
| D | plain game, no mod, Luma in place: window covers the whole monitor (rect -7,-29, 2575x1477 in the 150 % scaled desktop of 2560x1440), capture 3840x2160 | none | — |
| E | mod with `[stereo] enabled = 0`: no switch, title capture 3840x2160 | — | — |

Nothing was written while the switched game ran either (snapshot taken in 3D before each
exit). The only file the game creates is the UE4 crash reporter's
`CrashReportClient.ini` (a new folder on every start, also without the mod). Afterwards
every file of the save backup taken that morning (slots, `ff7remakedevice.sav`,
`ff7remakecommon.sav`, `Engine.ini`) still had the same hash. Closing the package
launcher does not end the game, so it is not an exit of its own. Not covered: the game's
own Quit menu entry (not reachable with scripted input), and changing a setting in the
game's graphics options while the window is switched; the game might then save the
window as the display mode.

The switch also exposed a crash: the desktop mirror kept a view of the back buffer, so any
`ResizeBuffers` of the game while stereo rendered (window mode or size change) failed with
`DXGI_ERROR_INVALID_CALL` and the game terminated. The mirror now makes the view for each
draw.

## Evaluation in play (scripted input)

From the latest save with keyboard input (walking) and relative mouse moves (camera),
Null backend, Quest 3 class FOV. Captures: `captures/stereo/runM` (room, walking out, the
street, the item shop), `runQ`, `runS1` (street), `runWin`.

What was reached: walking and turning in the first room, through the door and along the
street, into the item shop; camera yaw and pitch with the mouse; the commands menu
(Space). Not reached with scripted input: a conversation (no talk prompt appeared with E or
Enter near the shopkeeper), a real-time cutscene, the pause menu (Escape and Tab did
nothing), a loading screen (the shop is part of the street level), combat, a pre-rendered
movie (no `MediaPlayer` object existed on the title screen or in these areas).

| Item | Result |
|---|---|
| Right-eye ghost | fixed, see "Right-eye bloom fix" |
| Eye differences while walking and turning (shadows, lights, reflections, particles, fog, sky, culling) | none found apart from the ghost. An NPC visible at the edge of one eye only (`runM/m04_turned_*`) is outside the other eye's field of view (asymmetric FOV), not culled |
| Light sort-key patch outdoors | no visible difference (`runM/g_lightfix.png`; the differences are idle animation). Indoors it removes white blocks on skin, see "Skin lighting fix" |
| Camera yaw (mouse, stick) | turns the player smoothly, as in the game; artificial smooth rotation can be uncomfortable for some players (no snap turn yet) |
| Camera pitch with decoupled pitch | with `[camera] boom = game` the game moves the camera along its boom with pitch: at -34 degrees (looking down from above) the eyes are 2.4 m higher than at 0 (camera Z 99.7 -> 342.9, `stereo views`), at +10 degrees they sit at counter height (`runM/m05_pitch_down_L.png`, `m06_pitch_up_L.png`). Fixed by the level boom (default), see "Camera modes: evidence" |
| Commands menu | shown on the UI layer, the scene keeps rendering in stereo |
| Window modes | see "Window modes" |
| Frame time | see "Measured" |

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

## Trying the camera in the headset

What to try first, in this order. Throughout: End (View/Back + left stick click) recenters
when the view is not straight ahead, Insert (View/Back + Start) falls back to the virtual
screen when something looks wrong in 3D, and pressed again goes back to 3D or reconnects.


1. **Third person, walking and orbiting.** Walk around and move the right stick (or the
   mouse) up and down. The player's height should stay at the character's shoulder level
   while the view orbits; only the left/right part of the stick turns you. Walk with your
   back to a wall and orbit: the eyes should stay out of the wall.
2. **First person.** Stereo starts in first person outside battles. The view should be at
   Cloud's eye height, facing where the camera faced, with no part of Cloud or his sword
   visible. Walk, turn, toggle with View/Back + right stick click (or Home on the keyboard)
   back and forth a few times: in third person Cloud and his sword must be complete every
   time. A manual toggle holds until the next battle starts or ends.
3. **A battle.** It should switch to third person when the battle starts and back to first
   person when it ends; the log shows `player: battle signal 0 -> 1` and
   `player: battle started: third person`. If no such line appears, the signal is wrong.
4. **A conversation, a cutscene, a loading screen.** The log should show the camera mode
   switching to `game camera` (authored shots) and back afterwards.

If something is wrong, send `ff7vr.log` from the game's `End\Binaries\Win64` folder (it is
rewritten at every start, so copy it before starting the game again) and say roughly when
it happened. The lines that matter start with `player:` (every camera mode change with its
reason, the follow camera on and off, the battle signal, first person toggles, meshes hidden
and shown). Keys to flip in `ff7vr.ini` to narrow it down:

| Symptom | Try |
|---|---|
| third person feels wrong when orbiting, or eyes inside geometry | `[camera] boom = game` (the game's own camera position, the behaviour before) |
| first person at the wrong height or inside the head | `[first_person] eye = offset` and adjust `eye_offset`; or `head_offset` |
| first person in a battle, or third person outside one | `[first_person] battle_signal =` (empty: no automatic switch) and `default = 0` |
| any first-person problem | `[first_person] enabled = 0` switches the whole feature off |
| a cutscene or conversation viewed from the wrong place | `[camera] boom = game`; if that does not fix it, the follow-camera test passed for an authored camera: the log line `player: follow camera on (...)` names the camera |

## Long session (soak) test

The configuration a player gets (`tools/package/ff7vr.ini`: stereo, UI layer, foveation
`quality`, first person by default) with the Null backend (Quest 3 class eyes 2064x2208,
90 Hz) and the dev pipe on, from the latest save, 15.5 minutes of scripted play
(`captures/soak/runN`): 20 loops of walking out and back (rooms, then outdoors in the
slums), turning and pitching with the mouse, Home twice, the command menu and the main
menu opened and closed, End, Insert twice (stereo off for 4 s), idle. Process samples every
30 s (`monitor.csv`: working set, private bytes, handles, threads, GPU memory of the
process from the `GPU Process Memory` counters), frame times from the log's 10 s windows.

| | start (17:38) | middle (17:46) | end (17:53) |
|---|---|---|---|
| working set | 2945 MB | 2773 MB | 2871 MB |
| private bytes | 10783 MB | 10540 MB | 10862 MB |
| GPU memory of the process (dedicated) | 7613 MB | 7529 MB | 7676 MB |
| handles | 2998 | 2986 | 2986 |
| threads | 121 | 115 | 115 |
| frame interval in stereo, 10 s windows (median, p95) | 11.11, 11.11 ms | 11.11, 11.11 ms | 11.11, 11.11 ms |

Over all 31 samples: working set 2769 to 2965 MB, private bytes 9583 to 11064 MB (lower
while stereo was off: the eye target is released), GPU memory 6514 to 7676 MB, handles
2984 to 3003, threads 115 to 121; nothing grows steadily. Every all-stereo 10 s window has
a median and p95 of 11.11 ms (the 90 Hz pacing) from the first to the last minute; the
windows with a stereo switch have their longest frame at 50 to 77 ms (the switch
reallocates the eye target). Two other frames, at minute 13 and 14, took over 100 ms (the
render module's XR thread took over pacing for 30 ms and logged it); both came right when
the test script brought the window to the front and pressed a movement key (60 such
presses in the run), not at a switch; the cause was not found. The log has no warning or error line (2140 lines,
about 140 per minute); 20 recenters and 40 stereo switches were logged, `submit errors 0`,
the bloom fix `missed 0` for 75180 frames. Eye captures at the start, after the first
loop, in the middle and at the end (`s00` to `s03`) show the scene correctly in both eyes.

Private bytes in the samples taken while stereo rendered rose by about 28 MB per minute
over that run (10.4-10.8 GB in the first third, 10.6-11.1 GB in the last) while the
character went from the rooms to the slums outdoors. A second run of 7 minutes standing
still (`captures/soak/runS`, 20 loops of Insert twice, End, Home twice, the command menu;
samples every 20 s) shows no such rise: private bytes 10406 to 11023 MB, the last sample
(10580 MB, minute 7) within 75 MB of the one at minute 1 (10508 MB), handles
2660 to 2673, GPU memory 7.5 to 7.6 GB, no warning or error. So the switches and keys do
not leak; the rise while walking is most likely the game streaming the areas it reached,
not proven either way.

The same configuration on SteamVR's null driver (OpenXR, eyes 1512x1680, `captures/soak/runV`),
4 minutes of the same loops with SteamVR closed in the middle the way a user quits it:
the runtime moved the session through STOPPING to EXITING, the render module ended it in
6 ms and, as `[xr] reconnect_after_exit = 0` says, stayed off; the engine went to mono
within 0.4 s, the game carried on in its window (frame interval median 8.33 ms) and
nothing was logged as a warning or error. SteamVR started again did not bring the session
back by itself; `xr-restart` did (session created in 2.2 s, stereo and first person back
at once, `v03_after_restart`). The stereo key now does that for a player (see "Player
controls"). Handles 2706 to 2725, threads 116 to 121, working set 2714 to 3043 MB, GPU
memory 6358 to 7277 MB over the 10 samples; frame intervals in stereo 8.3 to 10.3 ms
median per 10 s window (the null driver's pacing), no error line, `submit errors 0`.

## What a real headset may do differently

Everything above was tested with the Null backend and SteamVR's null driver, whose
virtual headset never moves, never sleeps and never loses tracking. What the code does in
the situations a real one adds (read from the code, not seen):

| Situation | What happens |
|---|---|
| tracking lost, headset taken off | the XR layer passes the runtime's views on with their valid bits recorded but not acted on. Views that cannot be rendered (non-finite values, a non-unit orientation, a position over 100 m away, a field of view that does not open) are replaced by the last good ones (`stereo: unusable views from the host ...`, `unusable_views` in `stereo status`); finite but stale poses are used as they come, so the view freezes with the head until tracking returns. If the runtime stops the session (a Quest asleep), the engine renders mono after 45 frames without an XR frame and the desktop game carries on; when the session runs again stereo resumes with a short hitch (the eye target is allocated again) |
| eye size or field of view changes | the field of view is read from every frame's views, so the projection follows at once. A different recommended eye size (only possible with a new session) is compared with the eye target every frame and the target is reallocated (one hitch) |
| the session starts while the game is already in gameplay | stereo is wanted from the start but every frame stays mono until the host has frames; then the eye target is allocated (the first start also sets up foveated rendering: a single frame of about 0.35 s measured), the game window is switched from a fullscreen mode to `vr_window`, and the render module recenters at the session start. A session that starts while the player is not yet facing forward is fixed with the recenter key |
| refresh rate 72, 90 or 120 Hz | in stereo the game thread waits for each XR frame, so the game runs at the headset's rate; its own limit of 120 fps is above 72 and 90 and equal at 120. SteamVR's null driver paces at about 120 Hz, where the game delivered 115 to 120 frames per second with a median frame interval of 8.33 ms; at 120 Hz the GPU budget (8.3 ms, plus the video encoding of Virtual Desktop) is tighter than the 7.0 ms measured for 2 x 2064x2208 without foveation, so 90 Hz is the safer first setting |
| the head far from the origin | with `positional = 1` the eyes move by the head's offset (times `world_scale`) from the eye base, with no limit: standing up or leaning far moves the view through Cloud or through walls. The recenter key makes the current head position the origin again |
| looking straight up or down | with decoupled pitch the head's pitch is applied as it is on top of the camera's yaw; the conversion to the engine's rotator keeps the orientation at the poles (unit test: pitch +-90 and +-89.99 with yaw 30 give the same axes after the conversion). In first person nothing of the character is in view below |

## Known problems

Ordered by how much they would bother a player in the headset:

1. **Battle signal** (see "Combat"): seen working in headset sessions since 06/10
   (`battle signal 0 -> 1`, third person during the battle, back afterwards); the
   follow-camera test flips between "battle" and "the camera does not look at the
   character" several times per battle (not reported as a problem). If it ever misfires,
   battles are played in first person (the toggle still works, and the log shows the
   signal never changing), or exploration in some areas is in third person (the log shows
   the signal at 1 outside a battle).
2. **Level boom near obstacles**: the eyes are where the game camera would be at zero pitch,
   at the boom length the game's collision allowed for the pitched camera. Something the
   pitched camera passed over (a counter, a low wall, a person) can then be close in front
   of the eyes or, possibly, around them (`e05_level_pdown`: an NPC's back fills the view).
   Not seen inside a wall; not tested against one on purpose.
3. **First person**: the character is left out of the picture (`hide = pass`, the default:
   its shadow and footsteps stay; `hide = meshes` hides it completely and the owner heard no
   footsteps with it); the view stays level and, with `head_bob = 0`, does not bob with the steps
   (with `head_bob = 1` the eye bones' offset is smoothed over 80 ms only); interacting,
   climbing or squeezing animations were
   not tried. The eyes are 2 cm in front of the eye bones, about 75 cm above the pawn's
   location; with `hide = none` the inside of the face is visible.
4. **Not exercised with scripted input**: conversations (camera cuts and scripted camera
   moves), real-time cutscenes, the pause menu, loading screens, combat. How decoupled
   pitch and the head pose combine with a cinematic camera is unknown, and whether every
   authored camera fails the follow-camera test (so that neither the level boom nor first
   person applies there) has not been seen.
5. **Pre-rendered movies**: shown on the virtual screen (`[stereo] movie_screen = 1`, the
   default since 08/10). With `0` a movie is rendered into both eyes wherever the game draws
   it. A movie in the slow state after a quick restart plays at 3-12 fps (see "Movies").
6. **Smooth camera yaw** is applied as the game does it (no snap turn option).
7. Square Enix's custom glare (`docs/re/engine.md`, section 10) puts both views' glare at the
   same place of one target; in a scene with glare primitives the left eye would get the
   right eye's glare. Not seen yet (no glare primitives in the scenes tested). With
   `r.BloomQuality 0` its combine pass is not added at all (it is built only on the bloom
   path), so it cannot affect the picture then.
8. **Posters on the sandwich board** outside the first room looked different between the
   eyes in a headset session (missing in the left eye with the board off-centre, a
   translucent offset copy in the right eye). With the Null backend at 3072x3264 the posters
   were in place in both eyes, centred and off-centre, before and after the ambient
   occlusion fix; only the right eye's framed notice looked paler than the left's. The
   occlusion ghost (fixed) is the likely cause, since that session's image was much darker
   (shade, where occlusion dominates); not confirmed in a headset.
9. Without the UI layer (`[ui] layer = 0`, or no XR session) the in-game UI is composited
   into each eye as a central crop of the 16:9 UI; the size variables cannot fix that
   (`docs/re/engine.md`, "What the UI composite does with an eye view"). With it the UI is on
   its own layer (section "UI layer").
10. **The headset's own recenter** (holding the Meta button on a Quest) is logged by the XR
   layer (`runtime reference space change pending`) but the recenter offset stored at the
   session start or by the recenter key stays applied on top of the runtime's new origin
   (`src/xr/src/openxr_backend.cpp`, the `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`
   case), so after it the view can be off by the yaw and position stored before. The
   recenter key (End, View/Back + left stick click) puts it right.
11. **Gamepad View/Back alone** reaches the game on release, about 120 ms long, instead of
   while held (`[controls] pad_hold_view`). A game action that needs View held would not
   work; none is known in exploration (View opens the map).
12. With a real OpenXR runtime the render module must hand the frame its XR thread already
   waited to the game thread at the start of stereo instead of waiting a second one
   (`XrController::BeginGameFrame`, in place); otherwise the game thread blocks in
   `xrWaitFrame` forever when the pipeline is idle (seen with SteamVR's null driver).
