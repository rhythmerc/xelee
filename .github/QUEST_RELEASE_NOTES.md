Xelee for Meta Quest 3 / 3S, Steam Deck and Linux PCs. You need your own Super Smash Bros. Melee NTSC-U 1.02 disc image.

## Changelog

A Roomtone update: lighter on the headset, far less shader stutter (Classic and Adventure included), and glowing stages.

**Roomtone lighting (Beta)**
- Lighter on the headset: Roomtone shades the stage per face, with the room's light worked out per vertex. Regular matches on Onett, and getting close to any stage, now hold their frame rate much better with it.
- Glowing surfaces keep their glow in a dark room: Battlefield's orb and edges, Brinstar's acid and lights, Corneria's glows, Mute City's signs and neon, Pokémon Stadium's screen. Brinstar's acid, Battlefield's orb and Kongo Jungle's waterfall light the stage around them.
- Kongo Jungle's waterfall no longer shows its polygons.
- Retuned brightness, with per-stage tuning for Corneria, Big Blue, Onett and Pokémon Stadium.
- The room's light stays put while you walk around during a pause.
- Changing Lighting now applies after a restart, and the game offers one. Switching mid-game left the next fight to prepare Roomtone's shaders while it played, skipping parts of the scene.
- Roomtone takes more of the headset's power than Vanilla. The busiest fights (four players, scrolling stages like Icicle Mountain, and some Adventure stages such as Onett) may drop frames; switch Lighting to Vanilla if they bother you.

**Smoother play**
- The launcher prepares far more of the game's shaders before you play: both lightings, every stage and fighter, and now Classic and Adventure. After you press Play, let it count down instead of pressing Play again, and first fights stutter or skip parts of the scene far less. The first launch after this update takes longer while it does this.
- Surfaces a fighter's shadow can fall on (Corneria, Yoshi's Story, Green Greens, Onett) no longer switch lighting once a match starts, which prepared shaders mid-match.

**Fixes**
- Updating: after allowing installs from Xelee the first time, the update now installs without pressing Install again.

## Installing

**On the headset, no PC needed:**
1. In the headset's browser, open this release page and download `Xelee-Quest-arm64.apk`.
2. Open the **Files** app and go to **Downloads**. Tap the three dots next to `Xelee-Quest-arm64.apk`, then **Open with**, then **Package installer**.
3. The first time, the headset asks whether Files may install apps: allow it, then choose **Install**.
4. Xelee is in your library under **Unknown Sources**.

**With a PC:** enable developer mode, then sideload `Xelee-Quest-arm64.apk` with SideQuest or `adb install Xelee-Quest-arm64.apk`.

### Steam Deck

1. In Desktop Mode, download `Xelee-x86_64.AppImage` from this page and move it somewhere it can stay, such as a new `Applications` folder in your home folder. The updater replaces it in place.
2. Right-click it, choose **Properties**, then **Permissions**, and tick **Is executable**.
3. Right-click it again and choose **Add to Steam**. In Game Mode, Xelee is in your library under **Non-Steam**.
4. On first launch, choose your disc image in the launcher (the touchscreen works as a mouse). A disc image saved as `melee.iso` in your Documents folder is found without asking.

### Other Linux PCs (x86-64)

Download `Xelee-x86_64.AppImage`, make it executable (`chmod +x Xelee-x86_64.AppImage`) and run it. It needs a Vulkan driver. `xelee-linux-x86_64.tar.gz` is the same build as a plain folder: extract it and run `run.sh`.

**GameCube controller adapter:** Linux needs a one-time rule before Xelee can open it. In a terminal (Konsole on the Deck, in Desktop Mode):

```sh
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="057e", ATTRS{idProduct}=="0337", MODE="0666"' | sudo tee /etc/udev/rules.d/51-gcadapter.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

On the Deck, `sudo` needs a password: set one first with `passwd` if you never have.

## Updating

The launcher checks for new releases at each start (Settings → Launcher). Choose **Update Now**, then **Install**, and confirm in the system's dialog. The first time, the headset asks you to allow installs from Xelee.

On Steam Deck and Linux, choose **Update Now**, then **Restart to apply**: the new AppImage replaces the old one and Xelee restarts.
