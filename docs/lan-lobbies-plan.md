# LAN lobbies: XeleeNet on this network (implementation plan)

LAN becomes a special case of XeleeNet: the same menu, Host page, Lobbies
list, Joining page, CSS lobby, Ready, leaving and stage picks, with the
local network standing in for the lobby service. The wireframe is
`lan-lobbies-wireframe.html` (tailnet share, melee-pc; its generator is in
session scratchpad `wf/wf_lan.py`). It draws only what differs from
XeleeNet's pages.

## Decisions

Made 2026-10-29:

1. **Where LAN lives:** Online Mode keeps its LAN row. It opens the XeleeNet
   menu (Host, Join, Name) and pages in a LAN mode that lists and hosts only
   lobbies on this network, with no service. LAN lobbies never show in the
   internet list (`docs/xeleenet-plan.md` decision "LAN stays separate"
   holds).
2. **The old LAN lobby goes away:** Start no longer pairs two players. Its
   tests (`net_test.py --lan`, `--scenes`) move to the new flow.
3. **Join by address:** when the network blocks discovery, the Lobbies page
   offers typing the host's address. It replaces `MELEE_LAN_DIRECT`.
4. **No codes or hidden lobbies on LAN:** every LAN lobby is listed, and
   players join from the list or by address. The Host page has no
   Visibility or Hide code row, the Lobbies page no Enter code, and the CSS
   banner no code pill.

From the wireframe's review (the recommendations, approved 2026-10-29):

5. The pages' left tab and the CSS banner read *LAN*.
6. The CSS banner shows the host's address pill on every player's screen.
7. Lobbies on another Xelee version are counted under the list, as on
   XeleeNet.
8. LAN asks for the XeleeNet name on the first visit, as XeleeNet does.

## What the code does today

| Area | Where | Today |
|---|---|---|
| Service client | `xeleenet.c` | `pc_xn_start` (host or join by code, over HTTPS on a thread), `pc_xn_members`, `pc_xn_set_roster` (the host's heartbeat), `pc_xn_lobby`, `pc_xn_lobby_info`, `pc_xn_browse`/`_poll`, `pc_xn_stop`, `pc_xn_error`. |
| Connect phase | `net_punch.c` | STUN, then the service phase (`pc_xn_start`), then signed probes, host intros, guest reports, the host's start, the two-step switch for roster changes. Its only service calls are the ones above, plus `pc_xn_url`. |
| Pages | `xeleenet_menu.cpp` | Host (H1), Lobbies (B1, a fetch with Refresh and "updated" time), keypad (J1), Joining, Failed; the CSS banner. |
| Online flow | `gmonlinemode.c` | `ONLINE_KIND_XN` is the XeleeNet flow; `gmOnline_XnFlow()` gates its CSS, Ready, leaving and stage select. `ONLINE_KIND_LAN` is the old lobby view (`lobbyFrame`, the counter screen). |
| LAN lobby | `net_lan.c` | mDNS/DNS-SD announce and browse of `_meleepc._udp` (IPv4 and IPv6, interface choice, compatibility by protocol/build/disc, goodbyes), then a host election and a 1v1 `pc_net_connect`. |

## The shape

XeleeNet's service client gets a LAN twin behind the same calls. Everything
that calls them, from the punch up to the stage select, stays as it is.

- **The host's game is the lobby service.** It makes the lobby's id,
  announces the lobby over mDNS, keeps the member list, and answers
  joins on its punch socket.
- **Joining** is a signed request to the host's punch socket (the address
  from the announcement, or one typed in), answered with what the service's
  join reply carries today: the lobby id, our port, the members and their
  LAN candidates, and the settings summary. Then the punch goes on as on
  XeleeNet, without STUN.
- **The lobby's secret** (what the probes and link keys derive from) comes
  from the lobby id the announcement carries: anyone on the network may
  join a LAN lobby, as decision 4 has it. The links' keys still mix in each
  player's nonce and key, as on XeleeNet.
- **The code's place is the lobby id.** Where XeleeNet's code addresses a
  lobby (`pc_xn_start`'s `code`, `pc_xn_lobby`), LAN passes the id from the
  announcement, or the address for Join by address.
- **mDNS stays net_lan.c's**, split out of the election: sockets, addresses
  and interfaces, announce, browse, goodbyes and the compatibility fields.

## Phases

Each phase ends with a test run that shows it working (repro before fix for
anything found on the way).

### L1 · The seam (small) — done 2026-10-29

Built as planned. `xeleenet_backend.h` is the table of calls; `xeleenet.c`
holds the service's and dispatches; `xeleenet_lan.c` is L2's stub (an empty
list, and a start refused with "LAN lobbies aren't ready yet"). On LAN the
punch skips STUN and offers only the LAN address. `MELEE_XN_NET=lan` runs it
without the menu, with no URL. Tests: `net_test_xn_menu.py` (3 players) and
`test_service.py` pass; a LAN host offers 192.168.0.57 only and fails on the
stub.

The plan as written:


1. A network choice for the run: `pc_xn_set_network(PC_XN_NET_SERVICE |
   PC_XN_NET_LAN)`, set by the menu (and `MELEE_XN_NET=lan` for headless runs).
2. `xeleenet.c` dispatches its public calls to the service code (as today)
   or to a new `xeleenet_lan.c`. No behaviour change on the service side.
3. `net_punch.c`: on LAN, no STUN and no URL. The candidates are the LAN
   addresses only.

Test: `net_test_xn_menu.py` (default 3-player run) and `test_service.py`
unchanged.

### L2 · The LAN lobby service (large) — done 2026-10-29

Built as planned, with these specifics:
- **mDNS:** `net_mdns.c` is a module of its own. `net_lan.c` stays as it is
  until L4 removes it; only its address text helper is shared. Records
  expire after 5 s or on a goodbye. If nothing is heard for 5 s, not even
  our own query looping back, multicast counts as blocked
  (`pc_xn_lan_unavailable`).
- **The host's game is the service:** a lobby socket on UDP 42424
  (`MELEE_XN_LAN_PORT`), or an ephemeral port when that's taken. The TXT
  keys are `v rev disc id name host tag seats taken port s`, where `s` holds
  the listing settings' JSON, parsed by `pc_xn_listing_settings`.
- **Messages:** JOIN, WELCOME and LEAVE are signed with the player's key.
  REFUSE gives `full`, `version` or `not_found`. A guest sends JOIN every
  300 ms and gives up after 6 s.
- **Joining:** by the listed code (the first 6 hex characters of the lobby
  id) or by `@address[:port]`. The punch's code field grew to 64 characters
  for the address form.
- **Timeouts:** a member the host stops hearing from is dropped after 20 s,
  as on the service. Browsing stops 15 s after the list last asks.

Tests:
- `net_test_n.py --lan`: 3 players (browse) and `--by-address` both pass.
- 4 players with `--block 2:3` passes: P3 and P4 relay through the host,
  and the other links are direct.

Two runs froze for 24-40 s in Aurora's `cache_stats()`, an old wedge rather
than the LAN code:
- **The cause:** the Dawn cache prune held the cache mutex while it marked
  every used row's `last_session`. That rewrote each row's blob, gigabytes
  per launch: 7.7 s with one instance, 24-40 s with four sharing the disk.
- **The fix (Aurora):** the stats got their own lock, and the last-use marks
  moved into a `cache_used` side table. The prune now takes 20 ms.

The plan as written:

1. **`net_mdns.c`**, out of `net_lan.c`: the sockets, the address choice, the
   announce timer, browse with expiry, the goodbye, and a TXT record API.
   Service type `_xeleenet._udp`.
2. **The lobby record** (TXT): protocol, build, disc, lobby id, name, host
   name#tag, seats and taken, the settings
   summary the list shows (mode, stocks, time, items, stage list, set format,
   XR, in game), and the punch port. It is re-announced when anything
   changes.
3. **Browse:** records become `PcXnListing` rows as they arrive, ours
   included (marked, so the list can leave it out). There's no fetch.
   Records on another build or disc go into `others`.
4. **Join:** a new punch message pair, `T_JOIN` (the guest's key, name,
   nonce, build, disc, the lobby id it saw, or none when joining by
   address) and `T_WELCOME` / `T_REFUSE` ("full", "not_found", "version",
   "closed"). The guest retries for a few seconds and then fails as a code
   would on XeleeNet.
5. **Members:** the host keeps the list the service keeps today (key, name,
   tag, port, candidates, `joined`), feeds `pc_xn_members`, and drops a
   member the roster leaves out, as the service does.
6. **Leaving and closing:** a guest's planned leave works as now (the
   pads). A host closing sends a goodbye record and stops answering. Guests
   already see that as the lobby closing.
7. **Join by address:** `T_JOIN` straight to `ip[:port]`, with no browse.

Test: `net_test_n.py --xn --lan` (3 players), joining from the browse and by
address. Then 4 players with a guest pair blocked (`--block`), as XN2.

### L3 · The pages (medium) — done 2026-10-29

Built as planned, with these specifics:
- **Entry:** Online Mode's LAN row sets the network to LAN and opens the
  XeleeNet sub-menu (Host, Join, Name). Back from it returns to the row it
  came from. The XeleeNet row sets the network back to the service.
- **Text:** a language overlay (`pc_lang_overlay`). While the network is
  LAN, `xeleenet.X` is read as `xeleenet.lan.X` first, so `en.yaml`'s
  `xeleenet.lan` holds only LAN's differing lines, and the C menus, the
  pages and the banner all follow without branching.
- **The list:** live (read every frame), with "Looking on this network..."
  for its first 3 s. X is always Join by address; the legend has no Y and
  no Refresh. The blocked state (B2) shows when
  `pc_xn_lan_unavailable()`.
- **Join by address:** a 3×4 keypad (digits, a dot, DEL). X deletes, B is
  Back, START joins once the address has four parts. The join goes to the
  punch as `@address`. A wrong or silent address fails with Edit address.
  A listed lobby that has gone fails as "That lobby closed".
- **The banner:** the address pill comes from `pc_xn_lan_address()` (the
  host's own address, or the one the guest reached), with `:port` only off
  42424.
- **Found on the way:** a browse alone never opened mDNS (L2's tests only
  joined by code), so the list stayed empty. Fixed: `browse_tick` opens it.

Tests and previews:
- `net_test_xn_menu.py --net lan` (3 players, no wrangler) passes. The
  first guest joins by typing 127.0.0.1, the second from the list; all three
  agree the stage and play the match with no DESYNC.
- The service run (`net_test_xn_menu.py`, 3 players) still passes.
- `tools/xn_preview.py lan-host lan-lobbies lan-empty lan-blocked
  lan-address lan-banner`.

The plan as written:

1. Online Mode's LAN row opens the XeleeNet menu with the network set to
   LAN (`ONLINE_KIND_XN`), with LAN's hint lines.
2. Lobbies: live rows, no Refresh, no "updated" time, no country column, no
   Enter code; *No lobbies on this network yet*; the discovery-blocked state
   with X Join by address.
3. Join by address: the code keypad's page with digits and a dot.
4. Host: the same rows without Visibility and Hide code.
5. The CSS banner: the brand (decision 5) and the address pill in the code's
   place (decision 6).
6. Text under `xeleenet.lan.*` in `resources/lang/en.yaml`. Previews in
   `tools/xn_preview.py`: `lan-lobbies`, `lan-empty`, `lan-blocked`,
   `lan-address`, `lan-host`.

Test: `net_test_xn_menu.py --net lan` walks Online Mode › LAN, hosts and
joins from the list, and plays a match with no DESYNC. No wrangler.

### L4 · The old LAN goes (small-medium) — done 2026-10-29

Built as planned, with these specifics:
- **Removed:** `net_lan.c`/`.h` (the election, the counter lobby, the old
  mDNS), `ONLINE_KIND_LAN` and its lobby tick, `MELEE_LAN_TEST`/
  `MELEE_LAN_DIRECT` and their fixtures in `vi.c` and `os.c`. Also gone: the
  old LAN's tools (`net_lan_test.py`, `net_lan_fuzz.py`,
  `net_lan_txt_test.py`, `test_net_lan_txt.c`), `net_test.py --lan/--scenes`
  with their drives and `check_scenes`, `test_net_set_flow.py`, and
  Android's device-name lookup (the old announce's hostname).
- **Kept, moved:**
  - The session calls (`pc_net_connect`, the reliable channel, the match
    handshake) are now in `net.h`.
  - The disc id is `pc_net_disc_id` in `net_disc.c`.
  - `net_addr_text` is in `net_wire.c`.
- **Exit:** `pc_xn_exit()` (main's shutdown, Android's disconnect) sends a
  LAN lobby's goodbye in place of `pc_lan_stop()`. The service's lobby is
  left to its timeouts, as before.
- **Coverage:** `--scenes` moved rather than lapsed.
  `net_test_xn_menu.py --scenario set` (1-minute matches from a rules file
  in the host's profile) plays the match out to the results, goes back to
  the CSS and plays a rematch, and asserts the same stages everywhere.
  `--net lan --players 3 --scenario set` passes.
- **Unit tests:** the same 17 of 51 fail as before L4 (netcode, pad and
  wobble tests, unrelated); `net_resume` and `net_reliable` pass.
- **Docs:** `docs/netcode-plan.md` §8 now opens with a note pointing here.

The plan as written:


1. Remove the election, the counter lobby view and `ONLINE_KIND_LAN`'s flow
   in `gmonlinemode.c` (`lobbyFrame`'s LAN branch, `pc_lan_*` callers).
2. `MELEE_LAN_TEST` and `MELEE_LAN_DIRECT` give way to `MELEE_XN=host|join:…`
   with `MELEE_XN_NET=lan` (and `join:ip:port`).
3. `net_test.py --lan` and `--scenes` move onto the new flow, or retire in
   favour of `net_test_xn_menu.py --net lan`.
4. `docs/netcode-plan.md` §8 points here.

### L5 · Every scenario on LAN, and a real network (small) — desktop part done 2026-10-29

Desktop, 3 players on LAN through the menus (`net_test_xn_menu.py --net lan`).
Each of these scenarios passes:
- `ready`, `leave-guest`, `leave-host`, `sss-host-b`, `sss-repick`,
  `sss-timeout`, `sss-same`, `css-b-hold`, `leave-host-alone`;
- L4's `set`.

On the way:
- **`ready`** was written for 2 players: its P2 was the last to ready, so
  with 3 no countdown ever started. Every guest readies now, and the first
  guest's B stops it.
- **Blocked discovery:** `MELEE_NET_SIM_NO_MCAST` makes mDNS hear nothing,
  as on a network that drops multicast. `--no-discovery` gives it to every
  instance. Each guest's list must say discovery is blocked (logged once:
  "xeleenet pages: LAN discovery blocked here"), and every guest joins by
  typing 127.0.0.1. It passes.
- **The LAN tab** (and Slippi's) went missing on the pages after the
  sub-menu started drawing its own title. Fixed in `993afbc3`.

Still to do: the acceptance on a real network, with the Quest and a PC on
the home network hosting both ways.


1. `net_test_xn_menu.py --net lan --scenario …` for `ready`, `leave-guest`,
   `leave-host`, `sss-host-b`, `sss-repick`, `sss-timeout` and `sss-same`.
2. A discovery-blocked run (a knob that closes the mDNS sockets) joining by
   address.
3. Acceptance: the Quest and a PC on the home network, hosting both ways.

## Order and size

| Phase | Size | Depends on |
|---|---|---|
| L1 The seam | small | — |
| L2 The LAN lobby service | large | L1 |
| L3 The pages | medium | L2 |
| L4 The old LAN goes | small-medium | L3 |
| L5 Every scenario, a real network | small | L3 |

## Risks

- **mDNS on Android:** the Quest needs a multicast lock and may filter
  multicast in sleep. Today's LAN lobby works on the Quest; the new service
  type and record sizes need checking there.
- **Firewalls:** Windows asks on the first listen. The old LAN had the same
  prompt.
- **Several interfaces** (VPNs, Tailscale, containers): today's address
  choice handles them. The punch's candidates must use the same choice.
- **Records go stale** when a host crashes: an expiry (5 s, as today) drops
  them from the list.

## Later

- The two-headset shared stage (`lan-sync-wireframe.html`) builds on LAN
  lobbies: sync and placing run in the CSS lobby before the first match.
- An internet lobby already uses LAN candidates for players on the same
  network; nothing here changes that.
