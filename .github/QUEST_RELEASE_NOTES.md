Xelee for Meta Quest 3 / 3S, Android phones and tablets, Windows PCs, Macs, Steam Deck and Linux PCs. You need your own Super Smash Bros. Melee NTSC-U 1.02 disc image.

## Changelog

### Capture the Flag

A new team mode on Hyrule Temple: grab the other team's flag and bring it back to your base.

- **Offline:** VS Mode › Xelee Melee › Capture the Flag. Red vs blue, any mix of players and CPUs.
- **Online:** XeleeNet and LAN lobbies can switch between VS and Capture the Flag with the **Mode** bar in Lobby Settings.
- **Rules:** captures to win (default 5), time limit (default 10:00), whether your own flag must be home to score, flag return time, and whether carriers are slowed.
- **New CPU behaviors for CTF.** They go after the enemy flag, guard and return their own, throw the flag into their base, and pass to teammates at higher levels. The CPU level (1–9) sets how well they play.

### CPUs in XeleeNet lobbies

- New Lobby Settings row: **CPUs: Off / Host / Anyone** (default Host).
- Press the button on an empty seat to add a CPU. Set its fighter, level, colour and team the same way as offline.
- A CPU never keeps a person out: the lobby list shows "+1 CPU", and anyone joining (online, or with a second controller) takes a CPU's seat.

### Netplay

- Fixed a desync in matches with 3 or 4 players.
- Fixed a desync between Windows and the other platforms.
- Auto delay now goes by the worst connection between any two players, jitter included, and holds steady through a match.
- When two guests connect to each other better through the host, Xelee routes them through it automatically.
- Lobby Settings changes apply about a second sooner.
- Leaving mid-match now sends everyone back to the character select screen instead of a stuck No Contest.
- Name tag selection disabled in XeleeNet (unintended feature that crashed sessions. Name is now locked to XeleeNet name as intended)

### GameCube adapter

- The official adapter (WUP-028) now works on Windows and Android.

### Headset

- The HUD's top row sits just above the damage row.

### Platforms

- Builds added for MacOS on Intel and Apple Silicon.

## Installation

### Quest 3/3S

1. In the headset's browser, open this release page and download `Xelee-Quest-arm64.apk`.
2. Open the **Files** app and go to **Downloads**. Tap the three dots next to `Xelee-Quest-arm64.apk`, then **Open with**, then **Package installer**.
3. The first time, the headset asks whether Files may install apps: allow it, then choose **Install**.
4. Xelee is in your library under **Unknown Sources**.

**With a PC:** enable developer mode, then sideload `Xelee-Quest-arm64.apk` with SideQuest or `adb install Xelee-Quest-arm64.apk`.

### Android phones and tablets

Download `Xelee-Android-arm64.apk` on the device, open it, and allow installs from your browser or Files app when asked. It needs a 64-bit (arm64) device with Vulkan. `Xelee-Quest-arm64.apk` is for the headset only.

### Windows

Download `Xelee-Windows-x86_64.zip`, extract it, and run `xelee.exe`. Keep the DLLs and the `resources` folder next to it.

### macOS

Download `Xelee-macOS-arm64.zip` (Apple Silicon) or `Xelee-macOS-x86_64.zip` (Intel), unzip it, and move `Xelee.app` to Applications. It needs macOS 13 or later.

The first time, macOS blocks it because it isn't notarized: open **System Settings → Privacy & Security** and choose **Open Anyway**. If macOS says the app is damaged, run `xattr -dr com.apple.quarantine /Applications/Xelee.app` in Terminal.

### Steam Deck

1. In Desktop Mode, download `Xelee-x86_64.AppImage` from this page and move it somewhere it can stay, such as a new `Applications` folder in your home folder. The updater replaces it in place.
2. Right-click it, choose **Properties**, then **Permissions**, and tick **Is executable**.
3. Right-click it again and choose **Add to Steam**. In Game Mode, Xelee is in your library under **Non-Steam**.
4. On first launch, choose your disc image in the launcher (the touchscreen works as a mouse). A disc image saved as `melee.iso` in your Documents folder is found without asking.

### Other Linux PCs (x86-64)

Download `Xelee-x86_64.AppImage`, make it executable (`chmod +x Xelee-x86_64.AppImage`) and run it. 

Alternatively: Download `xelee-linux-x86_64.tar.gz`, extract it, and run `run.sh`.

**GameCube controller adapter:** Linux needs a one-time rule before Xelee can open it. In a terminal (Konsole on the Deck, in Desktop Mode):

```sh
echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="057e", ATTRS{idProduct}=="0337", MODE="0666"' | sudo tee /etc/udev/rules.d/51-gcadapter.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

On the Deck, `sudo` needs a password: set one first with `passwd` if you never have.

## Updating

The launcher checks for new releases at each start (Settings → Launcher). Choose **Update Now**, then **Install**, and confirm in the system's dialog. The first time, the headset asks you to allow installs from Xelee.

On Steam Deck and Linux, choose **Update Now**, then **Restart to apply**: the new AppImage replaces the old one and Xelee restarts.

On Windows and macOS, **Update Now** saves the new zip to your Downloads folder: unzip it and replace your old copy.
