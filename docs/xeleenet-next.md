# XeleeNet: where we are and what's next

The thread to pick up after a break. The plan itself (requirements,
architecture, milestones, decisions) is `docs/xeleenet-plan.md`. This file is
the short version of the state, kept current.

Last updated 2026-10-28 (evening). Branch `feat/xelee-net` on the fork. Push to the
`fork` remote: `origin` is upstream and refuses with a 403.

## Done

| Milestone | Commits | What |
|---|---|---|
| XN0 | `8fc5a769` | The lobby service (`tools/xeleenet`), a Cloudflare Worker with Durable Objects. 43 checks in `test_service.py`. Not deployed. |
| XN1 | `65a1a87f`…`a697c3fa` | The engine plays 3-4 players: per-port rings, a `Peer` per link, host-led agreements, relaying through the host, voiding the match on a drop. |
| XN2 (desktop) | `012b208f` | Joining by code: STUN, signed punch probes, host introductions, per-link keys and routes decided before the session. |
| XN2 trial prep | `374a7163`, `f2425c11`, `efdd205e` | Hotspot punching (answer where a probe came from), Quest HTTP (cleartext switch, logged Java errors), the desync check re-arms per scene, `MELEE_NO_CARD`, the connect wait honours Ctrl-C, `tools/xn_trial.py`. |
| XN3 (most of it) | `237683e4`, `673d1b4f`, `7120fc98`, `242bb209` | Online Mode > XeleeNet (Host, Join, Name). RmlUi pages to the wireframe: H1 with presets, H2 stage grid, More rules, B1 browser (auto-refresh, Show full, scrolls, up to 50), J1 keypad, J2-J4 track and card, J3 failures. Button legend with GC glyphs. Connecting runs from the menu (`pc_xn_punch_poll`); the host starts with the first joiner. Settings persist (`xeleenet-lobby.txt`). Preview harness: `tools/xn_preview.py`. |
| XN4 (started) | `673d1b4f`, `7120fc98` | The CSS is the lobby: the C1 banner over the CSS's top strip, a door per port, the host's rules (teams byte in RULES), per-port stage picks drawn by seed. Roster reload: joiners arrive into a live session through a two-step switch (ack, then switch at frame + 90). |
| XN5 (pieces) | `b6d6da9b`, `76adaaa7` | Windows GET + bearer over WinHTTP (built, never run on Windows); CI passes `vars.XELEENET_URL` into builds. |

## The real-network trial (in progress)

Three players on three networks, with the service on this desktop instead of
Cloudflare:

| Device | Network | Reaches the service at |
|---|---|---|
| Desktop A (this one): host + `wrangler dev` | home | itself |
| Quest | home | A's LAN address, port 8790 |
| Desktop B | iPhone hotspot | A's tailnet address, port 8790 |

Port 8790, not 8787: the tailnet share listens on 8787.

**Results so far:**
- **Quest + A rehearsal.** Punched in 0.3 s, 2-7 ms, no loss. But a DESYNC
  at the title screen's Start: on frame 689 desktop A drew one more random
  number than the Quest, with the same inputs. The cause isn't found. It's in
  the `MELEE_DEBUG_VS` title shortcut only, not the real lobby flow. The two
  XR-only branches in `gmtitle.c` don't touch the RNG.
- **3-way attempt.** All three links punched direct, including Quest ↔ B
  across the carrier NAT. The session then ended at the first scene hand-off,
  because B's first boot sat in the boot scene on a memory-card problem. Fixed
  with `MELEE_NO_CARD=1` everywhere (`f2425c11`). **The 3-way match itself
  hasn't been played yet.**

**To rerun it:**
1. On A: `tools/xn_trial.py serve` (leave it running), then
   `tools/xn_trial.py host --seats 3 --wait 240`. Note the code.
2. Quest: `tools/xn_trial.py quest CODE --url http://192.168.0.57:8790 --wait 240`,
   then close and relaunch Xelee and press Play. The Quest needs the
   cleartext APK: `MELEE_VERSION=0.2.1-beta MELEE_GRADLE_ARGS=-Pmelee.cleartext=true sh build/quest-rebuild.sh`
   in the melee-xr distrobox, then `adb install -r dist/Xelee-Quest-arm64.apk`.
   It's installed now.
3. B: download the zip from the tailnet share
   (`tools/xn_trial.py bundle` rebuilds it), unzip it, and run
   `./xn_trial.py join CODE --url http://100.120.25.9:8790 --disc …rvz --wait 240`.
4. When all three show the title, press Start on A.
5. Afterwards: `tools/xn_trial.py quest-clean`, or every Quest launch tries
   to join.

**What to read afterwards:**
- The route per link: `xeleenet: P# direct|through the host`.
- Ping, loss and rollbacks: the `net: frame N` lines.
- Any `DESYNC`. The check now re-arms at each scene, so a fight desync shows
  even after a menu one.
- Desktop logs are `~/xn-trial-host.log` and `~/xn-trial-join.log`. The
  Quest's is logcat, and its buffer rolls over in minutes, so save it soon.

## Where we are (2026-10-28, evening)

We skipped ahead: XN3's menus and the start of XN4 were built before the
3-way real-network match was played (step 1 below was never finished).

**Proven:** on one desktop, 3 instances go Host → Join by code → CSS with 3
doors → shared stage → a match with equal checksums
(`tools/net_test_xn_menu.py --players 3`). The first join into a live CSS
reloads it (frame 119). Over real networks (XN2 trial): all three links
punched direct, Quest ↔ hotspot included.

**Built, not yet run:**
- The second join into a live session (the two-step switch; the first
  version deadlocked).
- B1 against a real service with real lobbies; scrolling past five rows.
- Every J3 failure on screen (seen only in the preview).
- A desync in a menu isn't reported as one: the checksum runs in matches
  only, so it shows as a scene hand-off timeout (`peer -1`).
- Windows XeleeNet at all; the Quest with the new pages (Touch on them too).

**Not built:** from XN3, ask-to-join, Stage pick, Placement time and
Format (the SOON rows). From XN4, settings changes from the CSS, remove /
leave / close, the set score, the placement barrier, chat (dropped for
now). From XN5, the version gate on screen, XR placement. XN6 (relay) has not
started.

## The CSS and stage select, after the first real run (2026-10-28)

The cross-network run (main PC + Quest at home, a PC on a phone hotspot) played
two matches with no desync. It showed four flow problems: any START started,
one B emptied the lobby, the stage list was ignored, and the stage select said
NOW LOADING. The wireframe (`xeleenet-ready-wireframe.html` on the share) is
approved; the build plan is `docs/xeleenet-ready-plan.md` (P1-P5). Fixed since:
re-hosting after a session (ddf8fe4f), team dealing (3ce227c4). XeleeNet's text
is in `resources/lang/en.yaml` (943161cc).

## Next steps: a friends' test (Windows + Quest, 3-4 players)

1. ~~**Re-verify locally**~~ (3 players passed 2026-10-28: the second join,
   B1 against the service, a Doubles 2v1 with equal checksums). 4 players
   next. Under KDE the test needs `XAUTHORITY=/run/user/1000/xauth_*`.
2. **The service on a Tailscale Funnel** (authorized): `xn_trial.py serve`
   (wrangler dev, port 8790) behind `tailscale funnel`. It's HTTPS, so the
   Quest needs no cleartext build.
3. **Builds with that URL:** set the repo variable `XELEENET_URL`, then build
   Windows in CI and run it on a Windows box before handing it out. Then the
   Quest APK with the URL.
4. **A 3-4 player run across networks:** read the routes, rollbacks and any
   DESYNC (see "What to read afterwards").
5. **Then:** deploy to Cloudflare for real; the title-screen desync only if
   the CSS flow shows one; the `net: fullhash` peer-local difference from
   frame 1440 (seen in clean runs too; `fix/camera-sim-reads` is unmerged
   until a desync reproduces); the rest of XN4.

The web build waits on WebRTC (browsers can't punch UDP); per-client cameras
are planned and deferred (`docs/client-camera-plan.md`).

## The CSS cards (built 2026-10-29)

In a lobby's CSS the doors of the seats the lobby has stand centred and the
rest are hidden (2 seats: 2 doors, 3: 3, 4 unchanged), and each player's name
is in their door's black box. The "nudge the cards down" idea was dropped.
- **Moving the doors** (`cssXnCardsFrame`, mncharsel.c): a door isn't a
  subtree of the CSS model, but its parts sit in groups of all four doors
  under the root (MELEE_CSS_DUMP: 40 backgrounds, 45 emblems, 50 costumes,
  55 team buttons, 60 sliders with joint 85, the black boxes, inside it; 86
  KO stars, 111 tag lists, 132/140/148/156 door frames, 164 HMN plates).
  Those groups, the name texts and the KO star texts move right by half a
  door spacing (15.4) a missing seat, every frame in the camera's proc, the
  last to run, after Melee's animations and writes: a value written since
  last frame is the new original. Doors past the seats are hidden
  (`cssHideDoor`, and their box's mesh object in joint 85).
- **The hand**: Melee tests it against fixed places for the team and HMN/CPU
  buttons; those tests take the hand back by the shift (`CSS_DOOR_X`). The
  sliders and the name box test against joints, which moved with the doors.
  Hands start under their own moved doors.
- **The seats** come from the host's settings or a guest's listing
  (`pc_xn_css_seats`), fixed for a CSS visit, never fewer than a seated port
  needs. They feed the hand's tests, so every player must have the same:
  each logs `css: N seats` and net_test_xn_menu checks they agree.
- **Names** (`pc_xn_css_names`): "NAME" of "NAME#TAG" in the box's SIS text,
  kerned, shrunk to fit its 160 units down to 62% of the size, then
  narrowed down to 50% of the width (WAVEDASH KID fits there), then cut with
  "...". Your own in your port's colour, the rest white. Characters Melee's
  font lacks show as '?'; '$' is its full-width pair. VS Mode's box (the tag,
  or the character's name) is untouched outside a lobby.
- **The team button works in a lobby now**: the door loop's XeleeNet skip
  (3ce227c4, for the HMN/CPU toggle) skipped it too; a Teams lobby couldn't
  change teams at all. Only the team button runs there (any door's, as in
  Melee).
- The overlay's tags, READY ribbons and the leave ring follow the doors
  (`PcXnCssState.seats`).
- Tests: `tools/xn_preview.py css-2 css-3 css-4` (full lobbies, a 12-letter
  name among them); `net_test_xn_menu.py --scenario teams` (Teams with a
  seat a player: the host's hand is steered onto its moved door's team
  button by the game's `css: P1 A at` lines, and every player must log the
  same `css: P1's team`).

## Open follow-ups from XN1 and XN2

- A silent player freezes the match for ~6.5 s before it's declared gone.
- A relayed reliable message that finds the lane full is lost (logged).
- Route detection assumes a link that fails, fails both ways.
- READY_BARRIER for more than two players (LAN), and guest link stats feeding
  the host's delay pick.
- Trial builds only: `-Pmelee.cleartext=true` must never be used for a
  published APK.

## Traps met on the way

- `pkill -f PATTERN` kills its own shell when the pattern is in the command
  line. Kill by PID.
- A Quest build installs only over the installed version code: set
  `MELEE_VERSION` to the installed version or newer.
- The JSON reader's strings point into the reply text, so the text must
  outlive the parsed tree (`xeleenet.c` keeps one buffer per thread).
- `/tmp` is a RAM disk shared by every session. Keep big test output in
  `build/`.
