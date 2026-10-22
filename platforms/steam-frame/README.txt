Xelee for the Steam Frame
=========================

Melee in mixed reality, on the Steam Frame's own hardware. You need your own
Super Smash Bros. Melee NTSC-U 1.02 disc image; none ships here.

1. Unpack this folder on the headset, e.g. to ~/Applications/xelee.
2. Put your disc image (.iso, .gcm, .ciso or .rvz) in ~/Games or ~/Downloads,
   or beside xelee.sh; the launcher finds it there.
3. Add it to your Steam library: in Steam, Add a Non-Steam Game, browse to
   xelee.sh. Or, over SSH (Steam stopped meanwhile, as it rewrites its
   list when it quits):
     systemctl --user stop steam; python3 add-to-steam.py; systemctl --user start steam
   Start it from your library.

Point the controller at the launcher and pull the trigger (or press A) to
play. Set the headset to a fixed 120 Hz: Melee runs at 60, and at 72, 90,
108 or 144 Hz its 3D view shakes as you move your head. In a paused fight
the left controller's View button switches between your room and the
stage's own sky.

Logs are in logs/. Knobs go in melee-env.txt (see melee-env.example.txt).
More: https://github.com/rhythmerc/xelee/blob/main/docs/steam-frame.md
