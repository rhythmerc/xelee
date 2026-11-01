Xelee for Meta Quest 3 / 3S, Android phones and tablets, Windows PCs, Steam Deck and Linux PCs. You need your own Super Smash Bros. Melee NTSC-U 1.02 disc image.

## Changelog

XeleeNet, Xelee's own online play with custom lobbies for two to four players.

Windows, Linux, and Android flat builds are now available! Play Xelee with your friends whether they're in or out of a headset.

**XeleeNet (Beta)**
- A one-time message on the way to the main menu announces XeleeNet.
- Online Mode > XeleeNet: host a lobby or join one, for 2 to 4 players. Public lobbies are listed in Join. A private one opens only with the 6-letter code its host shares.
- Every fighter and stage is unlocked in XeleeNet and LAN lobbies, as in Slippi. Your own unlocks come back when you leave.
- Pick a name the first time (XeleeNet > Name). It's XeleeNet's own, separate from Slippi's.
- Hosting starts from a preset (1v1, Doubles or Free-for-all) or your own rules: seats, teams, stocks or time, items, friendly fire, pause, handicap, damage ratio and Frozen Stadium.
- Fighter and stage lists: the host chooses which fighters may be picked (all of them, the high or low tiers, or your own choice) and which stages are in play. Random draws only from those.
- Press A on a lobby in Join to see its players, fighters and stages before joining.
- The character select is the lobby. Each player's START is their Ready. Hold B to leave on your own; when the host holds B, the lobby closes. If you join during a game, you're seated when the players are back on the character select.
- Lobby Settings (Z on the character select): the host changes the rules without closing the lobby.
- Stage select: everyone picks and one of the picks is drawn, with a 15-second timer. Only the host's B goes back.
- More than one player per machine: press START on a spare controller at the character select to take an open seat. On the stage select, each seat gets its own cursor.
- Hide code masks the code on the character select's banner, for streaming.
- If a player drops mid-game, that game doesn't count.
- Players connect to each other directly where their networks allow it, and through the host where they don't.
- Every player needs the same Xelee version. Lobbies on another version aren't listed, and the list tells you how many there are.
- Not ready yet, shown as SOON: other stage-pick modes, placement time, First to N sets and Ask before joining.

**LAN**
- Online Mode > LAN now works like XeleeNet on your own network: a live list of the lobbies there, with Host, Join and the same character select, settings and stage select. No codes: every LAN lobby is listed.
- If your network blocks discovery, the list tells you, and X joins the host by its address instead.
- The old LAN lobby is gone. Games on this version can't play LAN with older ones.

**Headset**
- While a game waits on the network, the headset keeps drawing the scene, so you can look around it. Before, it froze on one image that SpaceWarp smeared.
- Menus, results and Classic's screens show on the virtual screen at its full resolution. Before, they were scaled up from 480 lines.

**New platforms**
- Windows (x86-64), Android phones and tablets (arm64, Vulkan), Steam Deck and other Linux PCs (AppImage) builds come with every release. They're new, so tell us what breaks. Install steps are below.

**Fixes**
- Clearing 100-Man Melee brings Falco's challenge again, and 10-Man and 100-Man clears and best times are saved to your records (from v0.2.2).
- Fixed a rare crash when a match loaded Pokémon Stadium's sounds.
- The game no longer freezes for several seconds while it tidies its shader cache.
- Online Mode's sub-menus no longer flash Main Menu on the way in and out, and tab titles animate with Melee's own.

**Also**
- Options > Credits: who and what Xelee is built from.
- The Android app icon is the new Xelee badge.

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
