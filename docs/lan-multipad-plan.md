# More than one player per machine in a lobby

Plan, 2026-10-30. Not built yet.

**The case:** a PC on the TV has two or three gamepads, and a friend in a Quest
joins its LAN lobby. Today each machine in a lobby is exactly one player. A
PC's second controller does nothing in netplay, because netplay reads only
physical pad 0 (`capture_local_sample`, src/pc/net.c:4670).

The UX is in the wireframe `lan-multipad-wireframe.html` (on the tailnet
share). In short:
- START on a spare controller in the CSS takes the first open seat. The host
  gives out the seat, and everyone's CSS reloads.
- Hold B on that controller gives the seat back. The PC's own player holding B
  leaves with all of that PC's seats.
- On the stage select, each seat on a PC has its own cursor, and they all
  pick at once (the user's call, 2026-10-30).

Scope:
- **In:** LAN and internet XeleeNet lobbies. They share all of this code;
  only the Go service needs its own change (§3).
- **Out:**
  - Slippi, which stays 1v1 and is refused when a machine has extra seats.
  - A Quest with extra pads. The Quest keeps its rule of one gamepad on P1
    (`xr_scene.c:1400`). A TV view from the Quest isn't possible through
    Meta's casting; see §7.

## 1. Where the single-port assumption sits

The input rings and almost all of the CSS logic already work per port:
- `s_remote_ring[port]` and `s_remote_mask` hold remote input per port.
- `cssXnReadyFrame` reads Ready, un-ready, hold-B and team from every port's
  synced pad (`mncharsel.c:5780`).
- The desync checksum covers all four head ports.
- Recording stores all four ports.

The one-port assumption is in four places:

| Area | Where | What it assumes |
|---|---|---|
| Local input | `s_local_ring`, `s_raw_last`, `capture_local_sample` (net.c:66, 4670), `write_head` (4357) | one local ring, fed from physical pad 0 |
| Port ↔ link | `Peer.port`, `peer_by_port` (net.c:91, 629), `peer_port[]`, `PeerSpec.player`, `PcNetLink.player` | each link drives one port; 'F' drops an origin with no `Peer` (net.c:1696) |
| Seats | `seat()`/`taken()` (tools/xeleenet/lobby.go:92, 414), LAN seating (xeleenet_lan.c:287, 352), `s_who[port]` and `begin_session` (net_punch.c:91, 342) | one member = one port; a new joiner could be given a port that an extra pad holds |
| "My port" in menus | `pc_net_local_player()`: stage-select cursor and picks (mnstagesel.c:309, 528, 1555), leave (gmonlinemode.c:1773), legend, hold ring, own-name colour, rumble (pad_rumble.c:129) | one local port |

The `local == 0` "am I the host" tests (about 20 of them) and the nonce and
link-key derivation by port all keep working if `net.local` stays the
machine's **primary** port. The host always owns port 0.

## 2. Engine (M1)

- **The local seat list.**
  - `net.local` stays the primary port.
  - Add `net.local_mask`, plus `local_phys[port]`: the physical pad (the PC's
    controller number) that drives each local port.
  - Controller 1 is always the primary port. Extra seats get controllers in
    the order they claimed them.
- **Capture.**
  - `s_local_ring` becomes `[PORTS][RING]`, and `s_raw_last` becomes one per
    port.
  - `capture_local_sample` already reads all four pads; it keeps one sample
    per local port.
  - `s_wrote` stays shared, since every local port runs on the same frame
    timeline. So `last_acked`, the 'A' ack and `rx_have` stay per link,
    unchanged.
  - `write_head` tests `local_mask` rather than `== net.local`.
- **The wire (proto 13).**
  - The 'M' packet gains a `ports` mask byte. Each local port's pads then
    follow, back to back, sharing `first`/`count`/`newest`/`ck`/`seq`.
  - Worst case is 27 + 3 × 32 × 8 = 795 bytes. Raise the 512-byte receive
    buffer and `PACKET_WIRE_MAX` to 1200 rather than cutting redundancy (the
    comment at net.c:1062 explains why redundancy matters).
  - A 1-port packet stays the same size as today apart from that byte.
  - Rejected alternative: sending extra ports as 'F' streams. It would mean N
    datagrams per link per frame, a second ack path for our own input,
    `rx_have` moved off `Peer`, and no resend from `tx_timer`.
- **Ownership.**
  - Add `port_owner[4]` (port → peer index, or local) and `Peer.ports` (a
    mask). `peer_by_port` goes through the owner table, so 'F' relaying and
    `REL_DROP` resolve an extra port to its owner's link.
  - `on_inputs` loops over the packet's mask into `s_remote_ring[port]`.
  - Frame advantage takes the minimum of `s_port_have` over the peer's ports.
- **Drops and resume.**
  - `links_poll`/`drop_begin`/`REL_GONE`/`REL_VOID` act on every port of the
    dropped peer, not just its primary port. Today the extra ports would stay
    in `s_remote_mask` and stall the session until the timeout.
  - `Resume.have` is the minimum over owned ports and applies to all of them.
- **Rumble.** `pc_pad_rumble_command` maps a local port to `local_phys[port]`.
  Only the primary port goes through port 1's merged device (Touch, keyboard,
  GC adapter slot 1).
- **API.**
  - `pc_net_local_mask()` is added next to `pc_net_local_player()`.
  - `PeerSpec`/`PcNetLink` carry a port mask.
  - `MELEE_NET_PEERS` accepts `0+2=host:port` so tests can start a peer that
    owns two ports.
- **Slippi:** connecting with more than one local port is refused.

Test: `net_test_n.py --players 3 --owners 0,0,1`, meaning a host with P1 and
P2 and a guest with P3. It runs with scripted input on the extra port, at
50 ms / 2%, with `--kill` on the two-port peer, to check that both of its
ports end and the match is voided.

## 3. Seats (M2)

- **The punch's roster.**
  - `XnStart` gains `owners`, a byte that was reserved: 2 bits per port,
    giving the owner's primary port.
  - `begin_session` builds one link per owner, never per port.
  - `on_start` and `roster_changed` read the owner map.
  - `XnReport.want`, a byte that was reserved, carries a guest's request:
    - bits 0-1 hold the number of extra seats it wants;
    - bits 2-7 hold a number it moves on every change.

    The number lets the host ignore reports sent before a seat was given
    up. The report repeats every 250 ms, as it already did.
  - Each machine remembers which physical pad claimed each of its seats. A
    seat keeps its pad across sessions, and a new seat takes the oldest
    waiting claim (`pc_net_connect_links` takes the map).
  - A claim the host hasn't filled within 6 s is dropped. That happens when
    the lobby filled first, and the CSS then says so.
  - The host is also a machine that can add seats; it does so without a
    message.
- **Granting a seat.**
  - The host gives the lowest free port, puts it in `ports` with the owner
    map, and `roster_changed()` sees the change.
  - From there the existing two-step switch runs: START acked, switch at
    frame + 90, `begin_session`, CSS reload. `onEnterCss` seats the door from
    `pc_net_port_mask()`, which now includes extra ports, and deals teams by
    port as now.
  - A full lobby refuses the request; the guest shows the local "lobby is
    full" notice.
- **Giving a seat back.**
  - Every machine sees the hold of B on the same lockstep frame and calls
    `pc_xn_punch_seat_left(port)`:
    - The owner lowers its request.
    - The host marks that exact port dropped, so it isn't kept, and lowers
      the owner's count. It ignores reports that still carry the old
      number.
  - `gmOnline_XnLeave(port)` gains this case for a port in our
    `local_mask` other than the primary one. Today that call is a no-op for
    our own extra port, so the seat could never leave.
- **Both lobby services** learn the extra ports from the host's roster:
  `pc_xn_set_roster(keys, n, extra)`, where `extra` is a port mask.
  - **LAN host** (xeleenet_lan.c): the announcement's "taken" and the full
    check count the extra ports, and join seating skips them.
  - **Go service** (tools/xeleenet):
    - The lobby gains `Extra []int`, taken from the heartbeat's `extra`
      list.
    - `taken()` counts them, so the listing shows 4/4 and joins are refused.
    - `seat()` skips them.
    - The host is the authority, and the service only mirrors it, so the
      service doesn't need to know which member owns which extra port.
  - If a joiner ever lands on a port that holds an extra seat (a race with
    the heartbeat), the joiner wins. The extra seat moves to another free
    port, or waits for one.
- **Names:** `pc_xn_css_names` gives an extra port its owner's name plus the
  seat's number (RHYTHM 2), and marks every local port as ours.

## 4. CSS (M3)

- **Spare controllers.**
  - On each machine only, a physical pad that is connected but not in
    `local_phys` is watched with a raw local read, outside the synced head. A
    START press from it sends the seat request.
  - Nothing enters the simulation until the reload, so this can't desync.
  - The prompt on the first open door (A1 in the wireframe) is drawn by this
    machine alone, in the overlay.
- **Mask-aware helpers.** These go from `== pc_net_local_player()` to
  `local_mask`:
  - the legend (`xeleenet_menu.cpp:1731`);
  - the hold ring (`PcXnCssState.hold`/hand become per port);
  - the own-name colour (`mncharsel.c:6371`, 6409);
  - the controller tags on our doors.
- **Leaving:** primary hold-B means "we leave" as now (all our ports end with
  the session). Extra-port hold-B is a seat release (§3).

## 5. Stage select (M4)

The stage-select cursor isn't lockstep (`mnstagesel.c:1555`). Today there is
one per machine, driven by `pc_net_local_player()`, and its position lives in
single globals (`mnStageSel_804D6CAE` and friends).
- **A cursor per local seat that picks.**
  - The cursor model (`sss_models->cursor`, mnstagesel.c:1777) is loaded once
    more for each extra seat, in that port's colour, with a port tag.
  - Each cursor keeps its own position and hovered cell, and is moved by its
    seat's physical controller (`local_phys`).
  - The game's cursor stays the primary seat's, so 1-seat machines are
    unchanged.
  - Every seat gets a cursor: in XeleeNet's stage select every port picks
    (loser-picks isn't wired into it yet).
- **Picks per seat.**
  - `NET_MSG_STAGE_PICK` already carries `[pick, port]`; each seat sends its
    own.
  - A and B act on that seat's pick only.
  - The remote-pick filter `buf[1] != local` becomes "not in `local_mask`".
- **The hovered-stage name and preview** (bottom left) follow the primary
  seat's cursor, which is the game's own. Making them follow the cursor that
  moved last would mean writing the game's hovered cell from other cursors,
  and the primary seat's A reads that cell. Left for later.
- **"All picked"** already counts `pc_net_port_mask()`, so it waits for every
  seat.
- **Pick rules.** Loser-picks and losing-team-picks work unchanged, since the
  set of picking ports is computed per port.
- **The main work** is the cursor state. Moving the position globals into a
  per-port struct touches the SSS input proc, the hit test and the
  "what our cursor is on" ghost row (mnstagesel.c:934). This makes M4
  medium rather than small-medium.

## 6. Order and size

| Step | Size | Proof |
|---|---|---|
| M1 engine | medium | net_test_n with a two-port peer, the 1v1 and 3P suites still green |
| M2 seats (punch, LAN host, service) | medium | net_test_xn_menu `--scenario multipad --net lan`; test_service.py |
| M3 CSS | medium | the scenario's screenshots against the wireframe |
| M4 stage select | medium | `--scenario multipad-sss` (FFA, everyone picks) |
| Headset | — | a PC with 2 pads hosts on LAN, the Quest joins, 2 games |

Each step keeps the existing suites passing. Proto 13 means the Quest and the
PC need builds from the same revision, as they do now.

## 7. A TV view from the Quest (looked into, parked)

- **Meta's casting:** it can't show an image other than the eye view. The
  Spectator Camera is deprecated, wired DP output mirrors the left eye, and
  MediaProjection captures the compositor's output.
- **Streaming our own flat render:** this would bring back the flat pass we cut
  during fights (Fountain of Dreams went from 22.6 to 51.3 fps when it went;
  docs/quest-xr.md), and it adds 50–100 ms through a browser.
- **The only good TV route is a second copy of the game fed by inputs,**
  which is this plan.
- **Pads on the Quest, later:** if pads paired to the Quest ever matter, a
  one-way input spectator (the Quest streams final inputs, and a PC plays them
  like a replay) would do it.
- **Unchecked:** whether wired DP output shows up as an Android presentation
  display. `adb shell dumpsys display` with the cable plugged in would tell.
