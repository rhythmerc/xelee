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
| Hands | tracked, pinches | not tracked: hand tracking is off (SteamVR's joints come from the held controllers), the trigger clicks |
| Display rate | 120 Hz requested | SteamVR's: it picks among 72, 90, 108, 120 and 144 Hz and refuses requests; keep it at 120 Hz |
| Application SpaceWarp | on (the loop at half the display's rate) | off: SteamVR switched the loop between full and half rate every few frames |
| GPU queue | a spare one for the runtime | Turnip's emulated second one (`tu_emulate_second_queue`); else shared with Dawn |
| Swapchains | no cap met | 16 a session: the optional pictures wait their turn |
| Layers in a fight | the room's, the shadow, the HUD, the 3D view | the 3D view first (the only layer SteamVR reprojects), the HUD above it; no stage shadow |
| Room lighting capture, boundary hiding, spatial anchors, colour grade | Meta extensions | not offered |
| Launcher | a 2D panel in Home | on the virtual screen in the headset: the laser and trigger, or the D-pad and A |
| Room or sky | the launcher's Room setting | the same, and View (left controller) in a paused fight |
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
4. Add it to Steam's library: Steam's *Add a Non-Steam Game*, browsing to
   `xelee.sh`; or over SSH with Steam stopped (it rewrites its list when it
   quits), `systemctl --user stop steam; python3 add-to-steam.py; systemctl
   --user start steam` (what `tools/frame_deploy.sh shortcut` does). Start it
   from the library.
5. Keep the display at 120 Hz: each of Melee's 60 frames a second then shows
   for two display frames. SteamVR's automatic rate moves it among 72, 90,
   108, 120 and 144 Hz while a game runs and refuses the game's requests (it
   logs "refused although offered"); at any rate but 120 the game's frames
   land unevenly on the display's and the 3D view shakes as the head moves.

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
| View (left), in a paused fight | Switches the room and the stage's own sky (saved as the launcher's Room setting) |

The launcher: the laser and trigger, or the D-pad or left stick to move, A or
Menu to pick, B to go back. View does nothing mid-fight or in menus.
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

Steam Audio ships no linux-arm64 library: the script builds one from its
source (`tools/build_steam_audio.sh`, the release's tag; its CMake supports
Linux aarch64) into
`build/deps/steamaudio-<version>/steamaudio/lib/linux-arm64/libphonon.so`.
`FRAME_STEAM_AUDIO=0` skips it, and the package then has no positional audio.
The libraries the game needs beyond glibc, zlib and the Vulkan loader go in
`lib/` beside it (the headset's SteamOS lacks curl's GnuTLS build, and the arm64
runtime it has installed is the Steam Linux Runtime 4's). No OpenSSL: the disc
check's SHA-1 is the game's own here, as on macOS and the Quest (the SDK's
OpenSSL is 1.1, which the headset lacks and whose licence doesn't go with the
GPL's); the script fails if anything links it again.

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
  layer, that; else ALPHA_BLEND, each frame starting with a projection layer
  (the Frame shows the room only behind one): in a fight the 3D view itself,
  see-through where nothing drew, and in menus (all quads) a clear 16x16 one;
  else none, and `aurora_xr_passthrough_available` turns false, so the game
  shows full VR (`src/pc/xr_scene.c`). `AURORA_XR_ROOM=auto|fb|blend|none`.
- **Layer order:** SteamVR reprojects only a frame's first layer for the head.
  The 3D view under the clear room layer flickered between three places (60
  game frames on a 72 Hz display: drawn one to three display frames before);
  first, it holds (`policy::fight_layer_first`, `AURORA_XR_FIGHT_FIRST`). The
  HUD goes above it, and the stage's shadow quad, which would cover the stage,
  is left out.
- **The queue:** `xr::device_created` after Dawn's device exists. No spare
  queue in Dawn's family: the runtime and the XR thread use Dawn's, and every
  Dawn entry point that can reach it is wrapped at link time to hold a lock
  the XR thread also takes (`lib/webgpu/queue_guard.hpp`); a submit translates
  its command buffers with Dawn's flush held, so only the flush holds the
  queue. Before all that, Turnip is asked for its emulated second queue
  (`tu_emulate_second_queue`). `AURORA_XR_SHARED_QUEUE=1` forces sharing.
- **Pacing:** the reported rate while `xrWaitFrame`'s period agrees with it,
  else the period: SteamVR can grant 90 Hz and run at 72. A period change
  counts once it has lasted 12 frames (SteamVR reports a held frame's now and
  then). A runtime listing one rate is asked for 120 Hz until it refuses one
  it offers (SteamVR does). Application SpaceWarp is off on SteamVR
  (`AURORA_XR_APPSW=1` turns it on).
- **Controllers:** `XR_VALVE_frame_controller_interaction` when offered,
  bindings tried in turn (bumper, then shoulder; then without the D-pad; then
  without haptics; the first with the View button), and each hand's
  interaction profile followed so a held controller's joints never count as a
  bare hand. Hand tracking is off where the Frame's controllers are offered
  (`AURORA_XR_HANDS=1`), so the trigger clicks.
- **Swapchains:** counted; optional ones within `AURORA_XR_MAX_SWAPCHAINS` or
  the count at which the runtime first refused one.
- **Defaults:** 2x MSAA on tile-based GPUs (the Frame's Adreno, on Linux).

## Tested on

A Steam Frame with SteamOS 0.5.6, SteamVR 2.18.2 and Mesa 26.3's Turnip on
the Adreno 750 (2026-10-22/23): the launcher on the virtual screen (laser,
trigger, D-pad), the disc found in `~/Downloads`, menus clicked with the
trigger, placing with the Frame's cards and grips, fights in the room and in
full VR, View switching them in a pause, the 3D view steady with both eyes
at 120 Hz, Turnip's second queue, at most 12 swapchains alive, and the game
started from Steam's library, with the 12-frame period settling and SpaceWarp
off by default. No Quest was at hand:
its paths are unchanged by construction (each new one is chosen by something
the Quest's runtime doesn't offer), held by aurora's policy tests' Quest
cases, and its build compiled (`tools/build_android.sh`'s native library).
