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

### L2 · The LAN lobby service (large)

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

### L3 · The pages (medium)

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

### L4 · The old LAN goes (small-medium)

1. Remove the election, the counter lobby view and `ONLINE_KIND_LAN`'s flow
   in `gmonlinemode.c` (`lobbyFrame`'s LAN branch, `pc_lan_*` callers).
2. `MELEE_LAN_TEST` and `MELEE_LAN_DIRECT` give way to `MELEE_XN=host|join:…`
   with `MELEE_XN_NET=lan` (and `join:ip:port`).
3. `net_test.py --lan` and `--scenes` move onto the new flow, or retire in
   favour of `net_test_xn_menu.py --net lan`.
4. `docs/netcode-plan.md` §8 points here.

### L5 · Every scenario on LAN, and a real network (small)

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
