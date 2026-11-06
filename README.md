# Xelee

An independent expansion of Melee featuring XeleeNet (online rollback multiplayer with customizable 2-4 player lobbies!), builds for Windows/MacOS/Linux/Android, and full mixed reality support on Quest 3/Quest 3S.

You need your own disc image. **No game data ships here.** The decompiled game
code is not licensed and is not relicensed by this project; only the port code
is GPL-3.0-or-later. Details under [License](#license).

[Discord](https://discord.gg/X4ZQjGTHj)

## Features

- Every stage and mode has been tweaked for mixed reality (Quest 3/3S builds)
- **Lobby-based online multiplayer** with XeleeNet
- **Slippi cross-play** with Unranked and Direct
- **Touch controller support**, with rumble
- **External gamepad support**, with rumble 
- **Hand tracking** for placing, turning and scaling the stage
- **Stage placement**: place the stage once with **Place Stage** and every
  stage starts there; any stage can be adjusted before its fight.
- **Laser pointer menus**: point and click through Melee's menus with the Touch controllers or hand tracking 
- **Asynchronous SpaceWarp (ASW)** boosts the base framerate of 60FPS to an apparent 120FPS
- **Dynamic resolution** to keep performance at a stable lock, ranging from 80% to 130% display resolution.
- **Spatial audio** makes sound play properly from fighters, and extra stage effects (WIP) give spatial sound effects to stages where vanilla is missing them
- **2xMSAA** anti-aliasing
- **UCF 0.84** as Slippi plays it (dashback, shield drop, SDI, shield SDI, tumble,
  DBOOC fix, 1.0 cardinals), Slippi's no-wobbling, neutral spawn and
  freeze-glitch rules, on by default offline (two toggles on the Gameplay page)
- **C-Stick in 1P**: the C-stick attacks in Classic, Adventure, Training and the
  other 1P modes, as in VS (on by default; a Gameplay toggle)

## Requirements

- A **Meta Quest 3 or 3S**. <!-- TODO: say whether Quest 2 / Pro are supported or untested -->
- Your own **Super Smash Bros. Melee NTSC-U 1.02** disc image (`.iso`, `.gcm`,
  `.ciso` or `.rvz`). The launcher checks it against the Redump database.
- **Developer mode** on the headset, to sideload the APK.
- A **Slippi account** for online play (the same one Slippi Launcher uses).

## Installing

1. Enable developer mode for your headset in the Meta Horizon app.
2. Install the APK from the
   [releases page](https://github.com/rhythmerc/xelee/releases) with
   SideQuest, or over USB: `adb install Xelee-Quest-arm64.apk`
3. Copy your disc image onto the headset (for example into `Download`, over
   USB or with `adb push`).
4. Open **Xelee** from your library (it's under *Unknown Sources*).

## AI Disclosure

Yes, Claude wrote every line of code. It also wrote sound effect generators, built the XR UI that I needed, ran sweeps for hiding stage pieces for MR play, did comparative analysis and fix work against Dolphin, ran extensive optimization rounds, and a million other things. I’m a software engineer by trade and I’ve made my peace with it. LLMs allowed me to develop this into a polished and optimized release that I’m incredibly proud of, and I hope the end result speaks for itself.

## First launch

The app opens as a flat panel in your home environment. Choose your disc with
the system file picker, let it verify, and press **Play**: the game starts in
your room on a floating screen. On the first launch (and after an update) the
launcher builds the game's shaders, which takes a few minutes; let it finish
for smooth first fights, or press **Play** to skip it.

Stages start small, a little below eye level and an arm's length ahead. Before
each fight the stage waits for you to place it (see below); you can turn that
off with **Before each fight** in the launcher.

## Placing the stage

The stage waits before each fight, with tip cards showing the gestures:

| | Hands | Controllers |
|---|---|---|
| Move | Point at the stage, pinch and drag | Point, hold grip and drag |
| Turn | Pinch with both hands and twist | Both grips and twist |
| Scale | Pinch with both hands, pull apart or together | Both grips, pull apart or together |

**A** or **Start** begins the fight, **B** resets the stage, and **Y** hides
the tips (they stay hidden until you restart the app). A paused fight lets you
grab the stage again.

**Place Stage** in Options (and in Online Mode) runs the same placement on a
practice Battlefield and keeps the spot for every stage as a baseline. Online matches can't
pause for placement, so Place Stage runs once on your first online match of
each session.

## Controls

Touch controllers play as player one:

| Touch | GameCube |
|---|---|
| Left stick | Control stick |
| Right stick | C-stick |
| A / B | A / B |
| X / Y | X / Y |
| Triggers | Analog L / R, digital past 90% |
| Either grip | Z |
| Left menu button | Start |

Point at the screen and pull a trigger to click a menu item. An external
gamepad plays alongside the Touch controllers.

## Playing online

Sign in with **Online Mode → Slippi → Login** (your Slippi email and
password), then pick **Unranked** or **Direct**. Direct uses connect codes, as
in Slippi. Matches are compatible with players on Slippi Dolphin: same rules,
same rollback netcode, same results.

Ranked, Teams and Party are not supported yet.

## Settings

The launcher's Quest settings include:

- **Resolution** and **SpaceWarp (ASW)**
- **Stage shadow**: the stage's shadow on the real surface under it
- **Before each fight**: place the stage before every fight, or start right away
- **Spatial audio** and **Expanded stage sounds**

Melee's own Options cover rumble per player (on the Touch controllers and your
gamepad) and **Place Stage**.

## Building from source

The Quest build cross-compiles the game with GCC from the Android NDK r26 and
Dawn with NDK r29; [docs/quest-xr.md](docs/quest-xr.md) covers the build, the
renderer and the tuning knobs, and [docs/building.md](docs/building.md) the
desktop builds. Aurora is a submodule, so clone with `--recursive`.

## Credits

Xelee is built on [melee-pc](https://github.com/999sian/melee-pc), a native PC
port of Melee from the [doldecomp/melee](https://github.com/doldecomp/melee)
decompilation, running on [aurora](https://github.com/encounter/aurora) (GX and
the Dolphin SDK on WebGPU through [Dawn](https://dawn.googlesource.com/dawn)) and
SDL3. Online play speaks [Slippi](https://slippi.gg)'s protocol and rules.
Spatial audio is [Steam Audio](https://valvesoftware.github.io/steam-audio/).

## License

Three situations, spelled out in [LICENSE.md](licenses/LICENSE.md): the decompiled
game code in `src/melee` and `src/sysdolphin` is **not licensed** and remains
the property of its copyright holders; the port code in `src/pc`, `tools`,
`platforms`, `cmake` and `.github` is **GPL-3.0-or-later** ([COPYING](licenses/COPYING));
bundled third-party components keep their own licenses. Because the game code
cannot be relicensed, the repository as a whole is not distributable under the
GPL. No game assets are in this repository.
