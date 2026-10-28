# XeleeNet: where we are and what's next

The thread to pick up after a break. The plan itself (requirements,
architecture, milestones, decisions) is `docs/xeleenet-plan.md`. This file is
the short version of the state, kept current.

Last updated 2026-10-28. Branch `feat/xelee-net` on the fork. Push to the
`fork` remote: `origin` is upstream and refuses with a 403.

## Done

| Milestone | Commits | What |
|---|---|---|
| XN0 | `8fc5a769` | The lobby service (`tools/xeleenet`), a Cloudflare Worker with Durable Objects. 43 checks in `test_service.py`. Not deployed. |
| XN1 | `65a1a87f`…`a697c3fa` | The engine plays 3-4 players: per-port rings, a `Peer` per link, host-led agreements, relaying through the host, voiding the match on a drop. |
| XN2 (desktop) | `012b208f` | Joining by code: STUN, signed punch probes, host introductions, per-link keys and routes decided before the session. |
| XN2 trial prep | `374a7163`, `f2425c11`, `efdd205e` | Hotspot punching (answer where a probe came from), Quest HTTP (cleartext switch, logged Java errors), the desync check re-arms per scene, `MELEE_NO_CARD`, the connect wait honours Ctrl-C, `tools/xn_trial.py`. |

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

## Next steps, in order

1. **Finish the 3-way trial** (above). If a pair can't punch, check
   whether it came out "through the host" and still played.
2. **The title-screen desync** (Quest vs desktop, `MELEE_DEBUG_VS`).
   - Find what drew the extra random number on frame 689. Compare the two
     sides' RNG call sites on that frame; the state dump prints only the seed.
   - It matters only if the real CSS flow shows the same thing. Rule that out
     during XN4.
3. **Deploy the service.** Cloudflare login or API token, plus the
   `XELEENET_TOKEN_KEY` secret. The CI deploys from `main` only. Until then,
   trials use `xn_trial.py serve`.
4. **XN3, the menus** (plan §4): XeleeNet rows above Slippi, the name, the
   Host page, the stage grid, Browse, Join with Code, join steps and errors,
   and the ask-to-join prompt.
   - The connect phase becomes asynchronous: `pc_xn_punch_poll` from the menu,
     not the boot-time wait in `pc_xn_punch_from_env`.
   - The host gets a start button instead of "starts when the seats are full".
5. **XN4, the lobby on the CSS** (plan §3.4-§3.5a): the banner and overlay,
   timed settings applies, the roster reload (joiners arriving into a live
   session, which reuse the punch on the session's socket via the datagram
   handler), the set score, and the placement barrier.
6. **XN5:** Windows HTTP (GET with a bearer token; WinHTTP only does POST
   today), the version gate, and logs.
7. **XN6:** a relay for a guest that can't reach the host. Today that
   guest waits 20 s and fails.

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
