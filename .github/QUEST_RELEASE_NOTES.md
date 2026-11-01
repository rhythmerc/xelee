Xelee for Meta Quest 3 / 3S, Android phones and tablets, Windows PCs, Steam Deck and Linux PCs. You need your own Super Smash Bros. Melee NTSC-U 1.02 disc image.

## Changelog

A small update to v0.3.0-beta: controllers on Android, and a way to send us your logs. New to XeleeNet? See [v0.3.0-beta's notes](https://github.com/rhythmerc/xelee/releases/tag/v0.3.0-beta).

**Controllers (Android)**
- The controller you press a button on now becomes player 1 when player 1's controller has never been used. Some phones list their own buttons as a controller: RedMagic phones show their shoulder triggers as an "Xbox 360 Controller", which took player 1 ahead of your real one. Your controller then couldn't type a XeleeNet name or play online. Controllers never swap during a fight, and the new order is remembered.

**Send logs**
- Settings > Launcher > Logs > Send now works on every platform. It uploads your recent logs and shows a code (XL-…) to give us with your bug report. Logs are kept 30 days, and public IP addresses are hidden.
- The logs now list your controllers and which player each one is.

**Windows**
- The game in the Windows zip is now `xelee.exe`.

## Installing

**On the headset, no PC needed:**
1. In the headset's browser, open this release page and download `Xelee-Quest-arm64.apk`.
2. Open the **Files** app and go to **Downloads**. Tap the three dots next to `Xelee-Quest-arm64.apk`, then **Open with**, then **Package installer**.
3. The first time, the headset asks whether Files may install apps: allow it, then choose **Install**.
4. Xelee is in your library under **Unknown Sources**.

**With a PC:** enable developer mode, then sideload `Xelee-Quest-arm64.apk` with SideQuest or `adb install Xelee-Quest-arm64.apk`.

### Android phones and tablets

Download `Xelee-Android-arm64.apk` on the device, open it, and allow installs from your browser or Files app when asked. It needs a 64-bit (arm64) device with Vulkan. `Xelee-Quest-arm64.apk` is for the headset only.

### Windows

Download `Xelee-Windows-x86_64.zip`, extract it, and run `xelee.exe`. Keep the DLLs and the `resources` folder next to it.

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
