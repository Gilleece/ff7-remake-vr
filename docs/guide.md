# ff7vr player guide: the details

The README covers the essentials. This guide has the rest: what the launcher does,
what you should see, a first-session checklist, everything that has not been tested,
the known problems in full, troubleshooting, performance and what the launcher
changes on the PC. In the package folder this file is `GUIDE.md`.

## The package folder

The package folder (`ff7vr-<date>-<commit>`) can sit anywhere, for example on the
desktop. It contains:

| File | What it is |
|---|---|
| `start-vr.cmd` | **double-click to play in VR** |
| `restore.cmd` | double-click to put the game folder back to normal if anything went wrong |
| `collect-diagnostics.cmd` | double-click after a problem: zips the last session's log, the settings and basic system facts to send back |
| `ff7vr.ini` | the settings (open it in Notepad) |
| `xinput1_3.dll` | the mod itself |
| `ff7vr-launcher.ps1` | the script the `.cmd` files run |
| `README.md`, `GUIDE.md` | the short guide; this detailed guide |
| `VERSION.txt` | which version of the mod this is |
| `LICENSE`, `THIRD-PARTY-NOTICES.md` | the mod's licence; the components built into it and their licences |
| `logs\` | one folder per session with the mod's log (created on the first session) |

## What the launcher does

After you double-click `start-vr.cmd`, the launcher:

1. finds the game through Steam. If the game of a previous session is still running
   or still closing, or another launcher window is still tidying up, it waits for it
   (up to 90 seconds, with a message saying what it waits for) instead of starting a
   second copy, and gives up with a message only if it does not go away. Virtual
   Desktop serves one game at a time, so two copies at once leave the headset
   without a picture;
2. puts things back first if an earlier session was interrupted;
3. checks that Steam runs (it starts Steam if not) and warns if no VR runtime's PC
   app seems to run (it looks for the Virtual Desktop Streamer, SteamVR, Meta Quest
   Link, PICO Connect and Windows Mixed Reality, and names them in the warning). That
   is only a warning: the game then runs flat on the monitor, and the mod keeps trying
   to reach a headset every 5 seconds, so connecting it later is enough;
4. sets ReShade/Luma aside for the session (it renames `dxgi.dll` in the game's
   `End\Binaries\Win64` folder to `dxgi.dll.vr-disabled`; with ReShade/Luma loaded
   the mod does not reach the headset);
5. copies `xinput1_3.dll` and `ff7vr.ini` into that folder and records every change
   in `ff7vr.session.json` there;
6. starts the game (with `-d3d11`) and waits;
7. when the game is gone, however it ended (quit, Alt+F4, ended in Task Manager,
   crashed), and its helper programs have closed too (the game's crash report
   window, if one opened, has to be closed first): keeps the session's log in
   `logs\<date-time>\`, removes the mod's files and puts `dxgi.dll` back;
8. closes its window by itself after a 10-second countdown when everything went
   well (press a key during the countdown to keep it open). After a warning or an
   error the window stays open with the message until you press a key. Either way
   everything is already put back by then: nothing waits for that key press.

Leave the launcher's window open while you play. If it gets closed anyway, or the PC
restarts during a session, double-click `restore.cmd` (or just start the next session:
it tidies up first). `restore.cmd` is harmless when nothing needs doing.

To play again straight away, quit the game and start `start-vr.cmd` again; if the
previous session is still closing, the new one waits for it by itself.

Options, from a command prompt in the package folder:

```
start-vr.cmd -KeepInstalled     leave the mod installed after the game exits (Luma stays set aside).
                                restore.cmd removes it again. The game can then also be
                                started from Steam (see "Installing without the launcher")
start-vr.cmd -KeepLuma          do not set ReShade/Luma aside: the game runs in 3D with it (the mod
                                corrects Luma's tonemapping for the right eye and turns Luma's own
                                DLSS off for the headset image; checked without a headset so far)
start-vr.cmd -ExtraArgs "..."   extra arguments for the game
start-vr.cmd -NoPause           close the window at once at the end, also after an error
powershell -NoProfile -ExecutionPolicy Bypass -File ff7vr-launcher.ps1 status
                                show what is installed, change nothing
```

The launcher starts the game executable directly instead of through Steam, so the
launch options in the game's Steam properties are not applied; `-d3d11` is always
passed. Your save games are not touched by the mod or the launcher (the game
autosaves as usual).

## What the launcher changes on the PC

Only the game's `End\Binaries\Win64` folder, and only for the session:
`dxgi.dll` is renamed to `dxgi.dll.vr-disabled`, and `xinput1_3.dll`, `ff7vr.ini`,
`ff7vr.session.json` are added (the mod writes `ff7vr.log` there while it runs).
Afterwards those are removed and `dxgi.dll` gets its name back. No game file is
modified, nothing is installed, the PC's OpenXR settings are not changed, and the
Steam launch options are not touched. The mod's name `xinput1_3.dll` is how the game
loads it: the game loads a file of that name from its own folder before the one in
Windows.

## Which headset runtime the mod uses

The mod talks to the headset through OpenXR. With `[xr] runtime = auto` (the
default) it finds the OpenXR runtimes on the PC by itself: the PC's default runtime,
every runtime registered with Windows, and the usual install folders of Virtual
Desktop, SteamVR, Meta Quest Link, PICO, Windows Mixed Reality and Pimax. It tries
first the runtimes whose PC app is running (Virtual Desktop Streamer, SteamVR,
Meta Quest Link, PICO Connect, Mixed Reality Portal), then the PC's default, then
the rest, and uses the first one that reports a connected headset. SteamVR, Meta
Quest Link and Windows Mixed Reality are only tried while they run (or when one of
them is the PC's default), because trying them would start them; PICO's runtime only
while PICO Connect runs, because it reports a headset even when none is connected.
If no runtime has a headset, the game runs flat and the mod tries again every 5
seconds, so starting the headset's app or connecting the headset later is enough.

The log says what happened: `OpenXR runtime (auto): chose <name> ...` with the
runtime's `.json` file, one `skipped` line per runtime it passed over with the
reason, and while nothing is found, `OpenXR runtime (auto), round N: no headset
found: ...` listing every runtime and why it was not used.

To pin one runtime instead, set `[xr] runtime` in `ff7vr.ini` to `virtualdesktop`,
`steamvr`, `system` (the PC's default OpenXR runtime) or the full path of a runtime's
`.json` file. Either way the choice applies to this game only; the PC's default
OpenXR runtime is never changed.

## Installing without the launcher

The mod can also stay in the game folder for good, without the launcher. Every
package build comes with `ff7vr-<date>-<commit>-dropin.zip` for this.

**Install:** unzip the drop-in zip into the folder where `ff7remake_.exe` is: the
game's folder, then `End\Binaries\Win64` (in Steam: right-click the game, Manage,
Browse local files). It adds `xinput1_3.dll` (the mod), `ff7vr.ini` (the settings),
`ff7vr-start.cmd` and a folder `ff7vr-docs` with this guide. If an `xinput1_3.dll`
is there already, another mod uses that name: do not overwrite it.

**Play:** start the game from Steam as usual, or double-click `ff7vr-start.cmd` in
that folder (Steam must be running). Either way the mod loads by itself.

- Direct3D 11: the mod needs it, and the game uses Direct3D 12 unless told otherwise.
  Nothing to set: when the command line has no graphics option, the mod adds
  `-d3d11` itself (the log says `d3d11: added -d3d11 to the command line`;
  `[loader] force_d3d11`). `ff7vr-start.cmd` passes it, and `-d3d11` in the game's
  Steam launch options does no harm. If the launch options ask for another one
  (`-dx12`, `-d3d12`, `-vulkan`), the mod leaves them alone, the game runs flat, and
  the log has a warning.
- ReShade and Luma can stay in place: nothing to do. While VR renders, the mod
  corrects Luma's tonemapping for the right eye and turns Luma's own DLSS off for the
  headset image (Luma treats the two eyes as one half-size frame). Luma's DLSS still
  works in flat play; for DLSS in VR, use the mod's `[dlss]` settings. Checked on the
  simulated headset only so far; if the right eye looks wrong with Luma loaded, rename
  `dxgi.dll` to `dxgi.dll.vr-disabled` for VR and say so.
- A `-dlss` drop-in needs an NVIDIA RTX graphics card and NVIDIA's DLSS model file,
  which is not part of the package: `nvngx_dlss.dll` beside `ff7remake_.exe` (see
  "DLSS" below; if the NVIDIA App's DLSS override is set for this game, the driver's own
  copy of the model is used instead). DLSS is on in the `-dlss` package (`[dlss]
  enabled = 0` in `ff7vr.ini` switches it off).

**Log:** `ff7vr.log` in `End\Binaries\Win64`. The logs and crash dumps of the five
sessions before it are kept in `ff7vr-logs\` there (`[log] keep_sessions`); older ones
are deleted.

**Remove:** delete from `End\Binaries\Win64`: `xinput1_3.dll`, `ff7vr.ini`,
`ff7vr-start.cmd`, `ff7vr.log`, the folders `ff7vr-docs` and `ff7vr-logs`, and any
`ff7vr-crash-*.dmp` file or `ff7vr-captures` folder; rename `dxgi.dll.vr-disabled`
back to `dxgi.dll` if you set it aside. Nothing else on the PC is changed.

The launcher package (`start-vr.cmd`, above) is the alternative that leaves nothing
in the game folder between sessions. Do not use both: the launcher refuses to start
while an `xinput1_3.dll` it did not put there is in the folder.

## What you should see

- **On the monitor:** the game starts as usual. As soon as the 3D view is running in
  the headset, the mod switches the game to a 1280x720 window (the 3D rendering does
  not work in the game's fullscreen modes); the window shows a cropped view of the
  left eye with the HUD drawn over it. This only lasts for the session: the game's
  display setting is not changed, and the next start is in your normal display mode.
- **In the headset, title screen, menus and loading screens:** the game on a flat
  virtual screen, 1.8 m wide, 2 m in front of you at eye height.
- **In the headset, in the world:** the 3D scene around you, **in first person**:
  you see through Cloud's eyes, his body and sword are hidden. Home (on the pad: both
  stick clicks together) switches to third person, behind the character at
  shoulder height. The horizon stays level: the game camera turns you left and
  right, but looking up and down is done with your head. Leaning moves the view.
- **In a battle** the view switches to third person by itself and back to first
  person afterwards (seen working in play). You stay behind the character while the
  game's battle camera frames the enemies (`[camera] combat = level`); any change of
  shot glides over a third of a second instead of jumping (`[camera] blend_seconds`).
- **Conversations and cutscenes** that use their own camera shots are shown from the
  game's camera, as the game frames them (not seen in testing either).
  `[stereo] cutscene_screen = 1` shows them flat on the virtual screen instead, like a
  movie, if the game camera's cuts and pans in 3D are uncomfortable.
- **HUD and menus in the world:** on a flat panel about 3 m in front of you, 3.56 x
  2 m, that stays in place when you turn your head. Whatever the game shows full
  screen (main menu, command menu, dialogue) is on that panel too.
- Whenever the game stops delivering 3D images (a menu that stops the 3D view, a
  loading screen, a long hitch), the headset switches to the virtual screen and back
  without a black frame.
- The view is centred once, when the headset connects: face the direction you want
  to play in at that moment, or press End (View/Back + left stick click) later.

## More on the controls

| Keyboard | Gamepad | Effect |
|---|---|---|
| Home | both stick clicks together (L3+R3), or hold View/Back + right stick click | first / third person |
| End | hold View/Back + left stick click | recenter |
| Insert | hold View/Back + Menu/Start | 3D off / on (reconnects a lost headset) |
| Page Down / Page Up | hold View/Back + D-pad down / up | HUD panel nearer / farther |

On the gamepad the game does not see the mod's combinations. View/Back pressed on its
own still reaches the game, but only when you release it (so the map does not open on
the way to a combination). For L3+R3 both clicks must go down within 150 ms of each
other; the pair switches once however long you hold it. A single stick click still
reaches the game (the game uses them, for example R3 for the camera and lock-on), but
up to 150 ms late, because the mod waits that long for the second click; a quick tap
is handed over as a short press when you release it. `[controls] fp_toggle_chord`
changes the buttons (empty = off) and `fp_toggle_chord_ms` the window. All keys can be
changed in `ff7vr.ini` (`[controls]`, `[first_person] toggle_key`).
The gamepad combinations go through the mod only for an XInput pad (an Xbox pad or a pad
in XInput mode); a DualSense or a pad in DirectInput mode is read by the game itself, and
the combinations do not apply to it (use the keys).

3D starts in first person outside battles. First person puts the view between the
character's eyes, leaves Cloud's body out of the picture (his shadow and footsteps stay;
the sword is hidden), and blends over in about a
third of a second. The view does not bob with Cloud's steps: it follows his movement at
once and slow changes of his head's height (crouching, climbing) about half a second
late. `[first_person] head_bob = 1` brings the step motion back. In first person the game's
sound is heard from your head, so it turns with you (`[first_person] audio_listener = 0` leaves it at the game camera). It only applies while the game's normal follow camera is active:
during a scripted camera shot the game's camera is used as it is. A battle switches
to third person and its end back to first person; a manual switch lasts until the
next battle starts or ends.

The headset's own recenter (on a Quest, holding the Meta button) is handled: the
mod drops its own recenter offset, so the view faces where you face and the HUD panel
and the virtual screen come back in front of you. It was tested with a simulated
headset but not yet seen on a real one; if it ever leaves the view turned, End does
the same job.

Snap turn (gamepad): `[comfort] snap_turn = 45` (or 30) makes the right stick turn your
view in steps of that many degrees instead of smoothly; push it again after letting go
for the next step. The left stick then moves Cloud where you look; keyboard movement is
not turned with it. Recenter makes the way you face forward again.

Taking the headset off (or opening the headset's own menu) while 3D runs pauses the game:
the mod presses M, the game's menu, once (`[xr] pause_on_remove = 0` turns this off,
`pause_key` changes the key). It only does so while the game window has the focus.

## First-session checklist

Check in this order; each step depends on the ones before it. The log is
`logs\<date-time>\ff7vr.log` in the package folder after the session (during the
session it is `ff7vr.log` in the game's `End\Binaries\Win64` folder).

1. **The launcher ran through.** Its window says the mod was installed and the game
   started, and after quitting: "The game folder is back to normal".
2. **The mod loaded.** The log starts with `ff7vr 0.1.0 loaded` and contains
   `engine: stereo device installed`. If it says `engine: stereo will NOT be enabled`,
   the reason follows on the same line.
3. **The headset was reached.** `OpenXR runtime (auto): chose ...` names the runtime,
   then `xr: session created ... runtime '...'` (for Virtual Desktop
   `'VirtualDesktopXR'`) and `xr: frame loop running`. If you see
   `xr: no session (SystemUnavailable ...)` and `no headset found`, the headset was not
   connected at that moment; connecting it is enough, the mod retries every 5 seconds.
4. **Title screen on the virtual screen** in front of you at eye height, sharp, not
   too dark or washed out compared with the monitor.
5. **Load a save. The world in 3D.** Correct depth, both eyes aligned (no double
   vision when looking at a near object), the horizon level, the scene stable when
   you turn your head. The 3D view is sharpened a little by default (`[picture]
   sharpen = 0.5`; `0` turns it off, `1` is strong). The log shows `stereo: rendering STEREO ... (eye WxH ...)` with
   the per-eye size the headset's runtime asked for.
6. **Smoothness.** Turning your head must feel smooth. The log's `timing:` lines
   every 10 seconds show the frame rate and `errors 0`.
7. **HUD panel.** Complete to its corners, sharp, comfortable to read; open the
   command menu (Space or the pad's command button). Try Page Down / Page Up (or
   View/Back + D-pad) for its distance, End (or View/Back + left stick click) to
   recenter, and note what you like for `[ui] distance` / `size`.
8. **First person** (the start): the view at Cloud's eye height, facing where the
   camera faced, no part of Cloud or his sword in view. Walk and turn: comfortable?
9. **Third person** with Home or both stick clicks (L3+R3): behind Cloud at
   shoulder height. Move the right stick or mouse up and down: your height should
   stay the same while the view orbits. Walk with your back to a wall and orbit: the
   eyes should stay out of the wall. Switch back and forth a few times: Cloud and his
   sword must be complete every time in third person.
10. **Foveated rendering.** Look straight ahead and let your eyes wander to the edges:
    it should look as sharp as before up to well beyond comfortable eye movement.
    Compare with `[foveation] enabled = 0`.
11. **A battle:** third person when it starts, first person again when it ends. The log
    shows `player: battle signal 0 -> 1` and `player: battle started: third person`;
    if no such line appears, battle detection does not work (see the settings).
12. **A conversation, a real-time cutscene, a loading screen between areas, a
    pre-rendered movie, the pause menu:** none of these has been tried yet. The log
    shows `player: camera mode ... -> game camera` when a scripted shot takes over.
    See the list below for what to look for.
13. **Quit the game** normally: the headset returns to its runtime's own view,
    the launcher tidies up. Afterwards ReShade/Luma works again in the flat game.

## What has not been tested

- **Other headsets and PCs.** The mod has been played only on a Meta Quest 3 through
  Virtual Desktop, on one PC, in the first areas of one save game (Sector 7 slums,
  indoors and the street). Other headsets, Virtual Desktop settings and graphics
  cards have not been tried. The automatic runtime choice was tested with SteamVR's
  virtual headset and with Virtual Desktop without a headset; other runtimes are
  recognised by their file names and install folders but have not been tried.
- **Conversations, real-time cutscenes, loading screens between areas, the pause
  menu.** Not tried with scripted input; battles and pre-rendered movies have been
  played (the switch to third person and back works; movies are listed under "Known
  problems in full"). What to look for: scripted camera shots should be shown as the
  game frames them (the mod then uses the game's camera); the camera may cut between
  shots, which can be uncomfortable in a headset.
- **Pre-rendered movies** are shown on the virtual screen (`[stereo] movie_screen = 1`,
  the default). A movie that plays at a few frames per second is most likely the slow graphics-driver
  state after a quick restart, not the movie itself: the movie's frames are uploaded to
  the card, and in that state uploads crawl (stereo can look fine until then). Quit, wait
  a minute and a half, start again. The log's `movie: stopped ... fps` line gives the
  frame rate over each movie, and `game copy engine` at 40 % or more in the timing lines
  confirms the state.
- **The View/Back gamepad combinations.** The L3+R3 chord has been used with a real
  XInput pad in play. The View/Back combinations were tested only with simulated
  button states, so what the game does with the View/Back press handed over on
  release is not known. The keys (Home, End, Insert, Page Down/Up) were tested in the game.
- **The headset's own recenter** (holding the Meta button) on a real headset. It is
  handled and was tested with a simulated headset, as was the mod's own recenter (End).
- **Lost tracking** (covering the headset's cameras, a dark room): the mod holds the
  last view, or keeps turning with the head at a fixed position when only the position
  is lost. Tested with a simulated headset only.
- **Disconnecting and reconnecting the headset** during a session, and quitting from
  Virtual Desktop's menu, were tested with SteamVR's virtual headset only.
- **Other save games and areas** than the first rooms of the save used in testing
  (Sector 7 slums). Sunlit areas, fog and big open areas are untested in 3D.
- **The game's HDR output.** The virtual screen assumes normal (SDR) output; with HDR
  switched on in the game it will look wrong.

## Known problems in full

- **The picture looks washed out and too bright in the headset.** Cause not known
  yet. It is there with HDR off in the game's options and the game's brightness
  setting at 0. Switching bloom off makes it a little better: in `ff7vr.ini`, section
  `[stereo_cvars]` at the end, remove the `;` at the start of the line
  `; r.BloomQuality = 0` (console variables in that section apply only while 3D
  runs).
- **Wait a minute and a half between quitting the game and starting it again**: a game
  started sooner can run at about 10 frames per second for minutes (the launcher and
  `ff7vr-start.cmd` wait by themselves; a start from Steam does not).
- **Turning comfort.** The game camera turns you smoothly, as in the flat game, which
  some players find uncomfortable: `[comfort] snap_turn = 45` turns in steps instead
  (pad only), and `[comfort] vignette = 0.6` darkens the edges of the view while you
  move or turn with the sticks.
- **Markers over enemies and objects are slightly off** on the default HUD panel
  (3 to 6 degrees towards the edges). `[ui] size = 1.57` lines them up; the larger
  default is easier to read. In battles the panel takes `[ui] battle_size` (1.57 by
  default) by itself, so the markers over enemies line up there; `0` keeps `size`.
- **The game window becomes 1280x720 while 3D runs** if the game was in a fullscreen
  mode, and stays that way until the game exits (switching 3D off with Insert does not
  change it back). The game does not remember it: your display setting is not changed,
  and the next start, with or without the mod, is in your normal display mode. Checked
  for quitting in 3D, quitting from the virtual screen and the game being killed. Not
  checked: changing the game's graphics options while the window is switched; leave
  them alone during a VR session.
- **ReShade/Luma together with the mod** has been checked without a headset only: Luma
  can stay in place (the mod corrects Luma's tonemapping for the right eye and turns
  Luma's own DLSS off for the headset image). The launcher still sets ReShade/Luma
  aside for every session and puts it back afterwards; `-KeepLuma` keeps it loaded.
- **Battle detection** has worked in play so far. If a battle stays in first person,
  switch with Home or both stick clicks (L3+R3), and send the log. If it misfires
  outside battles, set `battle_signal =` (empty) and `default = 0`.
- **First person:** the character is left out of the picture (its shadow and footsteps
  stay; `[first_person] hide = meshes` hides it completely); the view
  stays level and, with `head_bob = 0` (the default), does not bob with the steps;
  climbing, squeezing through gaps and other special animations were not tried.
- **Third person near obstacles:** the eyes are where the game camera would be at
  zero tilt, so something the tilted camera passed over (a counter, a low wall, a
  person) can be right in front of your eyes. The view now moves in front of walls and
  objects in the way (`[camera] collision = 1`; not yet tried against a wall or a
  person in play). `[camera] boom = game` uses the game camera's own position instead.
- **The desktop window** shows a crop of the left eye, not the full picture.
- **Some effects stay as in the flat game:** the vignette (darker image corners),
  and the game's depth of field in cutscenes. Camera motion blur and chromatic
  aberration are switched off while 3D runs.
- **Square Enix's lens glare effect** may appear in the wrong eye in scenes that use
  it (not seen in testing). With bloom off (`r.BloomQuality = 0`) the game does not
  draw it at all.
- **Fire, heat haze and refraction:** the right eye showed the left eye's distortion
  pass stretched over it (reported as horizontal blurry lines over the whole right eye
  near a fire). Fixed by `[stereo] distortion_fix`, on by default; checked without a
  headset only. If the right eye still breaks up near fire or hot air, set
  `r.DisableDistortion = 1` under `[stereo_cvars]` (no heat haze or refraction at all)
  and send the log.
- Frame pacing with Virtual Desktop has not been measured beyond the mod's own frame
  times (see "Performance"). With SteamVR's virtual headset the runtime sometimes
  blocked for 6 to 7 ms per frame during the first 15 to 20 seconds of a session.

- **Reflections that differ between the eyes:** the game's screen-space reflection
  pass only works for the eye drawn at the left of the game's picture. The right eye
  gets reflections, but in the wrong places: specks of reflected light on floors the
  left eye does not have, and less of a puddle's reflection. Not fixed yet, so the
  default is now `[stereo] ssr_fix = 2`: screen-space reflections off in both eyes, so
  the eyes match (reflection captures and light highlights stay). `ssr_fix = 1` gives
  the right eye its own, wrong ones; `ssr_fix = 0` leaves them in the left eye only.

Fixed and confirmed in the headset:

- **A ghost image in the right eye:** the game's ambient occlusion pass computed the
  right eye's occlusion from the left eye's data. Fixed by `[stereo] ao_fix`, on by
  default.
- **White square blocks on exposed skin indoors**, in both eyes, and the lowest third
  of the view missing some lamp light indoors, cut off at a hard horizontal line: the
  game's tiled lighting pass mishandled the eye views. Fixed by `[stereo] light_fix`,
  on by default. If you still see either in another area, say so; `light_fix = 0`
  brings back the old behaviour.

## When something is wrong

1. **The diagnostics zip.** After quitting the game, double-click
   `collect-diagnostics.cmd`. It writes `diagnostics-<date-time>.zip` into the
   package folder with the last session's log folder (`ff7vr.log`, and a
   `ff7vr-crash-*.dmp` if the game crashed), your `ff7vr.ini`, `VERSION.txt` and a
   `system.txt` (Windows version, graphics card and driver, the OpenXR runtimes
   and which one the mod used, the state of the game folder). It changes nothing.
   If you ran several sessions since the problem, send that session's folder from
   `logs\` as well.
2. A sentence on what you saw in the headset and when (the log has times; note the
   time on the clock), and a screenshot of the monitor if it shows the problem.

To narrow a problem down, change one setting at a time and start a new session:

| If the problem is... | Try |
|---|---|
| anything in 3D (image, depth, flicker, crash in the world) | `[stereo] enabled = 0`: if the virtual screen works, the 3D rendering is at fault |
| HUD or menus missing, cut off, doubled | `[ui] layer = 0` (the HUD goes back into the 3D image, cropped) |
| blocky or shimmering edges, any odd shading | `[foveation] enabled = 0` |
| uncomfortable camera height or movement | `[camera] boom = game`, then `[stereo] decoupled_pitch = 0`; for motion, `[comfort] vignette = 0.6` |
| first person in a battle, or third person outside one | `[first_person] battle_signal =` (empty) and `default = 0` |
| first person at the wrong height or inside the head | `[first_person] eye = offset` (a fixed height above the character's position instead of its eyes) |
| first person: the view lags when Cloud crouches or climbs | `[first_person] steady_seconds = 0.15` (follows faster, a little more step motion), or `head_bob = 1` |
| anything else in first person | `[first_person] enabled = 0` |
| the right eye breaks up (smears, lines) near fire, hot air or glass | `[stereo] distortion_fix = 1` (default); if it persists, `r.DisableDistortion = 1` under `[stereo_cvars]` |
| stutter or low frame rate | `[xr] resolution_scale = 0.8` |
| the headset stays on its runtime's own view although the game runs | look in the log for `OpenXR runtime (auto)`: if it chose another runtime than the headset's, pin the right one with `[xr] runtime`. Otherwise quit the game, wait for the launcher window to finish (it closes by itself), start again; if that does not help, restart the headset's PC app (for example the Virtual Desktop Streamer) |
| the game does not start or crashes at once | `restore.cmd`, then start the game from Steam without the mod to rule out the game itself |

## Performance

`[graphics] profile` in `ff7vr.ini` picks a bundle of speed settings in one line
(`custom`, the default, applies nothing). A key you set yourself in the ini always wins
over the profile; the log's `graphics:` lines say what the profile applied and what it
left alone. Measured on the development PC (RTX 5080) without a headset, 3072x3264 per
eye, standing in the slums street, 5-second windows, median frame time:

| Profile | What it sets | Frame time |
|---|---|---|
| `custom` (the shipped ini) | foveation `performance`, the four detail-level lines | 8.4 ms |
| `quality` | foveation `quality`, the four detail-level lines | 8.9 ms |
| `balanced` | foveation `performance`, the detail-level lines, sun shadows 2048 with 3 cascades, translucency lighting 32 | 7.9 to 8.1 ms |
| `performance` | `balanced` without volumetric fog and without the far detail levels, `render_scale` 0.9 | 6.4 ms |

With the shipped ini the profile only adds what the ini leaves out: to let it choose the
foveation preset and the detail levels, put a `;` in front of `[foveation] preset` and the
four detail lines under `[stereo_cvars]`. Fog and far detail matter more in open or hazy
places than in this street.

In the first headset session on the development PC (RTX 5080, Ryzen 7 5800X3D;
Quest 3 through Virtual Desktop, 3072x3264 per eye) frame times were mostly 11.5 to
14 ms, and 28 ms during one heavy minute: below 90 Hz at full resolution in places.
That is one PC and one session; judge it on your own setup.

The mod logs its own frame timing. Every 10 seconds `ff7vr.log` gets a `timing:`
block: the frame rate over those 10 seconds, the mode (`screen` = virtual screen,
`stereo` = 3D), how many frames went to the headset, `errors`, and the GPU time of
the 3D scene with foveated rendering. A Quest 3 at 90 Hz needs a frame every
11.1 ms (80 Hz: 12.5 ms, 120 Hz: 8.3 ms).

For orientation only, two numbers from the development PC, first room of one save
game, standing still, without a headset:

- 3D at 2 x 2500x2600 pixels per eye, without a VR runtime (internal test mode, the
  game's frame cap lifted): 8.6 ms per frame for both eyes.
- Foveated rendering, GPU time of the 3D scene at 2 x 2496x2592 per eye with the
  simulated headset: 7.71 ms off, 6.76 ms `quality`, 6.38 ms `balanced`, 6.12 ms
  `performance`.

Busy scenes (combat, open areas) cost more than a quiet room, and Virtual
Desktop's video encoding comes on top.

## DLSS (optional, NVIDIA RTX cards)

A package whose folder name ends in `-dlss` can use NVIDIA DLSS: each eye is rendered at
a fraction of its size and DLSS rebuilds the full-size image from it and from the
previous frames, in place of the game's own anti-aliasing. The DLSS model, NVIDIA's
`nvngx_dlss.dll`, is not part of the package: download it from NVIDIA's DLSS SDK repository
(https://github.com/NVIDIA/DLSS, folder `lib/Windows_x86_64/rel`) and put it beside
`ff7remake_.exe` in `End\Binaries\Win64`, or keep the one another DLSS mod (Luma) installed
there; without it the mod renders without DLSS (checked: the log says `NGX is not
available` and both eyes are rendered at the headset's full size). In the `-dlss` package DLSS is on
(`enabled = 1` under `[dlss]` in `ff7vr.ini`; `enabled = 0` turns it off); `input_scale`
sets the rendered share of each eye's width and height (0.65 in the package; 0.5 = a
quarter of the pixels). On the development PC (RTX 5080, no headset,
DLSS model L) 0.5 cost about as much as the game at full size (5 % more at 3072x3264 per
eye, between 9 % less and 4 % more at 3600x3600), with detail close to it and far sharper
than rendering at 0.5 without DLSS; hair and soft shadow edges are grainier than at full
size; 0.67 is calmer and closer to full size but costs about half as much again.
It has been played in a headset (Virtual Desktop at 3264x3072 per eye with `input_scale =
0.75`). Blocky dots away from the centre of the view, at that size, are foveated rendering's
coarsely drawn edges: DLSS keeps them sharp and, from a smaller rendered image, makes them
larger. `[foveation] preset = quality` removes them for about 0.5 ms per frame (measured
without a headset at 3264x3072 and 0.75: `performance` 9.6 ms, `quality` 10.1 ms, off 11.0 ms);
at 4032x3648 with 0.76 they are smaller. `[dlss] texture_bias = auto` (or `auto-1`) makes
textures a little crisper for 0.1 to 0.3 ms; it is off because DLSS's image is already more
detailed than the game's own at that size. Earlier `-dlss` packages could hang
the graphics card (within seconds at the title screen); that was a fault in the mod, fixed
in commit `ef2688a` (a package's folder name contains the commit it was built from; any
later one has the fix). It needs an NVIDIA RTX graphics
card with a current driver and the model file above. If the NVIDIA App's DLSS override
is set for this game, the App's model and preset apply instead of `[dlss] preset`. The App's
"DLSS override - Super Resolution" for this game must stay at the application's choice: if
it forces DLAA, DLSS cannot upscale and you see the game's own image (the log says why).
Packages without `-dlss` ignore the `[dlss]` section.

Since 06/10 the game renders each eye at `input_scale` of the headset's resolution (the
size Virtual Desktop asks for) and DLSS writes the headset's full resolution
(`output = runtime`, the default). So a sharper picture at about the cost of today's
rendering is: raise the resolution in Virtual Desktop and lower `input_scale` so that the
game still renders about 3072 pixels wide per eye. Measured without a headset on the
development PC: Virtual Desktop's 4032x3648 per eye with `input_scale = 0.76` renders
3064x2772 per eye and takes 11.9 ms per frame (the game at 3072x3264 without DLSS: 8.4 ms),
with 10 GB of video memory instead of 12 GB the earlier way; that fits 72 Hz, not 90 Hz.
5376x4992 with `input_scale = 0.57` takes 15.3 ms: too slow even for 72 Hz on that PC.
`output = engine` brings back the earlier way. Also since 06/10 the right eye no longer
shimmers when the head moves (its motion vectors were read wrongly by DLSS).

Virtual Desktop's performance overlay shows the share of the headset's resolution the
headset receives. With DLSS that is 100 %: DLSS's finished image at the headset's size.
The input scale (for example 65 %) shows only on frames without DLSS: the first frames
after the start, title frames without a 3D scene, and loading screens. So 65 % turning
into 100 % a few seconds after Continue is the end of the loading screen, not DLSS
stopping. The log's `projection layer:` lines record each change of what the headset
receives, and the `dlss:` line every 10 seconds counts the upscaled frames.
