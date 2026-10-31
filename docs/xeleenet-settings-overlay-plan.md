# XeleeNet: the Lobby Settings overlay

Plan, 2026-10-30. The design comes from two wireframes:
- the original lobby wireframe (`xeleenet-lobby-wireframe.html`, screens
  C1-C4);
- the fighter-list wireframe (`xeleenet-fighter-list-wireframe.html`, screens
  C2, C2b, F3 and F4).

It builds on:
- docs/xeleenet-plan.md §3.4 and §3.5;
- docs/xeleenet-ready-plan.md;
- docs/xeleenet-fighter-list-plan.md, assumed done: RULES `fighter_mask`,
  `pc_net_fighters()`, `Page::Fighters`, and the off-list filter in
  `xnPicksTake`.

Line numbers move with the fighter list. The function names are the anchors.

## Goal

In a XeleeNet or LAN lobby, the host opens Lobby Settings from the CSS. It's
the Host page's rows, in an RmlUi document over the dimmed CSS.
- START applies and B cancels.
- On Apply, every player's game takes the new rules on the same frame.
- Everyone's Ready clears, and a "Host changed: …" card shows.
- A player whose fighter came off the list gets the token back and picks
  again.
- Guests can open the same pages read-only.

## What the code does today

| Area | Where | Today |
|---|---|---|
| Rules into RULES | gmonlinemode.c `xnApplyRules` | Writes H1's rules straight into `GameRules`/`GamePrefs`, once, on `PC_XN_MENU_HOSTED`. |
| RULES capture/apply | net_handshake.c `rules_capture` / `rules_apply` | The host captures from `GameRules`/`GamePrefs`, plus the "wanted" teams, frozen and fighters. A guest applies on receipt, on whatever frame RULES lands. |
| New session = new RULES | gmonlinemode.c `xnAgreeFrame`; net_punch.c `start_build`, `at = frame + SWITCH_LEAD`, `begin_session` | Each roster change starts a new session. Its handshake agrees a frame, the CSS reloads on it and picks are kept (`cssXnKeepPicks` → `NET_MSG_XN_PICKS` → `xnPicksTake`). |
| Same-roster restart | net_punch.c `roster_changed` | None: a new session starts only when the ports or owners differ. |
| Seats on guests | `lobby_seats` → `pc_xn_css_seats` → mncharsel.c | A guest takes them from the service listing, a snapshot taken at join. They set `css_xn_dx`, which the hand's door hit tests use (`CSS_DOOR_X`), so they are sim state taken from outside the sim. |
| Teams on reload | gmonlinemode.c `onEnterCss` | `gm_InitVsMode` resets the players. Teams are dealt again only when the port set changes, so a same-roster reload would lose the teams players set. |
| Reads of GameRules during the CSS | mncharsel.c (the rules text, the handicap sliders) | Read every frame. Writing them mid-CSS on different frames on different peers desyncs. |
| Service settings | api.go `PUT /v1/lobbies/<id>/settings`; lobby.go refuses seats below `taken()` | Built, with no client caller. `pc_xn_punch_set_seats` is never called. LAN's `lan_host_options` only stores the settings. |
| Page engine | xeleenet_menu.cpp `Screen` (`choice`/`link`/`group`, `build_host`, `build_more`, `build_stages`, `build_fighters`, `legend`, `input`) | Every builder writes `s_host` directly. Pages are laid out inside the red Online frame. |
| CSS overlay | `Banner` (xeleenet-banner.rml): bar, notice slot `.nslot`, doors, `#clegend` | Fed by `pc_xn_css_frame` from `cssXnReadyFrame`. Takes no pointer events. |
| Input suppression | Launcher F1 `PADBlockInput`; the Slippi lock's `memset(HSD_PadCopyStatus…)`; the XR pointer is off in netplay | Netplay samples the local pads in `capture_local_sample`. A pad that is neutral there is neutral on every peer. |
| Rules-bar zones | mncharsel.c (rules menu, Teams toggle) | Guarded only by `cssOnline()`, which is false in XeleeNet. **Check:** whether any player's hand can still open Melee's rules menu or toggle Teams under the banner. |
| Keyboard over the CSS | mncharsel.c `mnNameNew_PcActive` | Freezes the CSS, so there's no Name row in a live lobby. |

## The key choice: how the change lands

**(a) A new message and an agreed frame inside the running session (§3.4).**
1. REL_XN_SETTINGS goes out.
2. The guests report their frames, and the host announces an apply frame.
3. Each field is applied inside the CSS tick.

**(b) A new session, as a join does (recommended).**
1. Apply stages the new rules and asks the punch for a restart with the same
   roster.
2. The usual two-step switch runs, then the RULES handshake.
3. On the agreed frame every player commits the rules and the CSS reloads,
   keeping picks except off-list ones.

| | (a) message + frame | (b) new session + reload |
|---|---|---|
| Desync risk | **High.** Each field needs new code inside the sim: live Teams, seats moving the doors, handicap sliders appearing, a token un-placed. A missed apply is silent, because menus have no checksum. | **Low.** Every field is read at CSS entry, as today. It's the path joins, leaves and multipad already exercise. One new hazard is fixed in O1: RULES landing mid-CSS. |
| Latency | ~0.3-1 s, no reload. | ~3.5-5 s: the ack round, SWITCH_LEAD (90 frames), `hs_lead` (120-200 frames), then the reload. |
| Code | New wire messages, an agreement, a journal and per-field CSS mutators: 500+ lines. | No new apply message: a restart flag, staged rules, seats in RULES, teams in the picks message. About 150 lines, plus the UI that both need. |
| What a guest sees | Values change in place. | "Applying the host's settings…", then the reload as a join looks, then the C4 card. |
| Joins mid-change | Need ordering against the roster reload. | Merge on their own: the next session carries both. |

**Recommendation: (b).** If the latency grates, a later step can re-run only
the RULES handshake inside the running session and skip the switch.

## Decisions (recommended, for review)

1. **Mechanism:** (b).
2. **Opening:**
   - The host's hand on the banner and A (C1; Melee's own rules-bar idiom). A
     mouse or laser click on the banner does the same.
   - Shortcut: **Z**, which the CSS doesn't use.
   - Legend: a not-Ready host sees `START Ready · Z Lobby settings · B Hold:
     leave lobby`. A Ready player's legend leaves Z out, since it can't be
     used.
3. **Only while not Ready.**
   - The overlay opens only when this player isn't Ready and no countdown
     runs. While it's open, the host's port sends neutral input, so it can't
     Ready and the countdown can't start.
   - Guests may still Ready; the reload clears it anyway.
   - If the sim shows the host Ready while the overlay is open (a START within
     the input delay), it closes as a cancel.
4. **Listing-only changes** (Visibility, Hide code) go to the service at once,
   with no reload and no Ready cleared. Any rule that affects play takes the
   reload.
5. **Name** is read-only in the overlay, because the keyboard freezes the CSS.
6. **Teams:**
   - Off → On deals teams again, as a new player set does.
   - Otherwise each player's team is kept: `NET_MSG_XN_PICKS` gains 4 team
     bytes.
7. **Guests** open with Z or the banner and get a read-only view. Their own
   port rests the same way while it's open.
8. **Seats:** the left arrow stops at the seats in use: occupied ports, plus
   pending claims, plus linked guests not yet seated. The value at the limit
   is dimmed in place, and the hint says "Remove someone first".

## The protocol

- **RULES** gains `uint8_t seats`. It rides on the same unreleased protocol
  bump as multipad and the fighter list. `rules_values_invalid` refuses seats
  outside 2-4.
- **Staged rules:**
  - RULES taken while an XeleeNet CSS runs is staged, not applied, on the host
    and on guests.
  - `pc_net_rules_commit()` applies the staged set. `xnAgreeFrame` calls it on
    the agreed frame, the same tick on every player, before the reload.
  - Direct and Slippi sessions still apply at once.
- **Wanted rules:**
  - `pc_net_set_rules_wanted(const GameRules*, item_freq, stage_mask, seats)`
    joins `pc_net_set_teams` / `_frozen` / `_fighters`.
  - `rules_capture` reads the wanted values when they're set, and
    `xnApplyRules` writes them there.
  - The commit writes `GameRules` and `GamePrefs`.
- **Restart:** `pc_xn_punch_restart(int reason)` sets `s_restart_req`. The
  PH_DONE condition becomes `roster_changed() || s_restart_req`, and
  `start_build` clears it. A byte of `XnStart.reserved` carries the reason (0
  roster, 1 settings), which `on_start` records for the guest's notice.
- **Editing status (C3, presentation only):** `REL_XN_EDITING` 0x16 `{on}`,
  host → guests. It is dispatched before the caller queue, as REL_CHAT is,
  and must never be queued (`xnPicksTake` drops other types). The sim never
  reads it.
- **Service:** `pc_xn_update_settings(name, open, json)`.
  - XeleeNet: `PUT /v1/lobbies/<id>/settings`.
  - LAN: `lan_host_options` plus a re-announce. Check that the announcer
    reads `s_opts` on every announcement.
  - On a 409 (seats), the overlay reverts the Seats row and shows the reason.

## The UI, screen by screen

- **C1, the banner:** the host's hand over it adds a glow (`hot`). The notice
  slot and the legend stay separate.
- **C2, the overlay (host):** `xeleenet-settings.rml`, a full-screen `#dim`
  over the CSS, plus `#screen`, `#board` and `#legend`, centred.
  - Left: RULES (Teams, Stocks/Win by, Time limit, Items, Friendly fire when
    Teams, More rules ›, Placement time SOON), then LINKS.
  - Right: FIGHTERS · STAGES · SET · LOBBY (Fighter list ›, Stage list ›,
    Stage pick SOON, Frozen Stadium, Format SOON, Seats, Visibility and Hide
    code, both not on LAN). Then the CHANGES box, APPLY and CANCEL.
  - No preset bar and no Name row.
  - A changed row shows `was <old>`.
  - The CHANGES box lists each change ("Stocks 4 → 3") and its effects:
    - "KOOPA's Fox is off the new list: KOOPA picks again";
    - "Teams on: teams are dealt again";
    - "Applies to the next game. Everyone's Ready is cleared."
  - Legend: `stick Change` on a value or `A Select` on a link or button, then
    `START Apply` and `B Cancel`.
  - LINKS reads "All direct", or "PIXEL ⇄ KOOPA via you", from
    `s_start.direct`.
- **C2b, the fighter page in the overlay:** `build_fighters`, plus a chip in
  the player's colour on each fighter someone holds. A held fighter that's off
  shows "Fox is off · KOOPA (P2) holds it and would pick again".
- **Read-only (guests):** the same pages built from the RULES in force, with
  nothing selectable. The legend shows `A View` on links and `B Close`.
- **C3 (guests while the host edits):** an info notice "Host is changing
  settings", and the host's door tag reads "editing…".
- **Between Apply and the reload:** everyone sees info "Applying the host's
  settings…".
- **C4 / F4 (after the reload):**
  - Every player diffs the previous Rules against the committed ones and shows
    info "Host changed: Stocks 4 → 3".
  - The banner's changed value is bright for 4 s.
  - The banner's rules line comes from the RULES in force, for guests too.
- **F3 (the player who lost their fighter):** `xnPicksTake` reports dropped
  ports. A local one gets a red notice "Fox is off the new list · pick again",
  and its door reads "choosing…".
- **Notice priority:** hold > countdown > own off-list > waiting > Host
  changed / Applying / editing > seat note > left.

## Steps

### O1 · Staged rules and seats in RULES (engine, small-medium)
1. In net_internal.h, net_wire.c and net_handshake.c: add `seats`, plus the
   staging and commit. Guests log `net: RULES staged`; the commit logs
   `net: RULES in force at frame N hash H`.
2. In gmonlinemode.c:
   - `xnApplyRules` writes the wanted values;
   - `xnAgreeFrame` commits;
   - the first host-alone open still writes `GameRules` at once.
3. `pc_xn_css_seats` uses `pc_net_rules_seats()` when a session runs, with
   the listing as a fallback.

Test: every existing `net_test_xn_menu.py` scenario stays green, and every
player logs the same commit frame and hash.

### O2 · Apply = a same-roster restart (engine, medium)
1. net_punch.c: `pc_xn_punch_restart`, the reason byte and
   `pc_xn_punch_switch_reason`. The Apply calls `pc_xn_punch_set_seats`.
2. gmonlinemode.c: `gmOnline_XnApplySettings()`.
   - In a session: the wanted values plus a restart.
   - Alone: `xnApplyRules`, then a local reload.
   - `NET_MSG_XN_PICKS` grows from 9 to 13 bytes (teams). Teams are dealt
     again when the Teams flag changed.
3. xeleenet.c / xeleenet_lan.c: `pc_xn_update_settings`.
4. A test knob, `MELEE_XN_TEST_APPLY=<frames>:<rules file>`, applies with no
   UI.

Test: scenario `settings-engine`.
- 3 players, with P2 holding Fox. The knob switches to Low tiers and 3 stocks.
- Every log shows the same commit, and `P2's pick … off the list`.
- Then a match with no DESYNC.

### O3 · The resting hand (engine, small)
1. net.c: `pc_net_hold_local(port, on)`.
   - `capture_local_sample` neutralizes that port's sample.
   - After release it stays neutral until the pad is at rest.
   - Only the machine's primary port is held; its other pads keep playing.
2. Alone, with no session, mncharsel.c zeroes `HSD_PadCopyStatus[0]` before
   `cssXnReadyFrame`.
3. The overlay reads the physical pad: `pc_net_phys_pad(pc_net_port_phys(me))`,
   or alone the copy from before zeroing. A small helper adds edge detection
   and repeat and calls `pc_xn_settings_input(PC_SCREEN_*)`.

Test: scenario `settings-hold`. With the overlay open, the host's stick, A
and START produce no `css: P1 A at` or `css: P1 ready` line on any player.

### O4 · The overlay document and pages (UI, medium-large)
1. xeleenet_menu.cpp:
   - a `HostSettings* s_edit` that the builders use instead of `s_host`; the
     overlay points it at a draft copy;
   - a second `Screen` with its own document and layout;
   - `build_host` gains an overlay variant (no preset or name, `was` values,
     the CHANGES box, APPLY/CANCEL);
   - the Seats lower limit;
   - the read-only builder from RULES.
2. `PcXnCssState` gains each door's ckind, for the CHANGES effects and the
   C2b chips.
3. `pc_xn_overlay_full_size` includes the overlay.
4. `resources/xeleenet-settings.rml/.rcss`.
5. The text: `xeleenet.settings.*`, the new notices and legend keys.

Test: xn_preview.py adds `css-settings`, `css-settings-fighters`,
`css-settings-guest` and `css-settings-seats`.

### O5 · Opening, closing and the CSS (integration, medium)
1. `cssXnReadyFrame` sees, on the shared pads, the host's port pressing A over
   the banner zone, or Z. On the local machine only, it then calls
   `pc_xn_settings_open()`. The sim doesn't change.
2. In XeleeNet the rules-menu and Teams zones are shut for every port.
3. The legend's entries per player.
4. The overlay closes when the sim shows this player Ready.
5. `REL_XN_EDITING` is sent on open and on close.

Test: scenario `settings-open`.

### O6 · After Apply: notices, banner, doors (UI, small-medium)
The diff at the commit, the dropped ports from `xnPicksTake`, the Banner's
notices and the bright value, and the guests' rules line from RULES. The door
tags read "editing…" and "choosing…".

Test: previews `css-changed`, `css-offlist` and `css-editing`, plus shots
from the live `settings-engine` run.

### O7 · The headset (small)
- One pointer feeder per frame for the RmlUi context, when any XeleeNet
  document is up.
- The Banner listens for a click on `#bar`, which opens the overlay. The
  overlay's rows take hover and click.
- Check what Touch maps to Z in menus. The banner click is the headset's main
  way in.

Test: on the Quest, with a desktop host and guest.

## Tests

`net_test_xn_menu.py` scenarios, each with 3 players and then `--net lan`:

| Scenario | What it checks |
|---|---|
| `settings-engine` | O2. |
| `settings-hold` | O3. |
| `settings-open` | O5. |
| `settings` | The full UI walk: open with Z, Stocks right, Fighter list → Low tiers, START. Every player commits on the same frame, P2's Fox comes back empty, Ready is 0/3, then a match with no DESYNC. |
| `settings-cancel` | B: no restart, no reload, Ready kept. |
| `settings-seats` | 3 of 4: lowering to 3 works, 2 can't be reached; raising to 4 lets a 4th player join. |
| `settings-teams` | Off → On deals teams again; a team set in the CSS survives an unrelated apply. |
| `settings-alone` | The host alone reloads locally. |
| `settings-race` | Apply while a join is in flight: the final RULES hash is equal everywhere. |
| `settings-list` | Visibility only: no reload, and the service row changes. |

## Order and size

| Step | Size | Depends on |
|---|---|---|
| O1 Staged rules, seats in RULES | small-medium | the fighter list |
| O2 Same-roster restart | medium | O1 |
| O3 Resting hand | small | — |
| O4 Overlay pages | medium-large | — (previewable) |
| O5 Opening and closing | medium | O3, O4 |
| O6 Notices | small-medium | O2, O4 |
| O7 Headset | small | O5 |

## Risks

- **RULES landing mid-CSS.** This is harmless today only because consecutive
  sessions carry the same rules. With (b), handicap and the fighter mask,
  both read every frame, would desync without O1's staging. O1 must land
  first.
- **Seats from the listing feed the hand's hit tests.** O1 moves them into
  RULES, which also fixes a latent join-time risk.
- **The 3.5-5 s latency.** The "Applying…" notice covers it. Re-running the
  handshake without switching is the follow-up.
- **There's no menu checksum.** The tests compare the commit hashes and the
  logged picks after the reload.
- **The hidden rules bar and Teams zones** may be live in XeleeNet today.
  Check before O5, and fix in O5 either way.
- **Seats racing a joiner.** The overlay's limit counts linked but unseated
  guests, and the service's 409 reverts the row.
- **Multipad.** Only the primary port rests. A second local pad still plays
  and can Ready. That's fine: the host itself isn't Ready, so nothing starts.
