# Steam Frame build

Xelee runs on the Valve Steam Frame as a native Linux arm64 game on the
headset's SteamOS, presenting through OpenXR to SteamVR on the headset. It is
the same game and the same aurora presenter as the Quest's
(docs/quest-xr.md); what differs is what the Frame's runtime offers, and the
presenter now asks rather than assumes. The Quest's build and behaviour are
unchanged.

## What it gets

| | Quest | Steam Frame |
|---|---|---|
| The room | `XR_FB_passthrough`'s layer | `XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND` (monochrome cameras) |
| Controllers | Touch (`oculus/touch_controller`) | its own (`valve/frame_controller_valve`), Touch emulation as a fallback |
| Hands | tracked, pinches | none: SteamVR's joints come from the held controllers, and are ignored |
| Display rate | 120 Hz requested | the player's choice in the headset's settings; 120 Hz recommended |
| GPU queue | a spare one for the runtime | Turnip may offer one; else shared with Dawn |
| Swapchains | no cap met | 16 a session: the optional pictures wait their turn |
| Stage shadow | on the surface found by depth raycasts | on the floor (`XR_EXT_local_floor`), where colour scale/bias is offered |
| Room lighting capture, boundary hiding, spatial anchors, colour grade | Meta extensions | not offered |
| Launcher | a 2D panel in Home | on the virtual screen in the headset, driven by the controller |
| Spatial audio | Steam Audio | Steam Audio, if built for linux-arm64 (see Building) |

Streaming from a PC (SteamVR over the Frame's adapter) offers no passthrough
to the app: there the game shows full VR (whole stages) by itself. That isn't
a target of this build, but the same presenter handles it.

## Installing

1. In the headset's settings, turn on Developer Mode if you want to deploy
   test builds over SSH (below); playing doesn't need it.
2. Unpack `Xelee-SteamFrame-arm64.tar.gz` (CI artifact, or built as below)
   somewhere in your home folder, for example `~/Applications/xelee`.
3. Put your disc image (`.iso`, `.gcm`, `.ciso` or `.rvz`, NTSC-U 1.02) in
   `~/Games`, `~/Downloads` or `~/Documents`, or beside `xelee.sh`. There is
   no file picker in the headset: the launcher takes the first one it finds,
   and Choose Disc steps through the others.
4. In Steam, add `xelee.sh` as a non-Steam game and start it from the library.
5. Set the display to 120 Hz: each of Melee's 60 frames a second then shows
   for two display frames. At 72 or 90 Hz the game keeps its own 60 Hz timer
   and the display repeats frames unevenly.

`melee-env.txt` beside `xelee.sh` takes the same knobs as on the Quest
(`melee-env.example.txt` lists a few). `MELEE_SKIP_LAUNCHER=1` boots straight
into the game with the last disc. Logs are in `logs/` (the last ten).

## Controls (Steam Frame controllers, port 1)

| Steam Frame | GameCube |
|---|---|
| Left stick | Control stick |
| Right stick | C-stick |
| A / B / X / Y (right) | A / B / X / Y |
| Triggers | Analog L / R, digital past 90% |
| Either shoulder button | Z |
| D-pad (left) | D-pad (taunt: up) |
| Menu (right) | Start |
| Either grip | Grabs the stage while placing it or paused, or the screen's bar; not Z |
| Either trigger, laser on the screen | Clicks (A) where it points |

The launcher: the D-pad or left stick moves, A or Menu picks, B goes back.
Placing the stage works as on the Quest (README, Placing the stage), its cards
naming the Frame's Menu button.

## Building

```sh
tools/package_steam_frame.sh        # dist/Xelee-SteamFrame-arm64.tar.gz
```

It builds in Valve's Steam Linux Runtime 3 (sniper) arm64 SDK, so the game
needs no newer glibc than the runtime's (2.31; the script checks), with GCC
14, the melee-xr Dawn fork (as the Quest build), Khronos' OpenXR loader
(shipped in `lib/openxr/`, used only when Steam's environment has none) and
`-DMELEE_STEAM_FRAME=ON` (OpenXR on from the start, the headset's launcher,
no updater). On an x86_64 machine it runs the SDK's container under QEMU
(binfmt_misc): a first build takes a few hours, mostly Dawn; later ones reuse
`build/frame-ccache`. CI builds it natively on an arm64 runner
(`.github/workflows/steam-frame.yml`).

Steam Audio ships no linux-arm64 library, so the package has no positional
audio unless one is built (its CMake supports Linux aarch64) and placed at
`build/deps/steamaudio-<version>/steamaudio/lib/linux-arm64/libphonon.so`.

## Testing

Without the headset, on a desktop: `tools/xr_monado_test.py` runs the XR build
against Monado with aurora's test layer giving it the Frame's personality
(alpha blending, its controller profile and the shoulder buttons' two names,
16 swapchains, rates reported or not) and checks the logs and frame dumps.
See docs/testing.md.

On the headset, with Developer Mode and SSH:

```sh
FRAME_HOST=steamos@<headset> tools/frame_deploy.sh deploy
tools/frame_deploy.sh env 'AURORA_XR_LOG_EXTENSIONS=1 AURORA_XR_DUMP=dumps'
tools/frame_deploy.sh run          # FRAME_APPID=<shortcut id> starts it through Steam
tools/frame_deploy.sh log
tools/frame_deploy.sh fetch /tmp/frame-run
```

What to read in the log at startup: the runtime and its extensions, the
environment blend modes, "OpenXR queue: ..." (a spare one, or Dawn's shared),
"Steam Frame controller bindings: ...", "Display at ... Hz", and every new
"Swapchains alive" peak. With a shared queue, the 10 s lines say how long the
XR thread waited for it and who held it.

## How the presenter tells (aurora lib/xr)

- **The room:** `xrEnumerateEnvironmentBlendModes`. With an `XR_FB_passthrough`
  layer, that; else ALPHA_BLEND, each frame starting with a clear 16x16
  projection layer (the Frame shows the room only behind one, and menus are
  quads); else none, and `aurora_xr_passthrough_available` turns false, so
  the game shows full VR (`src/pc/xr_scene.c`). `AURORA_XR_ROOM=auto|fb|blend|none`.
- **The queue:** `xr::device_created` after Dawn's device exists. No spare
  queue in Dawn's family: the runtime and the XR thread use Dawn's, and every
  Dawn entry point that can reach it is wrapped at link time to hold a lock
  the XR thread also takes (`lib/webgpu/queue_guard.hpp`); a submit translates
  its command buffers with Dawn's flush held, so only the flush holds the
  queue. Before all that, Turnip is asked for its emulated second queue
  (`tu_emulate_second_queue`). `AURORA_XR_SHARED_QUEUE=1` forces sharing.
- **Pacing:** the reported rate while `xrWaitFrame`'s period agrees with it,
  else the period: SteamVR can grant 90 Hz and run at 72.
- **Controllers:** `XR_VALVE_frame_controller_interaction` when offered,
  bindings tried in turn (bumper, then shoulder; then without the D-pad; then
  without haptics), and each hand's interaction profile followed so a held
  controller's joints never count as a bare hand.
- **Swapchains:** counted; optional ones within `AURORA_XR_MAX_SWAPCHAINS` or
  the count at which the runtime first refused one.
- **Defaults:** 2x MSAA on tile-based GPUs (the Frame's Adreno, on Linux).
