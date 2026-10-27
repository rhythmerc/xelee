Xelee for Meta Quest 3 / 3S. You need your own Super Smash Bros. Melee NTSC-U 1.02 disc image.

## Changelog

**Smoother play**
- Fixed periodic hitches: every few seconds a frame could take too long and be held on screen.
- Dynamic resolution no longer drops to its lowest setting while a stage's shaders compile at the start of a fight, and climbs back as soon as they're done.
- Foveated rendering (FFR): a new setting (Off / Low / Medium / High) that draws the edges of each eye at a lower resolution, leaving the GPU more room. Medium by default.

**Roomtone lighting (Beta)**
- A new Lighting setting: Vanilla keeps Melee's own lighting; Roomtone lights the fighters and the stage with your room's light, measured through the headset's cameras. The cameras are only asked for when you pick Roomtone, after the launcher explains why.
- A short calibration before your first fight of each session: look at the bright lights in your room and Xelee counts them as it finds them. Run it again any time from Options > Graphics > Calibrate room light.
- Stages without their own lighting get shading from your room, fighters and metal stages pick up subtle highlights, and the light follows the stage when you move it.
- Roomtone takes more of the headset's power than Vanilla. The busiest fights (four players, scrolling stages like Icicle Mountain, and some Adventure stages such as Onett) may drop frames; switch Lighting to Vanilla if they bother you.

**Settings in the game**
- Options now has Graphics, Audio, Gameplay and Controls pages with the launcher's settings, so you can change them without leaving the game. Settings that need a restart offer one.
- Controls: remap what each Touch controller or gamepad input does in a fight (move, smash, shield, grab, jump, attack, special, taunt or nothing), separately per device type. Menus keep the usual buttons, and online opponents and replays see plain GameCube input.
- A Rumble switch for every port.
- Spatial audio can be switched on and off while the game runs.

**Slippi accounts in the game**
- Online Mode > Slippi has Login and Register pages. A new account goes through Slippi's setup steps in the game: verify your email, accept Slippi's online rules and choose a connect code. No Slippi Launcher needed.
- Fixed "please restart Slippi Launcher" on Unranked: it came from an account that had never accepted Slippi's rules, which the setup steps now handle.
- Clearer errors when Slippi needs a newer version.

**Mixed reality**
- Play from behind the stage: walk around to the other side and left and right swap during fights, and the HUD turns to face you.
- Recentering keeps the stage and the screen where they were.
- Peach's Castle's water splashes no longer float behind the castle.
- The launcher panel opens at Horizon OS's default size.

**Fixes**
- Pointing with a controller's laser no longer switches the active device away from your gamepad.
- Pick lists take a choice on the first tap.

## Installing

**On the headset, no PC needed:**
1. In the headset's browser, open this release page and download `Xelee-Quest-arm64.apk`.
2. Open the **Files** app and go to **Downloads**. Tap the three dots next to `Xelee-Quest-arm64.apk`, then **Open with**, then **Package installer**.
3. The first time, the headset asks whether Files may install apps: allow it, then choose **Install**.
4. Xelee is in your library under **Unknown Sources**.

**With a PC:** enable developer mode, then sideload `Xelee-Quest-arm64.apk` with SideQuest or `adb install Xelee-Quest-arm64.apk`.

## Updating

The launcher checks for new releases at each start (Settings → Launcher). Choose **Update Now**, then **Install**, and confirm in the system's dialog. The first time, the headset asks you to allow installs from Xelee.
