# XeleeNet: Ready, leaving and stage picks (implementation plan)

This plan builds the wireframe `xeleenet-ready-wireframe.html` (tailnet share,
melee-pc). Its generator is in session scratchpad `wf/wf_ready.py`. The user
approved every recommended answer on 2026-10-28.

The wireframe answers four problems from the first cross-network run
(2026-10-28):
- Any player's START sent everyone to the stage select.
- One player's B took everyone out of the lobby.
- The stage select ignored the lobby's stage list.
- The stage select showed NOW LOADING until the last pick.

It builds on `docs/xeleenet-plan.md` (§3.4-§3.6) and the state in
`docs/xeleenet-next.md`.

## Decisions (approved)

1. **Starting:** when every seated player is Ready, a 2 s countdown runs, then
   everyone goes to the stage select. Any player's B cancels the countdown,
   and their own Ready with it.
2. **What clears Ready:** a tap of B, changing character or costume, or a
   player joining or leaving (a roster reload clears everyone's). Changing
   team keeps it.
3. **A player who doesn't ready:** once everyone else has been Ready for 10 s,
   their door prompts *Waiting on you · START*, and the others see *Waiting on
   NAME*. Nothing starts on its own. The host can remove that player (hold A
   on their door; that's part of XN4, not this plan).
4. **Leaving:**
   - Holding B for about 1 s leaves the lobby for that player alone.
   - A tap of B only takes back Ready.
   - The host leaving closes the lobby for everyone, who land on the XeleeNet
     menu with a notice.
   - Host migration comes later.
5. **Stages outside the lobby's list:**
   - They're dimmed and struck through, and can't be picked.
   - Random draws only from the list.
   - The grid keeps Melee's layout.
6. **The pick timer:** 15 s after the first pick. Anyone still choosing then
   counts as Random from the list.
7. **The draw:**
   - One of the picks is chosen, each pick at equal odds; duplicates each get
     their own card.
   - When every pick is the same, there's no draw.
   - The highlight runs for about 2 s, then rests about 1 s on the result.
8. **Backing out of the stage select:** host only. B takes everyone back to the
   CSS and clears picks and Ready. Guests have no B there.
9. **Changing a pick:** until the last pick is in, A on another stage changes
   yours.
10. **Screen layout:**
    - The legend and notices never share a spot. On the CSS, notices get their
      own slot under the banner.
    - Melee's portrait grid shrinks by about an eighth (56×50 to 52×44 in
      wireframe units) and moves down to make room.
11. **In a headset:** the picks card and the draw float as a panel, as the
    placing cards do.
12. **Picks across a roster change** (added 2026-10-29, during P4): when
    someone joins or leaves, everyone still in the lobby keeps their
    character and costume. Ready is still cleared (decision 2), and teams are
    dealt again for the new player set. A newcomer starts with no pick.

## What the code does today

| Area | Where | Today |
|---|---|---|
| CSS START | `mncharsel.c` ~3936 | Melee ORs every human port's trigger. Any START with `mnCharSel_804D6CF7` (READY TO FIGHT) set leaves the CSS. |
| CSS B | `gmonlinemode.c` `onExitCss` | A B scene exit (`CSSPendingSceneChange_2`) disconnects and goes to `GM_MENU`. Every peer runs it, because the CSS runs on the shared inputs. |
| Determinism | net.c | Menus run in lockstep on shared pads. Anything derived from `HSD_PadCopyStatus` and frame counts in the CSS agrees on every peer with no extra messages. |
| SSS input | `mnstagesel.c` | Each player moves their own cursor (`mnStageSel_804D50A0` = local port). B in netplay reads ports 0 **and 1** (`b_pressed`), a 1v1 assumption. |
| SSS picks | `mnstagesel.c` `netStageSel_*` | A pick goes out as a reliable `NET_MSG_STAGE_PICK [pick, port]`. Once every port in `pc_net_port_mask()` has picked, `netStageSel_Resolve` hashes the seed with the picks in port order. Picks are final once sent. |
| Stage list | RULES `stages` mask (`xnApplyRules`) | It reaches every peer (logged `stages=071000f0`), but only Random reads it. |
| Roster change | `net_punch.c` `roster_grew` + the two-step switch | Joins only. A peer that goes silent is voided by the engine; nothing shrinks the roster. |
| Overlays | `xeleenet_menu.cpp` `Banner` (C1) | One RmlUi document over the CSS (`xeleenet-banner.rml`), scaled to the window. |

## Phases

Each phase ends with a test run that shows it working, following the house rule
of repro before fix.

### P1 · Stage select: the list and host-only B (small) — done 2026-10-28

Built as planned. Off-list icons are dimmed where Melee draws them: their
materials get a last TEV stage scaling the colour by 3/8 (`HSD_MObjPcDim`;
their own colour and alpha did nothing, the textures carry their own TEV).
Hovering one is as empty space: no highlight, name or preview, and A picks
nothing. An overlay of struck cards was tried first and dropped (it spilled
past the art and drew over the cursor). `xeleenet-sss.rml` keeps the list's
pill and the legend. `net_test_xn_menu.py --scenario sss-host-b` passes.

1. **Off-list stages:**
   - With `gmOnline_XnFlow()` and a RULES `stages` mask, dim each icon outside
     the mask. Darken its material colour and draw a strike: a quad joint or
     an RmlUi line per icon.
   - A on an off-list icon plays the error sound and doesn't pick.
   - `netStageSel_Random` and Random's resolution draw only from the mask
     (check today's path honours it for manual Random too).
2. **Host-only B:**
   - In XeleeNet, `b_pressed` reads `HSD_PadCopyStatus[0]` only (the host is
     always P1, and the pads are shared, so every peer agrees).
   - The exit goes to `state_css` with picks and Ready cleared.
   - Other online kinds keep today's rule.
3. **The list pill:** "Lobby list: Competitive (6)" at the top right. An RmlUi
   document over the stage select, sharing the banner's scaling code.

Test: in a 3-player `net_test_xn_menu` run, one guest presses A on an off-list
stage (no pick), and a guest's B does nothing. The host's B returns everyone to
the CSS, and the run goes on to a match.

### P2 · Ready on the CSS — done 2026-10-28

Built in mncharsel.c (`cssXnReadyFrame`, from the CSS frame hook before the
cursor procs) rather than gmonlinemode.c: the state sits beside the doors it
reads. `net_test_xn_menu.py --scenario ready` passes; the default 3-player run
readies everyone. Seen once, not reproduced: a 3-player run whose host hung
entering the stage select on "Can't load SFX file; bank(id=2) buffer
overflow" and an ARAM transfer stuck past 5 s (log kept as
build/xn3p-hang-a.log).

The plan as written:

1. **State:** `xn_ready` (a port mask), `xn_ready_since[4]`, `xn_all_ready_at`
   (frame) and `xn_hold_b[4]` (frames held), all in gmonlinemode.c.
   - Fed each CSS frame from `HSD_PadCopyStatus[port]` for every human port,
     with `gmOnline_XnCssFrame` as the hook.
   - Cleared on CSS entry: every entry follows a reload, a match or a back-out.
2. **START:**
   - In XeleeNet, mncharsel's START block stops OR-ing ports. A port's START
     with a valid pick on its door sets its Ready bit (plus a sound). A pick
     is its door's `sel_icon` < 0x19 and its token placed.
   - The vanilla leave stays shut until `xn_ready` covers every human port.
     The countdown then starts: `xn_all_ready_at = frame`.
   - 120 frames later, set `mnCharSel_804D6CF6`/`CF2` as START does today.
3. **B tap:** a port's B with its Ready bit set clears the bit and is swallowed,
   so Melee's B (un-pick) doesn't see it. B during the countdown also cancels
   the countdown.
4. **Clearing:** a door's `sel_icon` or `costume` changing from the value it
   had when Ready was set clears that port. A roster reload clears all
   (`xn_reload`).
5. **Waiting on:** if every other port has been Ready for 600 frames, the last
   one is "waiting".
6. **Hide READY TO FIGHT:** hide Melee's banner joint in XeleeNet, as Slippi's
   flow does at ~3916.

Test: three instances, P1 and P2 ready while P3 holds back. START from P1 or P2
must not leave the CSS. P3 readies, the countdown runs, and everyone reaches
the stage select on the same frame (scene hand-off logs). A cancel by B
mid-countdown is tested too.

### P3 · CSS overlay: legend, notice slot, door tags, portraits — done 2026-10-28

Built with two changes: the doors' rectangles are measured fractions of the
screen (as the banner's place is), not projected joints; and the portraits
shrink to 0.76 (not 0.88), 3.3 units lower, because the real CSS has far less
room between the grid and the doors than the wireframe drew. The icons' bounds
table is rewritten while XeleeNet's CSS is up and restored on exit, so the
hand's hit test, the tokens' spots and their drift follow without touching
each use. Previews: `tools/xn_preview.py css-ready css-countdown css-waiting`.
The legend lacks "Hold B: leave lobby" until P4.

The plan as written:

1. **One RmlUi document for the CSS:** extend `xeleenet-banner.rml`, or add
   `xeleenet-css.rml` with the same scaling. It holds:
   - the banner (as today, plus the READY n/m pill, gold when all are in);
   - `#notice`, the slot under the banner (34 px at banner scale), in four
     kinds: info, warn, gold countdown with a draining bar, and red;
   - `#legend` above the doors, with the menus' GC glyphs (reuse the `.lg`,
     `.gl` rules from xeleenet.rcss);
   - one tag per door: the READY ribbon, *START when ready*,
     *Waiting on you · START*, and the dashed outline.
2. **Positions:** door rectangles come from the CSS model's door joints
   (`mnCharSel_803F0DFC.doors[i].door_joint` world position, projected the way
   the banner is placed), not from constants.
3. **Text:** all of it comes from `resources/lang/en.yaml`
   (`xeleenet.css.*`, `xeleenet.legend.*`).
4. **The legend per local player:**
   - Not ready: START Ready · B Hold: leave lobby.
   - Ready: B Not ready · B Hold: leave lobby.
   - During the countdown: B Cancel.
5. **The portraits:**
   - Find the joint holding the icon grid (`MELEE_CSS_PROBE`-style dump of
     the children of `mnCharSel_804D6CC0`).
   - Scale it by about 0.88 and move it down so the grid's top clears the
     notice slot.
   - Apply the same transform to the icon hit bounds (`icons[].bound_l/r/u/d`,
     used at ~2814) and to the token snap positions (~2228/2295). The hand
     and tokens must still land on the shrunken icons.

Test: preview shots of each state (`MELEE_XN_PREVIEW=css-ready|css-countdown|
css-waiting|css-leaving`, rendered with `tools/xn_preview.py`), then a live
3-instance run. The hand must pick every icon at its edges.

### P4 · Leaving alone, and the host closing — done 2026-10-29

Built as planned, with these differences:

- **No goodbye, no collection.** Every player sees the hold end on the same
  lockstep frame F, so each one releases the leaver's port from F + 1 on
  its own (`pc_net_port_leave`). The leaver waits until every peer has
  acknowledged its input up to F (`pc_net_leave_quietly`, at most 1.5 s;
  0 ms on loopback), then ends its session without a goodbye. A goodbye
  would read as a drop to a peer that hasn't reached F yet.
- **The hold counts only once B has been up** since the CSS came, so a B
  held over from the stage select's host B doesn't count. Held B is taken out of
  the pads, and Melee's back button is off in the lobby.
- **A 1v1 lobby:** when the guest leaves, the host's session ends, the punch
  gets its socket back, and the CSS reloads at once with one door. The next
  joiner starts a new session, as in a fresh lobby.
- **A departed player isn't re-learned:** guests' member lists never change,
  and the host's lags. The punch remembers each leaver's key and the
  service's `joined` stamp (now in the member list), so the same player
  rejoining is a new stay.
- **Landing:** the leaver goes to the XeleeNet pages (the Lobbies list for a
  guest, H1 for a host). After the host closes, guests land on the Lobbies
  list with "The host closed the lobby." on the hint line, and that lobby
  is hidden from the list for 30 s.
- **Picks (decision 12):** at a roster reload the host sends every port's
  character and costume (`NET_MSG_XN_PICKS`). Everyone else waits for that
  message on entering the CSS and starts from it, the newcomer included.
- **The seed:** after the agreed frame, players run different scenes, and
  those in the CSS draw random numbers (a pick on Random does). The engine
  puts back any draw made between ticks, so the re-seed happens on the
  reloaded CSS's first frame. The desync check pauses from the agreed frame
  until then.
- **Fixed on the way:** a second Join in the same run waited on the Joining
  page forever, because the last lobby's session agreement still counted.

Tests: `tools/net_test_xn_menu.py --scenario leave-guest` (3 players, and
`--players 2`). The last guest leaves, the others reload with their picks,
the leaver rejoins from the list, and everyone plays a match with no DESYNC.
`--scenario leave-host` checks the host closing the lobby. Previews:
`tools/xn_preview.py css-leaving css-left`.

Not covered:
- A leave while a join's start is in flight: the start in flight still
  names the leaver.
- A player who drops without leaving (a crash, a lost network) goes through
  the engine's drop and void, as before.
- B held anywhere but the CSS.

The plan as written:

1. **Hold B:** 60 frames of B held on a port in the CSS, counted from the
   shared pads, so every peer sees it on the same frame.
   - The ring around that player's hand is drawn by the CSS overlay on the
     leaver's own screen only. The others see nothing until the leave.
   - The red notice "Keep holding B to leave the lobby" shows from frame 10 of
     the hold.
2. **A guest leaving:**
   - The leaver: `pc_net_release_socket()` or a disconnect,
     `pc_xn_punch_end()` (which leaves at the service), then the XeleeNet menu.
     The CSS's scene exit is local and doesn't go through the hand-off.
   - Everyone else: the host's punch learns the departure. It's on the same
     frame from the pads, and also from the service's member list. The host
     then runs a **roster shrink**: `start_build()` for the remaining ports,
     the same two-step switch a join uses.
   - The CSS reloads with that seat open, and the notice reads "NAME left the
     lobby".
   - net_punch.c needs `roster_shrank()` beside `roster_grew()`, and the start
     message must allow ports to go away.
   - The engine's void-on-drop must not fire for a planned leave: the session
     ends by switch, not by silence.
3. **The host leaving:** P1 holding B closes the lobby
   (`pc_xn_punch_end` closes it at the service). Every peer sees the hold on
   the same frame and goes to the XeleeNet menu. The menu shows a notice there
   (host page `hostNote` or the Lobbies page): "The host closed the lobby".
4. **Remove:** today's B scene exit (`CSSPendingSceneChange_2` in
   `onExitCss`) for XeleeNet. Melee's own B-hold back-out on the CSS must not
   fire; swallow B in XeleeNet after the Ready logic has read it.
5. **Peer status:** a planned leave clears the peer status on every side.
   ddf8fe4f covers the leaver; the stayers need the same.

Test: three instances, P3 holds B. P1 and P2 stay in a CSS with two doors and
play a match with no DESYNC. P3 can host or join again right away. Separately,
P1 holds B: P2 and P3 land on the XeleeNet menu with the notice.

### P5 · The picks card, the draw and the timer

1. **Host-authoritative final picks:**
   - Guests may change their pick while others choose (A on another stage, a
     new `NET_MSG_STAGE_PICK`). That breaks today's assumption that a pick is
     final, so the host decides the end.
   - The host holds the newest pick per port. It sends `NET_MSG_STAGE_FINAL
     [picks[4], timed_out_mask]` once every port has picked, or 15 s
     (900 frames) after the first pick.
   - Every peer, the host included, resolves only from FINAL with today's
     seeded hash. A guest's late change that missed FINAL just loses, and
     their card shows the final pick.
   - Ports that timed out get Random from the list, seeded the same way on
     every peer.
2. **The draw:**
   - When FINAL arrives, a 120-frame highlight runs across the pick cards in
     port order, easing out to the chosen index. Then 60 frames rest on it,
     and the scene ends through the usual hand-off.
   - Each peer starts the draw when its own FINAL arrives. Screens differ by
     about a ping, and the hand-off evens out the exit.
   - With every pick the same, skip straight to the result.
3. **The card:** an RmlUi document over the stage select (with P1's pill). It
   shows a row per seated port, from `net_picks` plus the local pick: colour,
   name, thumbnail, and ✓ or *choosing…*. It replaces NOW LOADING.
   - Thumbnails: crops of the stage select's own icon textures if RmlUi can
     sample them, or else a per-stage colour plus the name. A later pass can
     add images.
   - Each pick is also outlined on the grid in that player's colour.
4. **The legend:**
   - Picking: stick Move · A Pick.
   - Picked: A Change pick.
   - The host's legend adds B Everyone back to CSS.
5. **In a headset:** check that the card and the draw read well on the menu
   quad. The wireframe asks for a panel like the placing cards; match that if
   the quad doesn't suit.

Test: three instances. A guest changes their pick twice, and one changes with
added delay (`MELEE_NET_SIM_DELAY`) racing the last pick. Every peer must log
the same resolved cell. A run where P3 never picks must resolve at 15 s with
Random from the list. A run where everyone picks Battlefield must show no draw.

## Order and size

| Phase | Size | Depends on |
|---|---|---|
| P1 Stage list, host-only B | small | — |
| P2 Ready | medium | — |
| P3 CSS overlay and portraits | medium-large (the portrait transform is the risk) | P2 for the states |
| P4 Leaving and closing | large (a roster shrink in the punch) | P2 |
| P5 Picks card, draw, timer | medium-large | P1 |

Suggested order: P1, P2, P3, P4, then P5. P1 and P2 fix what hurt the
friends' run. P4 is the deepest change; it touches net_punch.c's switch.

## Text and tests

- Every new string goes in `resources/lang/en.yaml`, under `xeleenet.css.*`,
  `xeleenet.sss.*` and `xeleenet.legend.*`.
- `tools/net_test_xn_menu.py` grows a `--scenario` per phase: `ready`,
  `leave-guest`, `leave-host`, `sss-host-b`, `sss-repick`, `sss-timeout` and
  `sss-same`. The default run keeps today's flow, now pressing START on every
  instance.
- `tools/xn_preview.py` grows the CSS and stage-select overlay states for the
  screenshot reviews.
