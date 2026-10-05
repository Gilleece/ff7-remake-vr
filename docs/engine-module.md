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
| light sort-key immediate | one byte in `FDeferredShadingSceneRenderer::RenderLights` | only when `[stereo] light_fix = 1` or the `stereo lightfix 1` command |
| Square Enix's bloom reduce pass (`Process`) | inline hook, render thread | `[stereo] bloom_fix` (default on): for the first level of a view that does not start at the origin, an RHI command arms the right-eye bloom fix (below) |
| D3D11 immediate context draw/dispatch/clear/copy functions | inline hooks, RHI thread | installed at the first stereo frame when the bloom fix is on (it needs to act on one DrawIndexed), or by the first GPU trace; otherwise not installed |
| `FRenderTargetPool::FindFreeElement` | inline hook, render thread | only after `gpu names on` (GPU trace labels) |
| the object array and name pool | read on the game thread | only with `[stereo] movie_screen = 1`: movie detection (below) |
| the controlled pawn, its location and the view target | reflected functions called through `ProcessEvent`, game thread, every frame | camera modes: level boom and first person (see "Camera modes") |
| the pawn's skeletal mesh components and the mesh components attached to them | `SetVisibility` through `ProcessEvent` | only while first person applies; put back afterwards |
| the battle signal | a reflected function called through `ProcessEvent` every frame (`[first_person] battle_signal`) | automatic third person in battles |
| XInput state | a filter in the loader's XInput proxy | only with stereo enabled: the View/Back combinations of "Player controls" trigger their action and are removed from the state; View alone reaches the game as a short press on release |
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
30 to 50 cm in the street, depending on the pitch). The test has a hysteresis of 30 frames each way, so a short scripted move does
not flip the mode. A camera that looks elsewhere (an authored shot of a conversation or a
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
  read every frame after the world has ticked (`SceneComponent.GetSocketLocation`). Its offset from the pawn's location is smoothed over
  about 80 ms so animation jitter does not shake the view, while the pawn's own movement is
  followed without delay. `head_offset` (forward, right, up, in the camera's yaw frame) is
  added. When the bone cannot be read (no such bone, a call fails, a location more than
  2.5 m from the pawn) the eyes go to the pawn's location plus `eye_offset` for that frame.
- **Body**: with `hide = meshes` (default) every skeletal mesh component owned by the pawn
  that is visible is hidden (`SetVisibility(false)`, not propagated to attached components),
  and so is every mesh component of another actor attached to those meshes (Cloud's sword
  is one: `SceneComponent.GetChildrenComponents(true)`), when the first-person blend passes
  half way; exactly those are shown again when first
  person stops applying (toggle, authored camera, battle, stereo off) or when the controlled
  pawn changes. A mesh the game shows again while hidden is hidden again the next frame.
  Finding the meshes scans the object array each time first person starts (a one-off cost
  of a few milliseconds on the game thread), so meshes added since the last time (equipment)
  are included. When the game exits nothing needs restoring (visibility is not saved).
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

### Gamepad toggle

The loader's XInput proxy passes every successful `XInputGetState` / `XInputGetStateEx`
result through `ff7vr::engine::filter_pad` (registered only when the stereo device is
enabled). View/Back (`0x0020`) held and the right stick click (`0x0080`) pressed requests a
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
| first / third person | Home (`[first_person] toggle_key`) | right stick click | switches the camera mode (see "First person") |
| recenter | End (`[controls] recenter_key`) | left stick click | the direction the head faces now becomes forward, and the head's position the origin, for the view and for the floating panels: the UI panel and the virtual screen are placed in front of the head again |
| stereo off / on | Insert (`[controls] stereo_key`) | Menu/Start | stereo off: the game is shown on the virtual screen (the same fallback as for menus and loading screens); on again: back to 3D. The game window keeps its size and mode either way |
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
photo mode key).

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
| `vr_window` | `1280x720` | window size the game is switched to while VR renders in a fullscreen mode; `0` keeps the mode (see "Window modes") |
| `movie_screen` | `0` | movie detection: stereo is held off while a pre-rendered movie plays, so the virtual screen shows it (below) |
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
| `hide` | `meshes` | `meshes`: hide the character's skeletal meshes, and the mesh components of other actors attached to them (the sword), while in first person; `none`: hide nothing |
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
| `stereo lightfix <0\|1>` | light sort-key patch |
| `stereo log <n>` | log the eye cameras of the next n stereo frames |
| `stereo bloomfix [0\|1]` | right-eye bloom fix on/off, with its counters (reduce passes seen, commands queued, draws fixed, misses) |
| `stereo movie [on\|off]`, `stereo movie menu <0\|1>` | movie detection on/off and its state; `menu 1` counts the menu background players too (test) |
| `stereo window [<w>x<h>\|0]` | window size while VR renders in a fullscreen mode, and the state |
| `stereo frametime <s>` | frame time window length in seconds; restarts the window (for A/B measurements) |
| `stereo swap <0\|1>` | test: render the right eye into the left half of the target and the left eye into the right half (the eyes then come out swapped). Tells a bug that follows a view's position in the target from one that follows the view |
| `gpu names on`, `gpu trace <prefix> [dump fullscreen \| dump <from> <to>] [scale <n>]`, `gpu status` | one-frame GPU trace (`docs/re/engine.md`, Tools) |
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
| Light sort-key patch outdoors | no visible difference (`runM/g_lightfix.png`; the differences are idle animation) |
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

What to try first, in this order:

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

## Known problems

Ordered by how much they would bother a player in the headset:

1. **Battle signal unverified in a battle** (see "Combat"). If it is wrong, battles are
   played in first person (the toggle still works, and the log shows the signal never
   changing), or exploration in some areas is in third person (the log shows the signal at
   1 outside a battle).
2. **Level boom near obstacles**: the eyes are where the game camera would be at zero pitch,
   at the boom length the game's collision allowed for the pitched camera. Something the
   pitched camera passed over (a counter, a low wall, a person) can then be close in front
   of the eyes or, possibly, around them (`e05_level_pdown`: an NPC's back fills the view).
   Not seen inside a wall; not tested against one on purpose.
3. **First person**: the whole character is hidden (its shadow too, presumably: not checked); the view does not
   follow the character's head animation (the eye bones' offset from the pawn is smoothed
   over 80 ms and the view stays level); interacting, climbing or squeezing animations were
   not tried. The eyes are 2 cm in front of the eye bones, about 75 cm above the pawn's
   location; with `hide = none` the inside of the face is visible.
4. **Not exercised with scripted input**: conversations (camera cuts and scripted camera
   moves), real-time cutscenes, the pause menu, loading screens, combat. How decoupled
   pitch and the head pose combine with a cinematic camera is unknown, and whether every
   authored camera fails the follow-camera test (so that neither the level boom nor first
   person applies there) has not been seen.
5. **Pre-rendered movies**: detection exists but no movie was reached; `[stereo]
   movie_screen` is off by default. Without it a movie would be rendered into both eyes
   wherever the game draws it (UI or scene).
6. **Smooth camera yaw** is applied as the game does it (no snap turn option).
7. Square Enix's custom glare (`docs/re/engine.md`, section 10) puts both views' glare at the
   same place of one target; in a scene with glare primitives the left eye would get the
   right eye's glare. Not seen yet (no glare primitives in the scenes tested).
8. Without the UI layer (`[ui] layer = 0`, or no XR session) the in-game UI is composited
   into each eye as a central crop of the 16:9 UI; the size variables cannot fix that
   (`docs/re/engine.md`, "What the UI composite does with an eye view"). With it the UI is on
   its own layer (section "UI layer").
9. With a real OpenXR runtime the render module must hand the frame its XR thread already
   waited to the game thread at the start of stereo instead of waiting a second one
   (`XrController::BeginGameFrame`, in place); otherwise the game thread blocks in
   `xrWaitFrame` forever when the pipeline is idle (seen with SteamVR's null driver).
