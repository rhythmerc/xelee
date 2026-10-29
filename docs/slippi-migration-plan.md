# Slippi netplay migration plan

Replaces the custom transport described in `docs/netcode-plan.md` with a native
implementation of Slippi's wire protocol, so this port can play against Slippi
Dolphin and use Slippi Launcher, recon replays and the surrounding ecosystem.
Ranked is explicitly out of scope (see §7).

Status: **M1 + M2 + M3 done; M4 (identity + matchmaking) next.**
`docs/netcode-plan.md` remains the description of what exists today.

Branch: **`slippi-net`**, cut from `quest-xr` at `2a1ec6b` (decision 4). All
migration work happens here; `main` stays untouched until the branch is ready.

Decisions resolved 2026-10-12: kill the *internet* fallback (DHT/ranked) in M6
but keep the legacy engine for LAN (1, revised), account-gated Unranked with
account-free Direct (2), pin `SLIPPI_SEMVER` to a release and maintain it
ourselves (3), work on a branch off `quest-xr` (4). See §8.

## 0. The decision in one paragraph

"Our current netplay" and "Slippi" are two complete, incompatible stacks. This
is a re-implementation, not a dependency swap: Slippi's netcode lives inside a
Dolphin emulator as `CEXISlippi` and a set of PowerPC Gecko/ASM codes, so it
cannot be linked into a native decompilation. The good news is that a sibling
native decompilation — **Melee Unlocked** — has already done exactly this and
published the blueprint. It implements Slippi's wire formats from scratch,
speaks the same ENet P2P protocol as Slippi Dolphin, logs in with the Slippi
Launcher's `user.json`, queues on the real `mm.slippi.gg` servers, and has a
recorded port-vs-Dolphin online match (`PORT_COMPLETION.md:135-146`). We copy
its *protocol*, not its *engine*: our snapshot engine, `.slp` recorder and
determinism work are already stronger than what it uses and should be kept.

The cost is real: we trade the serverless DHT/rating/resume design for a
central dependency on `mm.slippi.gg`, an ENet dependency, and a Slippi account
(`user.json`). The payoff is cross-play with Dolphin and the entire Slippi
tooling ecosystem. Ranked is excluded, which is exactly the boundary Melee
Unlocked drew (`slippi_net.cpp:436-438`).

## 1. What "use Slippi" can mean

| Reading | What it is | Viable? |
|---|---|---|
| **(A) Wire interop** — implement Slippi's protocols natively so Dolphin players can connect to us with their normal codes | Port `SlippiNetplay`/`SlippiMatchmaking`/`CEXISlippi` wire formats; keep our own sim/snapshot/loop | **Yes.** This is the only path, and Melee Unlocked is the proof. |
| (B) Link Slippi's Dolphin code | Reuse `SlippiNetplay.cpp`, `EXI_DeviceSlippi.cpp` directly | No. They are Dolphin classes (ENet host, `NetPlayUI`, config system, PowerPC MMU, Dolphin savestates). Not separable, and an emulator's savestate is not a decomp's memory image. |
| (C) Use Slippi Launcher as the client | Launch through the Launcher and let Dolphin do the rest | No. That is Dolphin; our game is not Dolphin. The Launcher itself is Electron + TypeScript and only manages Dolphin installs, replays and login. |

This plan is **(A)**. Throughout, "Slippi" means "Slippi Dolphin / Ishiiruka's
netplay protocol", not the Launcher's UI.

### What the two references actually contribute

- **`project-slippi/slippi-launcher`** (GPL-3.0): the ecosystem contract we must
  satisfy, not code to reuse. It owns the `user.json` account file (uid,
  playKey, connectCode, displayName) that Slippi clients read for login, the
  replay folder layout, and the login/refresh REST endpoints. We read its
  identity files exactly as Melee Unlocked does (`slippi_net.cpp:220-249`) — as
  a desktop fallback; our own account store is primary.
- **`Hero88go/melee-unlocked`** (GPL-2.0-or-later for the ported Dolphin/Slippi
  files, per `NOTICE`): the reference implementation. A native decomp with:
  - `port/runtime/hle/slippi_net.{h,cpp}` — ~1.6k lines, a from-scratch,
    wire-compatible port of `SlippiNetplayClient` on vendored ENet
    (`slippi_net.h:1-2`).
  - `port/runtime/hle/exi_slippi.cpp` + `slippi_online.cpp` — the Slippi
    EXI command surface (matchmaking, inputs, savestates, chat, reports).
  - `port/runtime/hle/native_savestate.{h,cpp}` — its own rollback snapshot
    engine (page-chunked, incremental).
  - `docs/mu-net-protocol.md` — a **separate, non-Slippi** P2P protocol used by
    its own lobby. Not relevant to interop; ignore it for this migration.

**License note.** Melee Unlocked's own code is GPL-2.0-or-later/GPL-3.0; the
files we care about are explicitly GPL-2.0-or-later (Dolphin/Slippi lineage)
and are compatible with our GPL-3.0-or-later port code (`licenses/LICENSE.md` §2).
We may port them with attribution. See §9.

## 2. What Slippi netplay actually is

Three layers, all of which must match for a Dolphin player to connect.

### 2.1 Game side — the Slippi codes (PowerPC Gecko/ASM)

On Dolphin, a set of `slippi-ssbm-asm` codes run inside the game and talk to an
**EXI device in slot B** via DMA. The device implements a command byte space
(`exi_slippi.cpp:49-59`, sizes at `:65-73`):

- `CMD_ONLINE_INPUTS 0xB0`, `CMD_CAPTURE_SAVESTATE 0xB1`,
  `CMD_LOAD_SAVESTATE 0xB2`, `CMD_GET_MATCH_STATE 0xB3`,
  `CMD_FIND_OPPONENT 0xB4`, `CMD_SET_MATCH_SELECTIONS 0xB5`,
  `CMD_GET_NEW_SEED 0xBC`, `CMD_REPORT_GAME 0xBD`, plus the recording/replay
  commands (`0x35`–`0x41`), file commands (`0xD1`–`0xD4`), delay (`0xD5`),
  music (`0xD6`–`0xD8`), rank (`0xE3`–`0xE5`).

The ASM decides **when** to capture/load a savestate, when to stall for
inputs, and how to sync the clock. The EXI device does the I/O. In a native
decomp there is no EXI device and no Gecko interpreter, so both halves must be
re-implemented as native C: the device logic (in `src/pc/slippi/`) and the
game-side behaviour (native hooks, replacing the ASM). This is the single
largest and riskiest part of the migration, and it is *behavioural* parity, not
just wire parity.

### 2.2 Transport — ENet P2P

Slippi uses **ENet** (MIT; vendored by Melee Unlocked at
`port/third_party/enet/`). Client setup: `enet_host_create(local, 10, 3, 0, 0)`
— 3 channels (`slippi_net.cpp:314`). Channel selection on send
(`slippi_net.cpp:499-513`): today's Slippi forks send pad/ack packets
`UNSEQUENCED` on channel **1** (ack on **2** in Melee Unlocked's variant),
everything else `RELIABLE` on channel **0**.

Message IDs (`slippi_net.h:39-46`):

```
NP_MSG_SLIPPI_PAD               0x80
NP_MSG_SLIPPI_PAD_ACK           0x81
NP_MSG_SLIPPI_MATCH_SELECTIONS  0x82
NP_MSG_SLIPPI_CONN_SELECTED     0x83
NP_MSG_SLIPPI_CHAT_MESSAGE      0x84
NP_MSG_SLIPPI_COMPLETE_STEP     0x85   (Ranked set-flow; unused when Ranked is off)
NP_MSG_SLIPPI_SYNCED_STATE      0x86   (Ranked tiebreak; unused)
```

Payload encoding is SFML-style **big-endian** (`Packet` class,
`slippi_net.cpp:152-171`): integers big-endian, `bool` one byte, strings as
`u32 length + bytes`. The pad payload is `frame i32, player_port u8,
checksum_frame i32, checksum u32`, then `8 × N` pad bytes newest-first
(`slippi_net.cpp:335-378`); a pad is the first 8 bytes of the 12-byte
`GCPadStatus` (`PAD_FULL_SIZE 0xC`, `PAD_DATA_SIZE 0x8`). Acks carry
`frame i32, player_idx u8`. Note our `WirePad` is already exactly this 8-byte
shape (`src/pc/net_internal.h:130-135`) — the pad encoding is already Slippi's.

### 2.3 Matchmaking + identity

- Identity: the Slippi Launcher's `user.json` (uid, playKey, connectCode,
  displayName). Read for login; refreshed from Slippi's REST API on a background
  thread (`slippi_net.cpp:181-249`). This replaces our ed25519 identity,
  `NAME#XXXX` code derivation and DHT topics.
- Matchmaking: ENet **plus JSON** messages to `mm.slippi.gg:43113` (prod) /
  `mm2.slippi.gg` (dev), per Ishiiruka `SlippiMatchmaking.h`. Flow: login →
  `FindMatch(mode, connectCode)` → server returns match id + player list +
  stage list + peer IP/port → simultaneous connect (hole punch) → peer-to-peer
  ENet session. Ranked additionally uses `COMPLETE_STEP`/`SYNCED_STATE` and
  calls back into Slippi's GraphQL/report API — all out of scope here.
- Version gate: the client speaks one `SLIPPI_SEMVER` (Melee Unlocked: `3.6.4`,
  `slippi_net.cpp:32`) and refuses matchmaking when the servers advertise a
  newer one (`slippi_version.h`).

## 3. Gap analysis: our tree vs the target

### 3.1 Keep — already at or above the reference

| Asset | Where | Why keep |
|---|---|---|
| Region snapshot engine | `src/pc/net_snapshot.c`, `melee_state.ld`, per-platform range tables | Whole-region `memcpy`, ~5.6 MB / 0.6 ms (measured), all platforms post-link verified (`netcode-plan.md:133-175`). Melee Unlocked's incremental page engine is a memory optimisation we do not need at our measured cost. |
| `.slp` recorder | `src/pc/slp.c`, `slp_format.c` | Already Slippi-format 3.18.0, already hooks the same game functions as Slippi's recording ASM (`slp.c` header), replay-verified. Only the online metadata fields are missing (§3.3). |
| Determinism groundwork | `-ffp-contract=off`, vendored musl trig, forced seed, prewarm, record/replay, sync test | Required for *any* rollback; Slippi's own determinism is exactly what makes cross-play possible. Already stronger than the reference's. |
| Per-frame loop seam | `gmscene.c:445-461` (`pc_net_sync` / `gm_RunSimTick` / `pc_net_after_tick`) | The place where a new transport plugs in without touching the game loop. |
| Pad injection | `HSD_PadRenewMasterStatus`, `net.c write_head` | Slippi's `GCPadStatus` and ours already match byte-for-byte. |

### 3.2 Retire (become dead code under the Slippi route)

- **DHT / serverless discovery**: `net_dht.c`, `net_dht_item.c`, the DHT half of
  `net_match.c`. Slippi matchmaking is central. (Keep the files until the new
  path is proven; delete in the final phase.)
- **On-device rating / signed records**: `net_rank*.c` (Ranked-only).
- **Custom reliable channel & wire v6–v9**: `net_reliable.c`, most of
  `net_wire.c`, the custom RULES/READY handshake (`net_handshake.c`). ENet
  provides the reliable channel; Slippi's own selections/seed flow replaces the
  handshake.
- **Custom `.slp` timing hooks**? No — keep; only the network-driven parts
  (`pc_net_sim_frame`/`pc_net_confirmed_frame`) get rewired.
- **Menu modes**: `mnonline.c` `RANKED` entry and `ONLINE_KIND_RANKED`. LAN can
  survive either as Slippi Direct over LAN or as our existing engine; see §6.

### 3.3 Add

1. **ENet dependency** (MIT). Vendored, like Melee Unlocked. Our CMake has no
   ENet today (`CMakeLists.txt:103` lists only platform socket libs).
2. **Slippi protocol module** — `src/pc/slippi/`:
   - `slp_packet.c/.h` — SFML-style big-endian packet codec (mirrors
     `slippi_net.cpp:152-171`).
   - `slp_proto.h` — the `NP_MSG_*` ids and payload sizes; the single source of
     truth for protocol constants, including `SLIPPI_SEMVER`.
   - `slp_net.c` — ENet client: connect, channels, send/receive, ack timers,
     remote pad queues, checksum exchange, time-offset sampling (ports
     `slippi_net.cpp:281-840`).
   - `slp_match.c` — matchmaking client: ENet + JSON to `mm.slippi.gg:43113`,
     login, queue, hole-punch, result (ports `SlippiMatchmaking`).
   - `slp_user.c` — `user.json` reader + REST refresh (ports `User`,
     `slippi_net.cpp:181-249`).
3. **Game-side Slippi behaviour** — native replacements for the ASM, added to
   the existing hook points:
   - online input delay scheduling and stalling (maps onto `net.c`'s window,
     but must match Slippi's cadence: `SYNC_INTERVAL 30`, `WINDOW 7`,
     offset formula `slippi_net.cpp:349-357`);
   - CSS/SSS selection broadcast/merge (`PlayerSelections`,
     `slippi_net.cpp:479-497`) so both clients pick together;
   - initial RNG seed handshake (`CMD_GET_NEW_SEED` / game start);
   - per-frame finalized-state checksum (we have `frame_checksum`; must emit it
     in Slippi's field layout and compare as Slippi does);
   - savestate capture/load cadence driven by the game side, calling our
     snapshot engine instead of `CMD_CAPTURE_SAVESTATE` DMAs;
   - chat (`0x84`), match-state polling (`CMD_GET_MATCH_STATE`).
4. **Online replay metadata** — fill the currently-zero `0x1A5` display names,
   `0x221` connect codes, `0x249` Slippi UIDs, `0x2BE` session id, `0x2F1` game
   number, `0x2F5` tiebreaker (`slp_format.c:151-155`) from the session, so our
   `.slp` files are indistinguishable from Dolphin's online replays.
5. **Account identity (launcher) vs match initiation (Melee UI)** — the
   launcher's online tab owns *only* authentication: login/logout and showing
   your connect code. Everything that starts a match stays in the game's own
   Online menu (`mn/mnonline.c` → `gm/gmonlinemode.c`), as in real Slippi and
   Melee Unlocked. See §4 "UI split" for why.

## 4. Target architecture and the seam

Everything a new transport must satisfy is already named in `src/pc/net.h`.
Keep that contract and swap the implementation behind it. The game code does
not change.

```
gmscene.c  ──pc_net_sync()──▶  ┌─────────────────────────────┐
           ──pc_net_after_tick()─▶│  transport adapter          │
           ──pc_net_stats()──▶  │  (src/pc/slippi/adapter.c)  │
           ──pc_net_resim()──▶  └───────┬─────────────┬───────┘
                                        │             │
                          ┌─────────────▼──┐   ┌──────▼──────────────┐
                          │ slp_net.c      │   │ net_snapshot.c      │  keep
                          │ (ENet, Slippi  │   │ (capture/restore)   │
                          │  msg ids)      │   └─────────────────────┘
                          └───────┬────────┘
                                  │
                    ┌─────────────▼───┐   ┌──────────────────┐
                    │ slp_match.c     │   │ pc_slp.c         │  keep
                    │ mm.slippi.gg    │   │ (.slp recorder)  │
                    └─────────────────┘   └──────────────────┘
```

The adapter owns rollback *policy* (when to capture, when to stall, when to
load), because Slippi's policy is defined by its ASM and is what the wire
behaviour must match. `net_snapshot.c` stays a dumb, correct store:
`snapshot_take(Snapshot*, frame)` / `snapshot_restore()` are transport-agnostic
already. The input injection point stays `HSD_PadRenewMasterStatus` /
`write_head`, since our pad bytes already equal Slippi's.

Why the adapter rather than editing `gmscene.c`: the loop's contract is
`(sync → tick → maybe re-run)`, which is protocol-neutral. Swapping behind
`pc_net_*` keeps the risk surface in `src/pc/slippi/` and lets the old engine
and the new one coexist behind a build/runtime switch during the migration.

**A/B switch.** Add `MELEE_NET_STACK=slippi|legacy` (default `legacy` until
parity, then `slippi`), so regression tests and the current LAN/Direct flows
keep working throughout.

### UI split — auth in the launcher, matches in the Melee UI

Two surfaces, and each owns exactly one concern:

- **Authentication — the launcher overlay** (`resources/launcher.rml`,
  `port-menu.rml`; RmlUi, drawn before the game starts). Login, logout, and
  displaying the account's connect code. This is also where on-device
  credential entry lives (`slp_user`'s own account store). It writes our store
  and, on desktop, may *discover* an existing Slippi Launcher `user.json` as a
  convenience. It must not start, search for, or configure a match.
- **Match flow — the game's Online menu** (`src/melee/mn/mnonline.c` →
  `src/melee/gm/gmonlinemode.c`). LAN / Direct / Unranked / Ranked selection,
  the Direct hub and its code-entry keyboard, the search/browse screens, and
  the `pc_net_match_start(...)` / `slp_match` calls. This is where a match
  begins, and it is unchanged from the existing port except for the backend.

Why: (1) it matches the ecosystem — Slippi Dolphin has no external match UI, and
Melee Unlocked drives matchmaking from inside the game; (2) the game is the only
surface on **Quest**, where the desktop Slippi Launcher cannot run at all, so a
launcher-only match flow would break the primary target; (3) the launcher is not
running while the game plays, so it cannot host a search or lobby that must
outlive the boot.

This also disambiguates "launcher". The auth surface is **our** RmlUi overlay,
which runs on every target. The **Slippi Electron Launcher** is a separate,
desktop-only program we never invoke; at most we read a `user.json` it left
behind, and only as a fallback.

#### Login flow (launcher auth surface)

Slippi has no OAuth redirect we can use from the game; the account is a
Firebase email/password identity, and the playKey is a backend secret. So the
login is three plain HTTPS calls (no Firebase SDK needed — the same calls the
Slippi Launcher makes):

1. **Sign in** — `POST https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=<FIREBASE_API_KEY>`
   with `{email, password, returnSecureToken:true}` → an **ID token** and the
   Firebase **uid** (`localId`).
2. **Fetch the account** — `POST <SLIPPI_GRAPHQL_ENDPOINT>` with
   `Authorization: Bearer <idToken>` and `getUserKeyQuery($fbUid)` →
   `displayName`, `connectCode.code`, `private.playKey`, and
   `getLatestDolphin.version`.
3. **Store** — write `slp_user`'s own account file (uid, playKey, connectCode,
   displayName, latestVersion, chat). Best-effort `slp_user` REST refresh keeps
   the display name/version current afterwards.

Config (no secrets in the repo): `FIREBASE_API_KEY` and
`SLIPPI_GRAPHQL_ENDPOINT` are build/runtime configuration, exactly as the
Slippi Launcher takes them from its environment. When either is absent the auth
surface reports "online login unavailable" and the in-game menu still offers
Direct with a manual connect code (decision 2).

On-device UX: the launcher online tab becomes **email + password + a Sign in
button**, then shows the connect code and a Sign out button. No match fields.
On desktop, if no account is stored and a Slippi Launcher `user.json` is found,
offer "Use this computer's account" as the fallback path.

## 5. Phased plan

Each phase has a gate; do not start the next until the gate passes.

### M1 — ENet + protocol codec (no game integration) — **DONE** (`9459517`)
Vendored ENet v1.3.13 (MIT) at `extern/enet`, built as `melee_enet`
(`SEPARATE_FROM_ALL`; not linked until M2). Added `src/pc/slippi/slp_proto.h`
(wire constants — the single source of truth, pinned `PC_SLIPPI_SEMVER`) and
`slp_packet.{c,h}` (SFML-style big-endian codec incl. the `ok`-latch
semantics). `tools/test_slp_packet.c` pins byte order, widths, string
truncation and pad-payload arithmetic; registered as ctest `slp_packet`.
**Gate met:** the codec encodes/decodes the same bytes as Slippi's `sf::Packet`
and latches/rejects exactly as the reference does. Verified on this host with
`cc -std=gnu11` (no cmake in PATH here; CMake wiring is in place for CI).

### M2 — Slippi P2P client over ENet

**M2a done** (`c55d874`): `src/pc/slippi/slp_msg.{c,h}` — transport-free
encode/decode of the message bodies (selections, chat, pads, pad-ack), ported
from `SlippiNetplay.cpp` (`writeToPacket`/`readSelectionsFromPacket`, chat,
inline pad + pad-ack), including Merge semantics, the allowed chat-id set, the
port→remote-index mapping, and the receiver's pad-count arithmetic.
`tools/test_slp_msg.c` pins each message's bytes and the malformed-pad
rejections (ctest `slp_msg`). Codec symbols renamed `pc_slp_*` → `pc_slippi_*`
to avoid colliding with the `.slp` recorder API.

**M2b done** (`5e6335f`): `src/pc/slippi/slp_session.{c,h}` — the transport-free
session core (pad queues + ack-floor trim, remote window, drop-old, RTT/ack
sampling, trimmed-mean clock offset). `tools/test_slp_session.c` wires two
sessions together and pins the behaviour (ctest `slp_session`). Two parity
details confirmed against upstream: the remote window returns the **oldest**
selected frames, and the game-facing pad is **12** bytes (8 wire + 4 zero).

**M2c done** (`e11587b`): `src/pc/slippi/slp_net.{c,h}` — the ENet driver
(`enet_host_create` 10 peers / 3 channels, connect/disconnect, dispatch,
send-channel choice: pads+acks unsequenced, rest reliable).
`tools/test_slp_net.c` runs P1↔P2 over loopback: selections, a pad reaching P2,
and the ack returning to P1 (ctest `slp_net`, ~15 µs ping). `melee_enet` is now
linked into the `melee` target.
**M2 gate met.** Next: M3 (game-side wiring + rollback parity).

### M3 — Game-side wiring + rollback parity
**Approach: ring-level transport swap (A), chosen after verifying the engine
against Slippi's ASM (see §6).**

**Core wiring done** (`69999c1`, `20f27bb`, `2acd237`):
- `slp_session`: `pc_slippi_session_remote_latest` / `..._pad_for_frame`.
- `net_snapshot`: `pc_slippi_wire_checksum()` gathers Slippi's exact checkum
  fields from the live game (separate from `frame_checksum`).
- `slp_adapter.{c,h}`: the transport seam; delivers each newly-arrived
  contiguous remote frame via a callback (the adapter's `on_inputs`-shaped walk).
- `net.c`: `MELEE_NET_STACK=slippi` routes send/receive/wait through the
  Slippi driver at their single entry points; `slp_frame_cb` applies frames to
  `s_remote_ring`/`s_remote_have`/`s_rb_frame`; `pc_net_connect_slippi()` and a
  Slippi teardown in `pc_net_disconnect()`. Rollback loop, barrier, snapshot
  ring and pad handling are unchanged.

Verified: full `melee` target links; 6/6 `slp_*` ctests pass; the legacy
`net_resume`/`net_handshake`/`net_reliable`/`net_lan_txt` tests still pass.

**M3 GATE MET** (`7cb0dfb`): a local melee-pc instance vs melee-pc instance
match runs over the Slippi transport with rollback. `tools/net_test.py
--slippi` (direct, loopback) plays 10800 frames: both peers log `net: test
done`, 210-980 rollbacks per peer (max depth up to 7, lost 0), scenes hand off
in lockstep, and `net: DESYNC` never fires. The desync gate is genuine, not
vacuous: `net: first Slippi desync check at frame 534 (local 80030014 remote
80030014)` proves nonzero matching Slippi checksums are being compared (the
peer's wire checksum is fed into `s_rck`, our per-frame Slippi formula lives in
`s_wire_ck_ring`, compared with `pc_slippi_checksum_is_hard_desync`).

Problems found and fixed while closing the gate:
- `pc_net_connect_slippi` passed NULL callbacks to the driver, so setup and
  every engine reliable message were silently dropped (pads survived because
  the adapter polls the session directly). Now a callback struct forwards
  `on_setup` -> `slp_setup_cb` and `on_rel` -> `rel_dispatch`.
- The engine's lane-1 reliable lane (REL_SCENE scene hand-off, REL_DELAY,
  REL_CHAT) still went out the absent custom socket (`sendto error 9`). It is
  now tunnelled over ENet's reliable channel as `PC_SLIPPI_MSG_REL` (0xE2);
  `on_rel`'s dispatch was extracted into `rel_dispatch` so both transports
  share one consumer.
- `s_slippi_wait_remote` never resent unacked inputs, so a peer waiting on a
  frame the other had already sent-but-not-resent waited forever (froze near
  frame 1200). Added `pc_slippi_session_resend` / adapter `resend_local`
  (rebuild the whole unacked window, matching Slippi and the legacy wait) and
  call it each wait iteration alongside `send_inputs`.

**Deferred to M4:** the in-game seed/rules exchange and CSS selection sync now
ride an interim melee-pc-only `PC_SLIPPI_MSG_SETUP` (0xE1, seed + start frame)
instead of the custom RULES/READY. Slippi's own selections flow replaces it for
Dolphin interop.

**Gate:** MET. Dolphin parity is proven later (§M5).

### M4 — Identity + matchmaking
Two halves, split by surface (§4 "UI split"):

- **Auth (launcher overlay):** `slp_user.c` — our own account store is
  first-class (Quest has no Slippi Launcher), with a desktop `user.json`
  discovery fallback; REST refresh; on-device login UI. This is the only online
  concern the launcher owns.
- **Matchmaking (Melee UI):** `slp_match.c` — Unranked + Direct on
  `mm.slippi.gg:43113`, initiated from `gm/gmonlinemode.c` as it is today. The
  interim `PC_SLIPPI_MSG_SETUP` is replaced by Slippi's own selections flow;
  version gate against the server's advertised Slippi version.

**Gate:** a real Unranked ticket is accepted and a match id + peer endpoint are
returned; Direct connect succeeds between two of our instances — both started
from the in-game Online menu, not the launcher.

### M4 UI port — Slippi's netplay screens (in progress)
Slippi's screens are Gecko/ASM over the retail menus, so there is no source to
port; these match what Slippi Dolphin shows. Done on `slippi-net`, all gated on
the Slippi transport (`MELEE_NET_STACK=slippi` / a Slippi match) so LAN and the
legacy stack are unchanged:
- **Lobby/search status** (`gmonlinemode.c`): Slippi connect code as "YOU",
  "Connecting to Slippi...", "Looking for an opponent", "Connecting to CODE...",
  "Matched with CODE - starting...".
- **Online CSS** (`mncharsel.c`): only the local player's panel, hand and token
  are drawn; the opponent's door, hand, token and the unused ports are hidden
  (picks still sync through the pads). HMN/CPU door toggles are disabled, so a
  Slippi match stays 1v1 between humans. `MELEE_CSS_DUMP=1` logs the joint
  layout the door strides were measured from; `MELEE_CSS_SHOW_ALL=1` turns the
  hiding off.
- **Name tags** (`ifnametag.c`): fighters carry their connect codes.
- **Stage flow** (`mnstagesel.c`): Unranked picks from the ticket's stage list
  (external ids; same cell on both peers from the shared seed, no SSS picks);
  Direct keeps the pick exchange.

Wanted, not built: a **stage placement flow** (probably a new button on the
Online menu, with its own UI) to place the Slippi stage set ahead of time. A
live online match cannot be frozen, so the XR pre-fight hold is off in netplay
and stages would otherwise use their default placement; placements made in the
menu must be what the fight uses (presentation only, same invariant as the hold).

Parked (user's call, 2026-10-13): Slippi's **Teams** (2v2, mode 3) and **Party**
(mode 4; fixed rules like Unranked, 5-minute timer) modes. Everything built so
far is 1v1: the rollback engine, `net.local/remote`, the ticket parse (it
rejects anything but one remote), the CSS panels and the results flow. They
need multi-remote ENet sessions and the teams parts of the Neutral Spawn code
and CSS first. The Online menu offers Slippi > Unranked / Direct and LAN only.

Not done: the four dark name bars under the CSS doors are still drawn (a
structure outside the joints hidden so far); the Direct screen still takes the
DHT `#XXXXXXXX` code, not a Slippi connect code (`ABCD#123`); results-screen
names; Slippi's chat/quick-chat styling.

### Slippi's gameplay ruleset (from the Gecko codes)
the catalogue (`tools/gecko_catalog.py` writes it to `docs/slippi-gecko-catalog.md`, a local file) lists every patch in Slippi Dolphin's code set
(generated by `tools/gecko_catalog.py` from `GALE01r2.ini`), with what is done,
missing or unverified here. Findings that change behaviour:
- **Rules are pinned.** The General Codes rewrite the VS rules every frame:
  stock mode, 4 stocks, 8 minutes, damage ratio 1.0, no handicap, items off,
  singles stage mask `E70000B0`, everything unlocked. A Slippi session now
  applies the same constants on both peers (`slippi_rules_force()` in
  `net_handshake.c`) and restores the player's own afterwards; before this,
  each peer played its own saved rules.
- **Pokemon Stadium is not frozen online unless Direct players ask for it.**
  The match setup forces `alt_stage_mode` to 0 for every mode except Direct and
  Teams (Melee Unlocked `slippi_online.cpp`, matching Ishiiruka's selection
  packet), and the Stadium hacks only make the transformations deterministic
  (the four `GrPs*.dat` files are preloaded so a transformation cannot land
  mid-rollback). Our session therefore reports `frozen=0` through
  `pc_net_rules()`, whatever the local "Hazardless Stadium" preference says,
  and the transformation files are preloaded by `pc_net_pure_load()`. Frozen
  Stadium in Direct needs the selections flow.
- **Pause is allowed only in Direct** (the match flags set bit 3 otherwise).
- **Neutral Spawn is ported** (`gmvs.c neutralSpawn()`): retail puts player i
  on the stage's spawn point i (by port); Slippi numbers the open slots and uses
  fixed neutral spots on the six standard stages. Singles only.
- **Still missing before M5:** the UCF SDI, shield-SDI, tumble, pad-buffer and
  DBOOC fixes, which desync a match against Slippi Dolphin the first time one
  of those inputs is made.

### M5 — Cross-play verification (the whole point)
1. Our instance vs our instance over the real matchmaking servers, full game.
2. **Our instance vs Slippi Dolphin**, Unranked and Direct, full game — the
   acceptance test this entire migration exists for.
3. Both peers' `.slp` files for (2) compare equal via `tools/net_state_compare`
   plus a Slippi replay diff; online metadata fields populated.
**Gate:** a recorded port-vs-Dolphin match completes with agreeing checksums and
equal replays, the same standard Melee Unlocked met (`PORT_COMPLETION.md:135`).

### M6 — Retirement (internet fallback only; LAN kept)
Delete the *internet* fallback: `net_dht*.c`, `net_rank*.c`, the custom
internet handshake and the DHT/ranked parts of `net_match.c`; retire the Ranked
menu entry. **Keep the legacy LAN stack** (`net_lan.c` mDNS discovery + election
+ direct-IP, and the `net.c`/`net_wire.c`/`net_handshake.c`/`net_reliable.c`
engine it runs on) as the LAN transport (decision 1, revised 2026-10-12).
(2026-10-29: that LAN stack is gone; LAN is XeleeNet on the local network, on
the same engine. See docs/lan-lobbies-plan.md.)
Selection becomes mode-driven, not a build switch: LAN → legacy engine,
Unranked/Direct → Slippi. `MELEE_NET_STACK` is retained as the engine selector
rather than removed. Update `docs/netcode-plan.md` (mark the DHT/ranked sections
superseded) and `README.md`.

### M7 — LAN: kept on the legacy engine, not folded into Direct
**Decided (2026-10-12, revised):** LAN play stays on the proven legacy engine
(mDNS discovery + election + the custom transport), which needs no Slippi
account, no `mm.slippi.gg`, and runs at delay 0. It is **not** folded into
Slippi Direct. This is the one deliberate exception to "Slippi only": the mDNS
path is well-tested (`README.md` LAN rows, Android phone-to-PC matches) and its
independence from central infrastructure is a feature. Slippi Direct (endpoint)
remains available too, so LAN players can opt into Dolphin cross-play if they
want. Maintenance cost is accepted: two transports, selected by mode.

## 6. Rollback/snapshot reuse — verified against Slippi's ASM

In addition to the seam evidence below, the rollback **policy** was checked
field-for-field against Slippi's authoritative ASM (`project-slippi/slippi-ssbm-asm`
`Online/Core/{StartEngineLoop,LoopEngineForRollback,TriggerSendInput}.asm`) and
Ishiiruka `CEXISlippi.cpp`. Conclusion: our loop already *is* Slippi's cadence,
so M3 feeds the driver into it rather than duplicating it.

| Slippi behaviour | Slippi source | Ours | Match |
|---|---|---|---|
| Prediction = repeat last received input | `TriggerSendInput.asm` `LOAD_STALE_INPUTS: li r3,0` | `predict(): = s_remote_ring[s_remote_have]` | ✅ |
| Roll back to earliest mispredicted frame | `CHECK_WHETHER_TO_ROLL_BACK_LOOP` → min player savestate frame | `s_rb_frame` = first mismatch; `rollback_to(s_rb_frame)` | ✅ |
| Snapshot before each predicted frame, ~7 kept | `CAPTURE_CHECK`, `ROLLBACK_MAX_FRAME_COUNT=7` | `snap_predicted()`; `SNAPS=8 > WINDOW=7` | ✅ |
| Stall when remote > window behind | `shouldSkipOnlineFrame` | `need = frame − WINDOW`; `wait_remote` | ✅ |
| Delay frames carry neutral pad; sample f → frame f+delay | `handleSendInputs` | `write_head` same convention | ✅ |
| Checksum frame = newest finalized frame | `TXB_FINALIZED_FRAME` | `ck_frame = confirmed_frame()` | ✅ |

Two **timing-only** differences (neither changes simulation state):
- Time sync: Slippi uses clock-offset + frame skip; we use frame-advantage
difference (a deliberate improvement, `netcode-plan.md`).
- Rollback trigger: Slippi has a trigger deadzone; ours is exact `memcmp`
(→ at most extra rollbacks, never a desync).

**The one real gap — now closed** (`3e79973`): Slippi's *wire* checksum is a
specific formula (`FN_COMPUTE_CHECKSUM`: ActionStateID, raw pos/percent bits,
spawn #, stocks, plus a float sum), and Dolphin **hard-aborts** on mismatch.
Our `frame_checksum` is a stronger FNV hash that can never equal it. So the
Slippi transport carries `src/pc/slippi/slp_checksum.c` (Slippi's formula,
ported and tested) while our own checksum stays for LAN/desync detection.

Remaining caveat: policy parity does not prove *simulation* parity with
Dolphin. M5 is the gate for that.

### Seam evidence

- `snapshot_take(Snapshot*, int32_t frame)` and `snapshot_restore(const
  Snapshot*)` (`net_snapshot.c:602`, `:706`) take no transport types. The
  snapshot knows its frame and scene and validates both.
- Rollback *policy* today lives in `net.c` (`predict`, `snap_predicted`,
  `rollback_to`, the barrier, `:2469-2710`). That policy is what changes; the
  storage does not.
- The frame checksum (`frame_checksum`, `net_snapshot.c:266`) and state ring are
  transport-neutral; only the field layout we put on the wire changes.
- The pad queue contract (`write_head`, `unconsume`, re-applied from the live
  copy after restore, `net.c:1214-1301`) is exactly where Slippi's
  `CMD_ONLINE_INPUTS` result must land. Our existing code already survives the
  "raw samples queued since the snapshot" case rollback needs.

The one genuine risk is that Slippi's stall/capture/load **cadence** (defined by
its ASM) differs from ours in ways that affect the wire and thus interoperability
— that is behavioural work in `slp_net`/adapter, not snapshot work.

## 7. Ranked is out of scope

Agreed and baked in:

- No `Ranked` menu entry, no rating, no set flow (best-of-three, bans/picks).
- Skip `NP_MSG_SLIPPI_COMPLETE_STEP` / `NP_MSG_SLIPPI_SYNCED_STATE` (the code
  points above are literally commented "Ranked … no Ranked in Melee Unlocked",
  `slippi_net.cpp:436-438`).
- No Slippi GraphQL `reportOnlineGame` / rank fetch, no `net_rank*.c`.
- We still read `user.json`, so we have a connect code and can play Unranked and
  Direct; the server will simply never place us in a ranked set.
- Rationale confirmed: this port is a novel representation of Melee; until the
  community accepts it, it should not touch the ranked ladder — the same call
  Melee Unlocked made and shipped.

## 8. Risks and open questions

| # | Risk | Mitigation |
|---|---|---|
| 1 | **Behavioural parity with Slippi's ASM** (stall/capture/load timing, input delay semantics) is the hardest part and is not in the wire format. | Port Melee Unlocked's game-side logic verbatim first; verify against a real Dolphin in M5. |
| 2 | Central dependency: `mm.slippi.gg` can change, rate-limit or die; unknown-message and version-gate behaviour must degrade gracefully. | Follow `slippi_version.h` gating; keep Direct (endpoint) working without matchmaking. Keep the legacy stack on the branch until M6, then remove it (decision 1). |
| 3 | Slippi protocol version drift (we pin `SLIPPI_SEMVER`; Dolphin may move). | **Decided: pin to a specific Slippi release** (reference pins 3.6.4; pick the current stable release at M1). Single constant in `slp_proto.h`; refresh from the server, refuse on newer. Keeping it current for compatibility is explicitly our responsibility (decision 3). |
| 4 | ENet vendoring on all platforms (Windows/macOS/Android/iOS) — new C dependency in a tree that currently has none. | ENet is portable C with existing Android/macOS support; add to `CMakeLists.txt` and CI matrix. |
| 5 | Account requirement: Slippi matchmaking needs a Slippi account (uid/playKey/connectCode). | Own account store first-class (works on Quest, no Slippi Launcher needed); desktop `user.json` discovery as a fallback. Direct stays usable with an arbitrary code when no account exists. |
| 6 | Licensing/attribution for ported GPL-2.0-or-later code. | §9. |
| 7 | We give up distinct strengths (serverless DHT, signed rating) for internet interop, while keeping LAN. | Intentional per this decision. Legacy internet modules stay behind the mode/`MELEE_NET_STACK` selector until M6; LAN keeps using them permanently. |
| 8 | Determinism across our build and Dolphin's is not the same as determinism within our builds. | M5 is the only test that counts. Our record/replay proves our internal purity first. |

### Decisions (resolved by the user, 2026-10-12)

1. **M6 end state: kill the internet fallback, keep LAN.** Once Slippi parity
   is proven in M5, the serverless *internet* stack (DHT discovery, ranked,
   signed rating) is deleted. **LAN play is retained on the legacy engine**
   (decision revised 2026-10-12): mDNS discovery and the custom transport are
   proven, account-free and independent of Slippi's servers. So the end state is
   two transports — legacy for LAN, Slippi for Unranked/Direct — selected by
   mode. `MELEE_NET_STACK` stays as that selector; the DHT/ranked modules do not.
2. **Identity: account-gated Unranked is acceptable.** Unranked matchmaking
   requires a Slippi account (uid/playKey/connectCode). The account is stored in
   our own file, entered through the launcher's auth UI (Quest included), with
   desktop `user.json` discovery as a fallback. Direct must remain usable
   without one (manual endpoint / code).
3. **Version policy: pin to a release.** Pin `SLIPPI_SEMVER` to a specific
   current Slippi release and keep it updated ourselves — compatibility is our
   responsibility, not "track latest" at runtime.
4. **Sequencing: work on a branch.** All migration work lands on the
   `slippi-net` branch, which was branched off `quest-xr` (the Quest XR build,
   `2a1ec6b`). `main` is untouched during the migration.

## 9. Licensing and attribution

- Our port code is GPL-3.0-or-later (`licenses/LICENSE.md` §2). The reference's
  netplay files are GPL-2.0-or-later (Dolphin/Slippi lineage, per its `NOTICE`),
  compatible with GPL-3.
- Add to `licenses/`: ENet (MIT), and a `NOTICE`-style attribution for code
  ported from `Hero88go/melee-unlocked` (GPL-2.0-or-later) and, transitively,
  from `project-slippi/Ishiiruka` and Dolphin.
- Every ported file keeps its original SPDX header and gains a line naming the
  source path it was derived from, as the reference does.
- Do **not** copy Melee Unlocked's `docs/mu-net-protocol.md` P2P protocol — it is
  its own invention, not Slippi, and is irrelevant to interop.

## 9.5 Build recipe (XR branch)

The `quest-xr` branch needs OpenXR + the local Dawn fork. On this workstation
the prepared environment is the `melee-xr` distrobox:

```sh
distrobox enter melee-xr -- bash -lc 'cd /home/rmercado/projects/melee-pc && \
  cmake -B build-container-xr -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DAURORA_ENABLE_OPENXR=ON \
    -DAURORA_DAWN_PROVIDER=vendor \
    -DFETCHCONTENT_SOURCE_DIR_DAWN=/home/rmercado/projects/dawn \
    -DDAWN_SUPPORTS_CXX_MODULES=OFF && \
  ninja -C build-container-xr melee'
```

- `vendor` + `FETCHCONTENT_SOURCE_DIR_DAWN=../dawn` uses the local Dawn fork
  (experimental Vulkan-hook + multiview patches OpenXR needs). The prebuilt
  `package` Dawn lacks them.
- `DAWN_SUPPORTS_CXX_MODULES=OFF` is required with CMake 3.28.
- The plain (non-XR) config does not link on this branch: `xr_batch_pipeline`
  is defined inside `#if AURORA_ENABLE_OPENXR` but used unguarded at
  `extern/aurora/lib/gx/command_processor.cpp:1626`, so `AURORA_ENABLE_OPENXR=OFF`
  fails to compile. Pre-existing, unrelated to Slippi.

All five `slp_*` tests pass and the full `melee` target links in this config
(verified 2026-10-12).

## 10. Sources (external)

- `project-slippi/Ishiiruka` `Source/Core/Core/Slippi/{SlippiNetplay,SlippiMatchmaking,SlippiUser}.h`,
  `Source/Core/Core/HW/EXI_DeviceSlippi.*`; `project-slippi/slippi-ssbm-asm`.
- `project-slippi/slippi-launcher` — `user.json` account format, replay layout,
  REST login/refresh.
- `Hero88go/melee-unlocked` — `port/runtime/hle/{slippi_net,slippi_online,exi_slippi,native_savestate}.{h,cpp}`,
  `port/third_party/{enet,slippilib}`, `PORT_COMPLETION.md:128-146`,
  `docs/mu-net-protocol.md`, `NOTICE`.
- Local context: `docs/netcode-plan.md` (§3-§6 transport, §7 DHT, §9 ranked,
  §14 sources), `src/pc/net.h`, `src/pc/net_internal.h`, `src/pc/net_snapshot.c`,
  `src/pc/slp_format.c`, `licenses/LICENSE.md`.
