# XeleeNet plan: lobbies

XeleeNet is Xelee's own online service. It sits next to Slippi and LAN in the
Online menu. The first pillar is **lobbies**:

- Players host and join public or private lobbies of **up to four**.
- The host sets the lobby's settings before hosting.
- The host can change those settings later from the character select screen.

Where the work stands and what comes next: `docs/xeleenet-next.md`.

This document has the requirements, the technical plan and the milestones.
The UI wireframe is a separate page (`xeleenet-lobby-wireframe.html` on the
tailnet share). Open questions are at the end.

Status: design agreed 2026-10-27. XN0 (the service) built 2026-10-27, XN1 (the
N-player engine) and XN2 on desktop (join by code) 2026-10-28.

**User decisions (2026-10-27):**
- **Four seats in v1.** The engine's multi-player work happens as part of this.
- **The CSS settings page is an overlay outside the sim.**
- **No passwords.** The lobby code does the job: a private lobby is reachable
  only by its code.
- **A player dropping mid-game voids that game** for everyone.
- **Names are XeleeNet's own**, not prefilled from Slippi. They don't have to
  be unique: the install key is the identity.
- **XeleeNet goes above Slippi** in Online Mode.
- **LAN stays separate.** LAN games never show in the XeleeNet browser.
- **Stage placement happens before every match,** as in 1P, with a "waiting
  for placement" state for flat players and for headset players who are
  done. The global placement before entry stays, as for Slippi and 1P. The
  Slippi-era Place Stage row isn't carried over.
- **Placement has a host-set timer, 15 s by default.** Placement ends
  automatically when it runs out. **With nobody in a headset, there's no
  hold.**
- **Ask before joining and Hide code stay** as optional host settings.
- **Stage pick defaults:** loser picks in 1v1, the losing team picks in
  Doubles, and everyone picks in Free-for-all.

## 1. What we already have

These findings come from surveys of `main` at 270e810c.

### 1.1 The native engine (LAN)

It already does a 1v1 match end to end.
- **Transport:** raw UDP, a 7-byte header, a BLAKE2b MAC per datagram,
  protocol v11 (`net_internal.h:146-250`, `net.h:45`).
- **Rollback:** a 64-frame ring with a 7-frame window. Time sync and auto delay
  use Slippi's formulas. Desync checksums, a watchdog, and a 3 s RESUME.
- **Reliable channel:** two stop-and-wait lanes (`net_reliable.c`). Caller
  types `>= 0x10` go through `pc_net_send_reliable` / `pc_net_recv_reliable`.
  It carries the scene hand-off (0x14), chat (0x15) and the stage pick (0x20).
- **Handshake:** the host sends RULES and the guest answers READY
  (`net_handshake.c`).
  - RULES carries `GameRules` (friendly fire included), the item frequency and
    mask, the stage mask and Frozen Stadium.
  - A RULES that arrives after the handshake is dropped
    (`net_handshake.c:28-30`), so rules are fixed for the whole session today.
- **Synced scenes:** lobby → CSS → SSS → VS → results → CSS all run on synced
  inputs (`GM_ONLINE`, `gmonlinemode.c`).
  - The vanilla VS CSS runs on those synced pads. So the Teams toggle, team
    colours and doors are already deterministic on the native path
    (`mncharsel.c:3101`).
  - The scene hand-off agrees an exit frame between peers
    (`pc_net_scene_hold`, `net.c:2003`).
- **Taking over a socket:** `pc_net_connect_socket` takes over an
  already-punched socket (`net.c:2800`).
- **Identity:** an Ed25519 key per install, with a code `NAME#XXXXXXXX`
  (`net_identity.c`).

### 1.2 Everything is built for exactly two peers

Three layers assume it.

**Session and rollback core**
- `NetSession` has one `peer` address and `local`/`remote` (`net_internal.h:344-351`).
- There is one remote ring with have/newest/acked (`net.c:64-71`).
- `confirmed_frame()` clamps to that one ring (`net.c:331`). `predict()`
  repeats one remote's pad (`net.c:3120`).
- `write_head` forces ports 2-3 to no controller (`net.c:3310-3330`).
- The desync check compares one checksum ring (`net.c:1559`).

**Wire and agreements**
- The receiver drops any header whose player isn't `net.remote` (`net.c:1318`).
- `'M'` and `'A'`, the reliable lanes, RESUME and BYE are per session, not
  per peer (`net_reliable.c:65`, `net.c:1663`, `:2229`).
- There is one MAC key, derived from two nonces (`net_wire.c:228`).
- The handshake waits on one READY (`net_handshake.c:524`).
- Time sync assumes a symmetric pair (`net_sync.c:305`).
- Auto delay is decided from the host's single link (`net_sync.c:185-254`).
- The scene hand-off and READY_BARRIER each wait on one peer (`net.c:1955`,
  `net_lan.c:1200`).
- A peer leaving ends the whole session (`net.c:3666`).

**Game side**
- Local port is P1 for the host and P2 for the guest (`gmonlinemode.c:51-60`).
- The stage pick is a coin flip between two picks (`mnstagesel.c:267-286`).
- Chat labels lines "You"/"Opponent" (`net_chat.c`).
- The HUD shows one ping (`ifnet.c:59`).
- The Direct won/lost/rematch logic is two-party (`gmonlinemode.c:1042`).

### 1.3 The Slippi side is half multi-player

- **Already multi-remote:** the session core (`PcSlippiSession`, up to 3
  remotes).
  - Per-remote rx queues, acks, ping, offsets and `remote_active`
    (`slp_session.h:54-112`).
  - The time offset takes the minimum across remotes
    (`slp_session.c:413-460`).
- **Still single-peer:**
  - The ENet driver holds one peer (`slp_net.c:14`, `:113-122`).
  - `net.c` always passes `remote_count = 1` (`net.c:2591`).
  - The ticket parse rejects more than 2 players (`net_match.c:823`).
- **Slippi Teams is a full mesh:** every client dials every address in the
  ticket.

### 1.4 Other limits

- **No server of ours.** No STUN, no ICE, no relay. Two different home NATs
  were never tested.
- **No spectators.**

### 1.5 UI we can reuse

- **Online menus:** native pills in VS Mode's red frame (`mnonline.c`).
- **Slippi pages:** RmlUi content inside the red frame, with a native tab and
  hint box (`mnslippi.c`, `slippi_menu.cpp`). They bring the step track, gold
  line work, chips and notes.
- **Settings row engine** (`settings_page.hpp`). It's tied to Options through
  `mnPage`, so lobby pages need their own host, as `mnslippi.c` is.
- **Name Entry keyboard,** callable from anywhere, the CSS included
  (`mnNameNew_PcOpen`).
- **The rules bar:** on the VS CSS it opens Melee's Rules menu
  (`mncharsel.c:3056-3080`). Online, it's disabled only for Slippi.

## 2. Requirements

Priority: **v1** = first release, **v1.x** = soon after, **later** = planned
but not scheduled.

### 2.1 Identity

| ID | Requirement | Pri |
|---|---|---|
| ID-1 | No account, email or password. The install's Ed25519 key is the identity. | v1 |
| ID-2 | A display name (1-12 chars, Name Entry charset), picked on first entry to XeleeNet and changeable later. It starts empty: no Slippi prefill. Names **don't have to be unique**. The install key is what identifies a player (bans, roster, seats). The short `#XXXX` after the name is the key's fingerprint, so two players with the same name can be told apart. | v1 |
| ID-3 | The server authenticates each request by a signature over a server nonce. No secret ships in the APK. | v1 |
| ID-4 | Losing the key (a reinstall) gives a new identity. Export and import of the key. | later |

### 2.2 Hosting

| ID | Requirement | Pri |
|---|---|---|
| H-1 | Online › XeleeNet › **Host Lobby** opens a settings page before the lobby exists. | v1 |
| H-2 | A preset row at the top. **Competitive 1v1:** 2 seats, 4 stocks, 8:00, items off, legal stages, loser picks, first to 2. **Doubles:** 4 seats, Teams, 4 stocks, 8:00, friendly fire on, legal stages, losing team picks, first to 2. **Free-for-all:** 4 seats, Time 2:00, items on, all stages, random. **Custom.** Changing any rule turns the preset into Custom. | v1 |
| H-3 | Lobby settings: name (default `<NAME>'s lobby`), visibility (Public / Private), **seats (2-4)**. | v1 |
| H-4 | Mode: **Free-for-all / Teams**. Friendly fire (Teams only). | v1 |
| H-5 | Match rules: Stock / Time, stock count, time limit, handicap, damage ratio, items (off, or frequency + item switch), pause (on / off). Values and limits as in Melee's Rules menu. | v1 |
| H-6 | Stages: a legal-stage list (presets Competitive, All, Custom → a stage grid page). A stage pick rule, offered by mode (§3.6): Loser picks (2 players or Teams), Everyone picks → random among the picks, Random from the list, Host picks. Frozen Stadium (on / off). **Placement time:** 10, 15, 20, 30, 45 or 60 s, default 15 (S-6). | v1 |
| H-7 | Set format: **Endless** (a win tally) or **First to N wins** (N = 2, 3). In a 1v1 the banner shows First to 2 as "Bo3". | v1 |
| H-8 | The last settings used are remembered across launches (`xeleenet-lobby.txt`). | v1 |
| H-9 | START (or **Open lobby**) creates the lobby and goes to the CSS. Hosting fails with a clear reason: offline, server down, version too old. | v1 |
| H-10 | Every lobby has a 6-character code (no 0/O/1/I). A private lobby is reachable only by its code. | v1 |
| H-11 | **Ask before joining** (host setting, off by default): each join waits for the host's accept on the CSS. | v1 |
| H-12 | **Hide code** (host setting, off by default): the banner masks the code, and the host holds a button to reveal it. For streaming. | v1 |

### 2.3 Joining

| ID | Requirement | Pri |
|---|---|---|
| J-1 | **Browse Lobbies** lists public lobbies: name, host tag, seats taken/max, mode and a rules summary, region, an XR mark if any player is in a headset, and "in game" when a game is running. | v1 |
| J-2 | The list hides lobbies from other builds (proto/rev/disc mismatch) and counts them in a footer. Full lobbies are hidden behind a "Show full" toggle. | v1 |
| J-3 | Sorted by region closeness to us, then by newest. A refresh button, plus auto-refresh every 10 s while open. | v1 |
| J-4 | **Join with Code** types the code on the Name Entry keyboard (capitals and digits). Also reachable from Browse (Y). | v1 |
| J-5 | Joining shows its steps (Find · Connect · Lobby) and names each failure: code not found, lobby full, closed, version mismatch, declined by host, couldn't connect. | v1 |
| J-6 | Joining a lobby whose game is running waits on a "Game in progress" screen until the game ends, then seats you at the CSS. | v1 |
| J-7 | A measured ping per listed lobby. | later |
| J-8 | Invite a recent player (a Recent tab). | later |

### 2.4 In the lobby (the CSS)

| ID | Requirement | Pri |
|---|---|---|
| L-1 | The CSS is the lobby. It's the synced VS CSS LAN uses, with one door per seat. Empty seats read "Open seat". Seats beyond the lobby's size are shut. | v1 |
| L-2 | The top banner shows the lobby name, mode and rules summary, set score and the code (masked if H-12). | v1 |
| L-3 | The **host** clicks the banner to open **Lobby Settings** (H-2…H-7, plus visibility and the join options). A guest who clicks it gets a read-only view. | v1 |
| L-4 | While the host has the page open, guests see "Host is changing settings…" and can't ready up. Their CSS keeps running. | v1 |
| L-5 | Applying the settings lands on the **same sim frame** on every peer. It clears everyone's ready state and shows what changed. | v1 |
| L-6 | Settings can't change from the SSS through the results. A changed mode or set format restarts the set score. | v1 |
| L-7 | In Teams, each player picks a team on their own door (vanilla CSS). Only the host switches Free-for-all/Teams, from the banner page or the CSS's own toggle. | v1 |
| L-8 | **Seat changes happen at the CSS only.** When someone joins or leaves, every peer reloads the CSS on an agreed frame. Everyone keeps their character, colour and team. Ready states clear. | v1 |
| L-9 | The host can remove (and ban, for this lobby) any player from their door. | v1 |
| L-10 | B held on the CSS leaves the lobby. If the host leaves, the lobby closes for everyone. | v1 |
| L-11 | **A player dropping mid-game ends that game for everyone,** with no result. The rest return to the CSS without that player, and the lobby stays open. | v1 |
| L-12 | After a game everyone returns to the CSS. The set score updates. A finished set shows its winner on the banner until the next game. | v1 |
| L-13 | Quick-chat names the sender ("PIXEL: Good games"). | v1 |
| L-14 | A public lobby leaves the browser when its seats are full and comes back when one opens. | v1 |
| L-15 | Host migration: the lobby survives the host leaving. | later |
| L-16 | A dropped player's fighter is removed and the game goes on (Slippi's `remote_active` model). | later |

### 2.5 Network and service

| ID | Requirement | Pri |
|---|---|---|
| N-1 | Match traffic is peer-to-peer on the native engine, with auto delay (LAN keeps delay 0). | v1 |
| N-2 | Topology: **mesh where it can, through the host where it can't** (§3.3). Every guest must reach the host. Guest-to-guest links fall back to forwarding by the host. | v1 |
| N-3 | Set-up uses UDP hole punching: STUN for the public mapping, plus the LAN address for players behind the same NAT. | v1 |
| N-4 | A relay for a guest who can't reach the host. Without it, J-5 says "couldn't connect". | v1.x |
| N-5 | The lobby service is HTTPS + JSON only (curl on desktop, the existing JNI path on Quest). | v1 |
| N-6 | Lobbies expire 30 s after the host's last heartbeat. | v1 |
| N-7 | Per-key and per-IP rate limits on host, join, list and code lookups. | v1 |
| N-8 | Lobby names: Name Entry charset, ≤ 20 chars, and a server-side word filter. | v1 |
| N-9 | Free-tier hosting in normal use (Cloudflare, like the log drop). | v1 |
| N-10 | Logs carry the lobby id, the roster, and each link's route and stats, so "Send logs" covers XeleeNet failures. | v1 |

### 2.6 Platforms

| ID | Requirement | Pri |
|---|---|---|
| P-1 | Quest, Linux and Windows in v1. Any mix of them in one lobby. | v1 |
| P-2 | On Quest, the global stage placement auto-runs before the first XeleeNet entry of a launch, as for Slippi and 1P. The XeleeNet menu has no Place Stage row (it's still in Options). | v1 |
| P-3 | Every page works with Touch lasers, a gamepad and a GC adapter. | v1 |
| P-4 | LAN gets 3-4 players from the same engine work (up to 3 guests in its election). | v1.x |

### 2.7 Stage placement before each match

| ID | Requirement | Pri |
|---|---|---|
| S-1 | A match starts with a **placement hold**, as in 1P, **when at least one player is in a headset** and set to place ("Place the stage", the launcher default). All players' games load the stage and stop before the first tick, on the same frame. With nobody to place, there's no hold: the match starts as any netplay match does. | v1 |
| S-2 | A headset player places that match's stage with the 1P hold: cards, legend and gestures. A/Start says "ready". A headset player set to "Start right away" doesn't place; they count as flat for S-1 and S-3. | v1 |
| S-3 | A flat player is ready at once and sees **"Waiting for placement"**, listing who's still placing, and the time left. | v1 |
| S-4 | A headset player who's done sees the same waiting card in the headset. They can **B: adjust again** until the match starts. | v1 |
| S-5 | The host releases the hold when everyone is ready **or the timer runs out**. A 3·2·1 countdown follows, and every game ticks frame 0 together. | v1 |
| S-6 | The **placement time** is a lobby setting (H-6), 15 s by default. Every player sees it count down. When it runs out, placement ends automatically. Anyone still placing keeps their stage where it is, which is safe because placement is presentation only. | v1 |
| S-7 | Sudden Death never holds (as in 1P). | v1 |
| S-8 | Placement never reaches the sim or the wire. Only "ready" bits travel. | v1 |

### 2.8 Not in v1

- Spectators, rotation queues and "winner stays".
- Ranking or matchmaking queues.
- Free-text chat.
- Cross-play with Slippi Dolphin.
- Slippi Teams. It gets easier, because the engine core is shared (§3.2), but
  it isn't part of this work.

## 3. Architecture

```
  Xelee (host, P1)                XeleeNet service            Xelee (guests, P2-P4)
  ───────────────                 (CF Worker + DOs)           ─────────────────────
  STUN → public ip:port
  POST /lobbies ────────────────▶ LobbyDO(code) ◀──────────── GET /lobbies, POST …/join
  heartbeat (2 s) ◀────────────── joins, candidates ───────▶ join reply: everyone's candidates
                        ┌────────── host ══ guest (always direct) ──────────┐
  P2P links:            │  guest ══ guest  (direct if punched, else via host) │
                        └────────────────────────────────────────────────────┘
  native engine: N-peer rollback, host-led agreements (RULES/READY, scene exit, settings, roster)
```

### 3.1 Service

> **2026-10-30: rewritten as a Go service** (`tools/xeleenet`, one static
> binary, in-memory state with a save on shutdown, its own STUN). The goal was
> no dependence on one vendor: it runs on any VPS or container host. The API
> and every behaviour below are unchanged. Only the Durable Object mechanics
> (alarms, wall-clock billing) no longer apply. The README is current; the
> rest of this section is the original Worker design.

**Cloudflare Worker + Durable Objects.** We already deploy a Worker (the log
drop, `tools/log-drop`, `.github/workflows/log-drop.yml`). The lobby service
is `tools/xeleenet/`, deployed the same way (`.github/workflows/xeleenet.yml`).

As built in XN0 (`tools/xeleenet`; its README has the details):

- **`Lobby`** (Durable Object): one per lobby. It holds the record, the
  members and their candidates, the "Ask before joining" queue and the ban
  list, and it decides every join. An alarm closes it 30 s after the last host
  heartbeat.
- **`Directory`** (Durable Object): one instance. It hands out codes, resolves
  them, keeps one open lobby per host (hosting again closes the old one), and
  holds a summary row per lobby for the browser. Each `Lobby` pushes its row
  when something the browser shows changes.

Lobby record:

```
id, code, name, public, ask, compat {proto, rev, disc}, state (open | playing)
host     { key, name, tag, xr, candidates, geo, port 0 }
members  [ { key, name, tag, xr, port 1-3, candidates, country, joined } ]
banned   [ key ]
settings { preset, seats, mode, win, stocks, time_min, items, stage_list,
           pick, format, first_to, place_secs }      the browser's summary only
score    [ per port or team ]                        host-written
```

**API** (JSON over HTTPS; a signed session token on every call, §3.7):

| Call | Who | What |
|---|---|---|
| `POST /v1/session`, `/v1/session/verify` | all | Nonce challenge, signed answer, a 1-hour token and the tag |
| `GET /v1/lobbies?proto&rev&disc[&full=1]` | browser | Public summaries on the same build, nearest first; other builds counted |
| `POST /v1/lobbies` | host | Create. Returns `{id, code}` |
| `POST /v1/lobbies/{id}/heartbeat` | host | Keep alive, every 2 s at the CSS and 10 s in a game. Sends the state, the guests the host's game reaches, the score. Returns members with candidates, and pending joins |
| `PUT /v1/lobbies/{id}/settings` | host | Mirror the settings summary for the browser |
| `POST /v1/join` | guest | By code. Candidates in; returns a port and every member's candidates, or "pending", or a refusal |
| `GET /v1/lobbies/{id}/join` | guest | Ask again after "pending" (waits ≤ 20 s for the host's answer) |
| `POST /v1/lobbies/{id}/admit` | host | Accept or decline a pending join (H-11) |
| `POST /v1/lobbies/{id}/kick` | host | Remove and ban a key for this lobby |
| `DELETE /v1/lobbies/{id}` (or `POST …/close`), `POST …/leave` | host, guest | Close, leave |

**Only the host polls** (changed from a long-poll by every member, 2026-10-27).
Durable Objects bill wall-clock time while a request is open, so members
long-polling would keep every lobby running all the time. Instead:
- a joiner gets everyone's candidates in its join reply;
- the host learns about the joiner from its next heartbeat;
- the host's game tells the other members over the peer links, as it already
  leads every other roster change (§3.5).

The free plan covers roughly 50-100 lobby-hours a day; the $5 plan, far more.
Hibernating WebSockets for the host are the next step if it grows.

The service never carries match traffic and never decides game state. The
host owns the roster and the full settings over P2P (§3.4). The service only
mirrors a summary so it can list and route.

### 3.2 The engine goes N-peer

This is the largest piece. It's shared by LAN and XeleeNet, and later by Slippi
Teams through the adapter.

**Core**
- A `Peer` struct per remote: address, route, link key, seq/RTT, acks,
  `last_acked`, reliable lanes and resume state. It replaces `net.peer` /
  `net.remote`. Local port 0-3; a present-mask of ports.
- Remote rings become `[4][RING]`, with per-port have/newest/acked.
- `confirmed_frame`, the stall test and the snapshot condition take the
  **minimum over present ports**.
- `predict` runs per missing port. The rollback trigger is indexed by the
  sender's port.
- `write_head` and `head_check` loop over 4 ports with the present-mask.
- Checksums: one ring per peer, any mismatch is a desync. `check_desync`
  names the port.
- `slp_frame_cb` uses its `remote_slot`. That's where Slippi Teams would plug
  in later.

**Wire** (protocol v12)
- The header's `player` must be in the port set and match that peer's address.
- `'M'` carries **per-origin pad blocks**, so forwarded inputs (§3.3) travel in
  the same packet type.
- Each link sends its own unacked window. `'A'` means "newest contiguous frame
  of yours I hold", per link.
- Link keys: BLAKE2b(session, both nonces, both pubkeys) for each pair.
- RESUME and BYE are per link. A BYE from the host ends the session. A BYE from
  a guest triggers §3.5.

**Agreements: the host leads them all**
- **RULES/READY to N guests.** The host waits for every READY. Every READY must
  name the same `start_frame`, and re-issues go to all. `hs_lead` is sized
  from the worst ping.
- **Scene hand-off and READY_BARRIER.** Each peer sends its exit frame to the
  host. The host announces `exit = max(all) + SCENE_HANDOFF`.
- **Time sync and auto delay.**
  - There's a per-peer advantage ring. Each peer acts on its **worst** peer, as
    Slippi does: the minimum trimmed mean (`slp_session.c:413-460`).
  - Guests report their link stats to the host, and the host announces the
    delay from the worst link.
- **Settings apply (§3.4) and roster changes (§3.5)** use the same
  host-announced agreed frame.

**Game side**
- Ports by seat: the host is P1, guests take P2-P4 in join order. A seat keeps
  its port until that player leaves.
- Doors: present ports are human; the rest are "Open seat" or shut.
- `mnCharSel` keeps the vanilla Teams toggle, gated to the host's port.
- The stage pick carries the sender's port and becomes `pick[4]` (§3.6).
- The chat payload gets a sender port, and lines show names.
- The HUD shows the worst ping, with a per-player list on the pause screen.
- Results and the set score are tallied per port, or per team.
- The VS splash shows all players.

### 3.3 Topology: mesh, with the host as fallback

- Every guest connects to the host. This connection is required: the host
  leads every agreement.
- Every pair of guests tries a direct link too. A pair that can't punch
  through within 8 s falls back to **forwarding by the host**. The host
  re-sends that pair's `'M'` pad blocks on its own links. This is why `'M'`
  carries per-origin blocks.
- Why mesh first: guest-to-guest inputs over a direct link save one hop. With
  4 players, the 3 guest pairs are half the links.
- Why the host fallback: a lobby never fails because two *guests* can't reach
  each other. The only hard requirement is guest ↔ host, which joining tests
  anyway.
- Cost: the host forwards a few kB/s per failed pair. Acks on a forwarded pair
  are end to end, so the RTT measures the real path.
- The route of each link is logged (N-10). The Lobby Settings page shows a
  small link table for the host ("PIXEL ↔ KOOPA via host").

### 3.4 Settings: before hosting and from the CSS

- **Data.** One `XnLobbySettings` (`src/pc/xeleenet_lobby.h`). It converts to
  and from the RULES payload. New fields: seats, mode, pick rule, set format,
  placement time, preset and `settings_rev`.
- **Before hosting.** A page in the red Online frame, hosted like the Slippi
  pages (`MENU_KIND_XN_PAGE`). It uses the settings row engine. "Stages…"
  opens a stage grid page.
- **From the CSS** (user decision: the overlay).
  - The host clicks the banner and the same page opens as an RmlUi overlay
    over a dimmed CSS.
  - While it's open, the host's port feeds neutral input to the sim, so the
    host's cursor rests. The host broadcasts `REL_XN_EDITING` (0x16) for L-4.
  - **Apply:**
    1. The host sends `REL_XN_SETTINGS` (0x17) `{rev, settings}` to every
       guest.
    2. Each guest answers with its current frame.
    3. The host announces `apply = max(all) + SCENE_HANDOFF` (0x18).
    4. Every peer applies the settings inside the tick at that frame:
       `GameRules`, the masks, the Teams flag, clearing the ready states and
       the toast. A rollback that re-runs the frame applies them again.
  - The settings are journalled with the frame. `settings_rev` goes into the
    frame checksum, so a missed apply shows up at once.
- **Lowering the seat count** below the number of players present isn't
  allowed. The host removes someone first.

### 3.5 Roster changes: join, leave and drop

The synced CSS has a fixed set of ports, so a roster change is a **CSS reload
at an agreed frame**:

1. A joiner punches to every member, and its links come up. While that
   happens the CSS keeps running for everyone, and the joiner's screen shows
   "Joining · connecting to 3 players".
2. The host announces `REL_XN_ROSTER {rev, ports, frame}`, using the same
   agreement as the settings apply.
3. Every peer leaves the CSS scene at that frame, re-runs RULES/READY with the
   new port set and re-enters the CSS. Character, colour and team carry over
   from the lobby state. Ready states clear. That's about a second, under a
   "PIXEL joined" card.
4. A **leave** at the CSS works the same way: BYE to the host, a roster
   announcement, a reload.
5. A **drop mid-game** (decided): the host announces "game void", and every
   peer leaves the match at an agreed frame. There are no results and no set
   point. Then comes a roster reload without that player.

   Later, perhaps: Slippi's `remote_active` model, with neutral inputs from
   frame F on, then fighter removal. That needs every remaining peer to agree
   on the last frame they hold of the dropped player.
6. **The host dropping** closes the lobby (host migration is later).

A joiner who arrives during a game (J-6) has its links set up, waits, and is
seated by the roster reload when everyone is back at the CSS.

### 3.5a Stage placement before each match

Today there's no hold in netplay. `pc_xr_place_hold()` bails on
`pc_net_active()` (`xr_place.c:350`), because a peer would start the match
without waiting. XeleeNet makes the hold a **network barrier** instead.

- **Where.** The same hook as 1P: the scene loop's sim-free hold
  (`gmscene.c:458`), at the match scene's stage load, before tick 0. Every
  peer reaches that point on the same sim frame, because the scene hand-off
  agreed the scene start. Holding there changes no sim state on anyone.
- **Whether to hold.**
  - Each peer keeps the host told whether it **wants to place**:
    `REL_XN_PLACER` (0x1C) `{wants}`, sent on change. `wants` = an XR session
    is presenting and the player's setting is "Place the stage".
  - With the SSS exit, the host sends `REL_XN_MATCH_PLAN` (0x1D)
    `{match_seq, hold, seconds}`. `hold` = anyone wants to place.
  - It arrives before the match scene does, because the scene exit is itself
    a host-announced agreement. A peer whose plan is missing at stage load
    holds and waits for one (the safe default).
- **Who holds.** When `hold` is set, **every** peer holds, flat ones included.
  - The netplay exclusion becomes `pc_net_place_barrier()`.
  - Placers also run the placement UI.
  - Everyone else reports ready at once and shows the waiting card.
  - When `hold` is clear, nobody holds, and the match starts as any netplay
    match does.
- **Messages.**
  - `REL_XN_PLACE` (0x19), peer → host: `{match_seq, ready}`. It can go
    ready → not ready → ready again (S-4's "adjust again").
  - `REL_XN_PLACE_STATE` (0x1A), host → all, once a second and on change: who's
    ready, and the seconds left.
  - `REL_XN_RELEASE` (0x1B), host → all: `{match_seq}`. Sent once all are
    ready, or when the host's timer runs out (S-6). The host's clock is the
    only one that counts.
- **Release timing.** Ticks don't run during the hold, so there's no frame to
  agree on. Instead:
  - Each peer starts a 3 s countdown when `RELEASE` arrives.
  - The host delays its own countdown by half its worst RTT.
  - Frame 0 lands within a few ms on every peer. The time sync absorbs the rest,
    as it does at a match start today.
- **Keeping the links alive.** No input frames flow during the hold.
  - The tx timer keeps acking, so links stay warm.
  - The stall, no-progress and resume timers (`wait_remote`) are suspended
    while `pc_net_place_barrier()` holds. A silent peer is still caught by the
    link timeout.
  - A drop during the hold voids the game like any mid-game drop (§3.5).
- **Global placement.** The Place Stage flow (the Battlefield dummy) still
  auto-runs before the first XeleeNet entry of a launch, when no spot is set.
  Per-match holds then start each stage from that spot plus its own last
  grab, exactly as in 1P.

### 3.6 Stage pick and set score with more players

| Pick rule | 1v1 | Teams | Free-for-all |
|---|---|---|---|
| Loser picks | Loser picks; winner locks in with none (Direct today) | Losing team; either member's first pick counts | not offered |
| Everyone picks, then random among the picks | Today's LAN coin flip, generalised | yes | default |
| Random from the list | yes | yes | yes |
| Host picks | yes | yes | yes |

The first game of a set (no loser yet) uses "Everyone picks".

**Set score:** First to N, per player or per team. "Endless" keeps a win tally.
A void game (§3.5) doesn't count.

### 3.7 Identity and trust

- `POST /session` returns a nonce. The client signs `nonce || pubkey` and gets
  a 1-hour token.
- Peers check each other's key in the punched HELLO against the roster the
  service gave out.
- The host is authoritative over the settings and the roster. Peers are
  honest-but-checked, as on LAN.
- Link MACs stop off-path injection. They aren't encryption. An X25519 key
  exchange from the Ed25519 identities can come later if on-path tampering
  matters.

### 3.8 Client code layout

| Where | What |
|---|---|
| `src/pc/net.c`, `net_internal.h`, `net_wire.c`, `net_reliable.c`, `net_handshake.c`, `net_sync.c` | The N-peer engine (§3.2): `Peer[]`, per-port rings, per-origin `'M'`, host-led agreements, forwarding |
| `src/pc/xeleenet.c/.h` | Service client: token, create, list, join, admit, heartbeat thread. Uses `pc_slippi_http_*` / `pc_http_android`; rename them `pc_http_*` here. |
| `src/pc/net_stun.c` | STUN binding and a NAT type hint |
| `src/pc/net_punch.c` | Candidates → simultaneous open per link → route decision. Factor it out of the DHT path before M6 deletes that. |
| `src/pc/xeleenet_lobby.c/.h` | `XnLobbySettings`, presets, RULES extension, timed apply, roster reload, set score |
| `src/pc/xeleenet_menu.cpp`, `resources/xeleenet.rml/.rcss` | Host page, stage grid, browser, join steps, CSS overlay |
| `src/melee/mn/mnxeleenet.c` | Native host for the pages in the red frame |
| `src/melee/mn/mnonline.c` | A XeleeNet row above Slippi; the XeleeNet menu (Host, Browse, Join with Code, Name; no Place Stage) |
| `src/melee/gm/gmonlinemode.c/.h` | `ONLINE_KIND_XN_*`, N-player CSS entry, results and set score; the banner and toasts in `mnonlinecss.c` |
| `src/melee/mn/mncharsel.c` | Banner click, host-only Teams toggle, Open seat doors, the host's remove action |
| `src/melee/mn/mnstagesel.c` | `pick[4]` with sender port and the pick rules |
| `src/pc/net_chat.c`, `src/melee/if/ifnet.c` | Sender names; worst-ping HUD |
| `tools/xeleenet/`, `tools/xn_cli.py` | Worker, DOs, tests; a headless API driver |
| `src/pc/xr_place.c`, `src/melee/gm/gmscene.c` | `pc_net_place_barrier()`: match plan, the hold in netplay, ready reporting, waiting card with timer, countdown (§3.5a) |
| `tools/net_test.py` | Grows to N instances, with a per-link delay relay |

## 4. Milestones

Every milestone is checked on desktop first: N instances, `wrangler dev`, and
`tools/udp_delay_relay.py` per link. The headset comes at the end of each.

| M | Scope | Done when |
|---|---|---|
| **XN0** Service ✅ | Worker + DOs, auth, lobbies, directory, host heartbeat, admit, expiry, rate limits; `xn_cli.py`; CI test + deploy | `tools/xeleenet/test_service.py` hosts, lists, joins (with and without ask-to-join), removes and closes against `wrangler dev`: 42 checks pass (2026-10-27). Rate limits only enforced deployed |
| **XN1** N-peer engine | §3.2 over direct addresses (env-configured, like `MELEE_NET` today): `Peer[]`, rings, v12 wire, host-led agreements, worst-peer sync, forwarding, void-on-drop | `net_test.py` with 3 and 4 instances, 40-140 ms per link, loss and jitter: 20 FFA and 10 Teams games with equal checksums. One pair forced through the host. The 1v1 regression suite still passes. |
| **XN2** Connect (desktop ✅) | `net_stun.c`, `net_punch.c`, candidates through the service, per-link routes | 4 desktop instances join by code. Then a real test: home + phone hotspot + a second network |
| **XN3** Menus | XeleeNet rows, name, Host page, stage grid, Browse, Join with Code, join steps and errors, ask-to-join prompt | Host → browse → join → CSS on desktop with a gamepad; every J-5 failure seen on screen |
| **XN4** Lobby on the CSS | Banner, overlay, read-only view, timed apply, roster reload, game-in-progress wait, remove, leave and close, pick rules, set score, chat names; placement barrier | 4 instances: 20 settings changes and 10 joins/leaves under latency with equal checksums; the next game uses the new rules. Placement: one Monado XR instance + 3 flat. Hold, adjust again, release on all-ready and on timer; no hold with 4 flat or with the XR instance on "Start right away". Checksums equal from frame 0. |
| **XN5** Quest + hardening | Touch lasers everywhere, XR placement, logs, version gate, Windows build | A 4-player lobby with at least one Quest and two NATs |
| **XN6** Relay | TURN or our own relay for guest ↔ host failures, chosen from the XN2-XN5 logs | A guest on a carrier-NAT hotspot joins |

**XN1 steps** (each keeps the 1v1 suite green before the next starts; steps 1-4
built 2026-10-28, see the commits on `feat/xelee-net`):

1. **Per-port remote input.** One input ring and contiguous mark per remote
   port. `s_remote_have` becomes the lowest of them; `predict`, `write_head`
   and `head_check` loop over the ports. Still one remote.
2. **`Peer[]` transport.** The per-link state moves into a `Peer` struct:
   - address and pinning, MAC key and grace;
   - seq window, `rx_have`, `last_acked`, RTT ring;
   - the last packet, reliable lanes, resume, received checksums and the
     advantage ring.
   Still one peer.
3. **N direct peers.** `MELEE_NET_PEERS=0=host:port,1=host:port,…` with
   `MELEE_NET_PLAYER=0-3`; every link sends its own packets, and the header's
   player must match that link's peer. Nothing a 1v1 sends changes, so the
   wire stays v11 until step 6.
4. **Agreements across N:**
   - RULES/READY to every guest, with one start frame; READY back to the host
     only. With 3+ players the host then sends `REL_NONCES` (every nonce by
     port) and each guest pair keys its own link, lower port first;
   - the scene exit is all-to-all: every peer announces, everyone leaves on
     the latest frame + SCENE_HANDOFF (simpler than host-collected, and the
     same answer on a full mesh);
   - delay from the host's worst link; a delay message only counts from the
     host; time sync per link, acting on the peer we are least ahead of
     (Slippi's rule);
   - resume per link; a desync check against every peer.
   - Still open: READY_BARRIER (LAN's lobby, P-4), guest links' stats feeding
     the host's delay pick.
5. **The harness.** `net_test.py --players 3|4` boots N instances, one delay
   relay per link, and a 4-port `MELEE_DEBUG_VS`.
6. **Forwarding** (built). Wire v12 adds `'F'` (another player's input,
   passed on: origin port plus that player's packet body, from the passer's
   own ring of real input, with the origin's latest checksum report) and
   `'G'` (its ack). Routes:
   - A guest that hasn't heard another guest 8 s after the handshake, or has
     heard nothing on a working link for 3 s while its other links are
     fresh, marks the link dead and sends the host `REL_ROUTE` (the ports it
     can't hear).
   - The host forwards those players' input and relays their reliable
     messages (`REL_RELAY`).
   - Input still goes out on a dead route, so a link that comes back is heard
     and revives.
   - Test knob: `MELEE_NET_SIM_BLOCK=1:2`.
7. **Void on drop** (built). A guest that says goodbye, or is silent on the
   host's link for the stall timeout plus the resume window (6 s) while the
   other guests are heard, has left. The sequence:
   1. The host sends `REL_DROP`.
   2. Every remaining peer passes on what it holds of that player's input.
   3. Once the collected input has settled (500 ms) and every guest holds
      it, the host sends `REL_VOID {port, last frame, void frame}`.
   4. From the frame after the last, that port reads as a released pad.
      Anyone who predicted past it rolls back.
   5. The match ends as a no contest on the void frame (60 frames on).
   - The void tick runs lockstep, with the frame before it lockstep too,
     because ending the match isn't pure under a re-run.
   - The host leaving still ends the session.
   - In a 3+ player session, the session's silence clock is the host's link
     (on a guest) or the freshest guest (on the host).

   **Not done yet:**
   - The match freezes for the ~6.5 s it takes to declare a silent player
     gone. A goodbye is quicker.
   - Routes decided inside the engine wait 8 s at the start. XN2's punch
     phase decides them before the session, so this is the mid-session
     fallback.
   - A relayed message that finds the lane full is logged and lost.
   - Route detection assumes a link that fails, fails both ways.

**XN2 steps** (started 2026-10-28):

1. **STUN** (`net_stun.c`): a binding request and response codec (RFC 5389;
   tested against RFC 5769's sample) and our LAN address (the source address
   on the route out). Two servers by default (Google, Cloudflare;
   `MELEE_XN_STUN` overrides, `off` skips). If the two see different ports,
   the NAT maps per destination, and the log says direct guest links will
   likely fail.
2. **Service client** (`xeleenet.c`): sign in, host (and heartbeat every 2 s
   at the CSS, 10 s in a game, with the roster of guests the game reaches), join, leave and close, all on
   one thread of its own. It uses GET and POST only, so the service gained
   `POST /v1/lobbies/<id>/close` next to `DELETE`.
3. **The punch** (`net_punch.c`). One socket from STUN to the match.
   - Every punch datagram is `"XNP"`, a type, both ports and the lobby, and is
     signed with the sender's identity key. The service handed out every key.
   - **Probes** carry the sender's nonce and echo the receiver's. A link is up
     when a probe comes back echoing ours: both directions work, and we use
     the address it came from.
   - Only the host polls, so the host **introduces** each guest to the
     guests who joined after it. Each guest **reports** which links are up,
     or have given up after 8 s.
   - The host **starts** once the seats are full, every guest reaches it, and
     every guest pair has decided. The start message carries the seed, who
     plays, which pairs are direct (both ends punched), and every player's
     nonce.
   - Each link is keyed from the lobby, both nonces and both public keys. The
     handshake doesn't rekey it.
4. **Into the engine** (`pc_net_connect_links`). The engine gets the socket
   and each link's address, key and route. A pair that didn't punch starts
   out dead: its guests tell the host at once (REL_ROUTE), and the 8 s
   discovery inside the session doesn't happen.
5. **Harness:** `net_test_n.py --xn` starts `wrangler dev`, the host opens a
   lobby, and the others join with the code from its log
   (`MELEE_XN=host|join:CODE`, `MELEE_XN_URL`, `MELEE_XN_SEATS`,
   `MELEE_XN_IDENTITY`). `--block` blocks the punch too.

   **Desktop results** (one machine, real STUN, local `wrangler dev`):
   - 3 players: every link punched in under a second. Session up 1-3 s after
     the last join, and the match played clean.
   - 4 players, P2/P3 blocked, 30 ms and 1% loss: both guests gave up on
     each other at 8 s, and the host started them "through the host". The
     engine relayed from the first second, and the match passed with equal
     checksums.

   **Not done yet:**
   - The real test: home, a phone hotspot and a second network (the user's
     devices).
   - On Android, the HTTP path may not return a refusal's body (status only);
     Windows has no GET/bearer path yet (XN5).
   - A joiner arriving while a session runs (XN4's roster reload), and the
     host's own start button (XN3/XN4). For now the host starts when the
     seats are full.
   - A guest that can't reach the host waits 20 s and fails. XN6's relay is
     what fixes that case.
   - The mid-session route fallback (8 s) stays for links that die later.

**Testing four players for real** needs four devices on at least two networks.
Plan the XN5 session ahead: Quest, PC, the Android tablet and a fourth device.

## 5. Risks

- **The N-peer engine is the bulk of the work,** and it touches the rollback
  core that has taken months to make deterministic. Mitigation:
  - XN1 comes before any UI.
  - The 1v1 harnesses (`net_test.py`, `net_determinism.py`, fuzzers) must keep
    passing at every step.
  - 2 players stays a special case of N, not a separate path.
- **NAT failure rate.** Unmeasured. Mesh with the host as fallback limits a
  failure to guest ↔ host, but carrier NAT may still make XN6 necessary.
- **Roster reloads and timed applies** are new territory: sim state changing
  from a message. The scene hand-off is the precedent. `settings_rev` and the
  roster in the checksum make mistakes loud.
- **Host upload with 3 guests and forwarding.** It's small (Melee inputs), but
  measure it on a Quest on wifi.
- **Exact-build matching.** Every release splits the lobby list until people
  update. The browser says so.
- **Moderation of lobby names.** A charset, a length cap and a filter in v1.
- **Placement waits.** These are capped by the lobby's timer (15 s by
  default) and skipped entirely when nobody places.

## 6. Decisions log

All of the first round's questions are answered (2026-10-27). Nothing is open
right now. The next step is XN0 (service) and XN1 (N-peer engine).

1. **Lobby size:** four seats.
2. **CSS settings:** the overlay.
3. **Passwords:** none; the code does the job. Ask before joining and Hide
   code stay as optional host settings, off by default.
4. **Mid-game drop:** void the game.
5. **Default stage pick:** loser picks in 1v1, the losing team picks in
   Doubles, everyone picks in Free-for-all.
6. **Name:** XeleeNet's own, non-unique; the key is the identity.
7. **Where XeleeNet sits:** above Slippi.
8. **LAN in the browser:** no.
9. **Placement wait:** a host-set timer (default 15 s) that ends placement
   automatically; no hold when nobody places.
