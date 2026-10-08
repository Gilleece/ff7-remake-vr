# ff7vr: VR for FINAL FANTASY VII REMAKE INTERGRADE (PC)

VR mod for FF7 Remake that adds first person, DLSSS and DFR. The main goal here was performance as I found other mods, amazing as they are, to be much more demanding than I would have expected. This is still early days, I made this for myself and am just sharing. This was primarily vibe coded, just for transparency. 

To install take a look at the "Playing" section of this readme. 

First person mode can be toggled with home on keyboard or L3 and R3 together on controller. The mod automatically switches to 3rd person for combat. There's a bunch of options to tweak in the ff7vr.ini file with descritions in that file. For me personally I get massively better performance out of this mod than others. I have only played through a few sections so almost certainly some areas will have issues etc, as I play through the game I will try to fix them.

The details (what the launcher does, a first-session checklist, the full lists of
known problems and untested areas, troubleshooting, performance) are in
`docs/guide.md`, which is `GUIDE.md` in the package folder.

## State

Tested on Quest 3 and Pico 4 ultra so far, any openXR headset should work but if there's specific headsets not working let me know and I will try to address that. 

## Requirements

- FINAL FANTASY VII REMAKE INTERGRADE on Steam, file version 1.0.0.7 (the current
  Steam build). Another build is refused by the mod: the game then runs without VR.
- Steam running.
- A PC VR headset with an OpenXR runtime (Virtual Desktop, SteamVR, Meta Quest Link,
  ...). The mod picks the runtime that has a headset connected by itself; to pin one,
  set `[xr] runtime` in `ff7vr.ini`. The PC's default OpenXR runtime is not changed.
- An NVIDIA graphics card for foveated rendering; on other cards the mod works
  without it.
- Windows 10 or 11.

## Playing

Steam only: 

Start your headset's PC app (for example the Virtual Desktop Streamer or SteamVR) and
connect the headset (this can also be done later), then double-click `start-vr.cmd` in the package folder. The launcher sets
ReShade/Luma aside (if you have them setup), copies the mod into the game folder, starts the game and waits;
when the game exits it puts everything back and keeps the session's log in `logs\`. the intention here was to allow people to keep their 2D setup intact.
Leave its window open while you play.

If a session was interrupted or anything looks wrong in the game folder, double-click
`restore.cmd` (harmless when nothing needs doing). Save games are not touched.

Non-steam (or if you want to launch through steam itself):

Without the launcher: unzip the `-dropin.zip` into the game's `End\Binaries\Win64`
folder (beside `ff7remake_.exe`) and start the game from Steam, or with
`ff7vr-start.cmd` there. The log is `ff7vr.log` in that folder; to remove the mod,
delete the files the zip added. Details (ReShade/Luma, DLSS): `GUIDE.md`,
"Installing without the launcher".

## Controls

The game's own controls are unchanged. The mod adds these; the keys work while the
game window has the focus, and on the gamepad you **hold View/Back and press** the
second button, or click both sticks together (the game does not see these
combinations):

| Keyboard | Gamepad | Effect |
|---|---|---|
| **Home** | **both stick clicks together (L3+R3)**, or hold View/Back + right stick click | switch between first and third person |
| **End** | hold View/Back + left stick click | **recenter**: the direction you face now becomes forward, and the HUD panel and the virtual screen move in front of you |
| **Insert** | hold View/Back + Menu/Start | 3D off (the game on the virtual screen) and on again. If the headset has lost the game (for example after Virtual Desktop or SteamVR was closed and opened again), the same key reconnects it |
| **Page Down** / **Page Up** | hold View/Back + D-pad down / up | HUD/menu panel 0.25 m nearer / farther |

## If something is wrong

After quitting the game, double-click `collect-diagnostics.cmd`: it zips the last
session's log, your settings and basic system facts into the package folder and
changes nothing. Send that with a sentence on what you saw and when. To narrow it
down, change one setting at a time:

- anything in 3D: `[stereo] enabled = 0` (if the virtual screen works, 3D is at fault)
- HUD or menus missing or cut off: `[ui] layer = 0`
- blocky edges or odd shading: `[foveation] enabled = 0`
- stutter: `[xr] resolution_scale = 0.8`

The full table is in the guide.

## Building from source

Visual Studio 2022 or later with the C++ workload; everything else is fetched on the
first build. One command builds a release and assembles the package in `dist\`:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package\package.ps1
```

Keep the checkout at a short path (for example `C:\src\ff7-remake-vr`): the
dependency build can exceed Windows' path length limit otherwise. Developer
documentation: `docs/dev-harness.md` (build, run, test without a headset),
`docs/engine-module.md`, `docs/render.md`, `docs/benchmarking.md`.

## Licence

MIT, see `LICENSE`. The third-party components the mod is built with, and their
licences, are listed in `THIRD-PARTY-NOTICES.md`.
