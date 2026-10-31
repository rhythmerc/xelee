# XeleeNet: the fighter list

Plan, 2026-10-30. The design is the approved wireframe
`xeleenet-fighter-list-wireframe.html` (tailnet share, melee-pc/), sections 1,
2 and 5.

The host picks which fighters a lobby allows, the same way it picks the stage
list. It's the natural mirror of the stage list:
- a Host page row, which opens a picker page;
- RULES carries a mask to every player;
- on the CSS, off-list portraits are dimmed in place and can't be picked;
- Random draws from the list.

Two follow-up plans assume this one is done:
- docs/xeleenet-lobby-details-plan.md: Details before joining, with the fighter
  grid.
- docs/xeleenet-settings-overlay-plan.md: changing the list (and the other
  rules) from inside the lobby.

## Decisions (2026-10-30)

1. **Presets:** All (25), High tiers, Low tiers, and Custom (any toggle). The
   tiers follow the Back Rooms 2021 NTSC tier list (ssbwiki, "List of SSBM tier
   lists (NTSC)", 13th list):
   - **High tiers (S to B-), 13:** Fox, Marth, Jigglypuff, Falco, Zelda /
     Sheik, Captain Falcon, Peach, Ice Climbers, Pikachu, Yoshi, Samus, Luigi,
     Dr. Mario.
   - **Low tiers (C+ to F), 12:** Ganondorf, Mario, Donkey Kong, Young Link,
     Link, Mr. Game & Watch, Mewtwo, Roy, Pichu, Ness, Kirby, Bowser.
   - **Zelda / Sheik** is one entry, and it goes with **High**. The list ranks
     Sheik in B and Zelda in D, but the two can't be allowed apart, and lobbies
     that allow the entry are allowing Sheik.
2. **Zelda / Sheik is one entry** (25 entries, one per portrait). Both bits are
   on or both are off.
3. **A on an off-list portrait:**
   - the error sound plays;
   - the token stays in the hand;
   - a 2 s notice ("Fox isn't on this lobby's list") shows on that player's own
     screen only.
4. **A pick that's off the list** comes back empty: on a CSS reload, and after
   a mid-lobby change (the overlay plan). It never becomes a random fighter.
5. **Off-list portraits are dimmed only**, with no strike, as the stage select
   does.
6. **The name** is "Fighter list", next to "Stage list".
7. **The mode presets** (1v1, Doubles, Free-for-all) set it to All. Changing it
   turns the mode preset to Custom, as the stage list does.
8. **Costumes, Teams and the same-costume rule are untouched.** VS Mode, 1P and
   Slippi are unchanged.

## The mask

`fighter_mask`, a `uint32_t` with bit = CharacterKind (src/melee/ft/forward.h:129):
- bits 0-25 are the playable fighters;
- bit 0x13 (Sheik) always equals bit 0x12 (Zelda);
- All is `(1u << 26) - 1`.

The presets are constants in one place, `src/pc/xeleenet_fighters.h`, which
both C and C++ include:
- `XN_FIGHTERS_ALL`, `XN_FIGHTERS_HIGH` and `XN_FIGHTERS_LOW`;
- the 25 portrait entries in the CSS's own order, with their names: the
  icons[] table, mncharsel.c:224;
- `xn_fighters_normal(mask)`, which makes the Sheik bit follow Zelda and
  clears bits past 25.

## Steps

### F1. RULES carries the mask (engine)
- **The struct:** `Rules.fighter_mask` (src/pc/net_internal.h:273), placed
  after `teams`.
  - The sizeof check, the wire byte order (net_wire.c `wire_rules`) and
    `rules_hash` follow from it.
  - The payload grows by 4 bytes, well under REL_MAX.
- **Capture and apply** (net_handshake.c:255-284), like `teams`:
  - `pc_net_set_fighters(mask)` sets what the host sends (s_fighters_wanted),
    and `rules_apply` keeps what's in force.
  - `pc_net_fighters()` returns that mask in a XeleeNet session, and All
    anywhere else, so VS Mode, 1P, Slippi and the old direct sessions never
    see a list.
- **Validation:** `rules_values_invalid` (net_handshake.c:400) refuses a mask
  with no playable bit, or one whose Sheik bit differs from Zelda's. That's as
  stage_mask 0 is refused.
- **Protocol:**
  - The bump to 13 is already on feat/lan-multipad and unreleased, so this
    rides on 13. This branch builds on that one.
  - If multipad were ever to ship first, this step bumps to 14.

Test: the unit test for the Rules wire layout (tools/test_net_resume.c checks
sizes), and a 2P lobby run in which every log line "net: RULES" names the same
mask.

### F2. The host's settings, and the picker page
- **HostSettings** (src/pc/xeleenet_menu.cpp:130):
  - gains `unsigned fighters = XN_FIGHTERS_ALL`;
  - is saved as `fighters=` (host_save and host_load at :151-215; 0 or junk
    loads as All);
  - `preset()` (:217) sets it to All.
- **PcXnRules** (xeleenet_menu.h:57) gains `fighter_mask`.
  `pc_xn_menu_rules` fills it, and `xnApplyRules` (gmonlinemode.c:1606) calls
  `pc_net_set_fighters`. That's the same path the stage mask takes.
- **The Host page** (build_host, :575-590):
  - The right column's heading becomes "FIGHTERS · STAGES".
  - A "Fighter list" row comes first, next to Stage list. Its value is
    "All (25)", "High tiers (13)", "Low tiers (12)" or "Custom (n)", and A
    opens `Page::Fighters`.
  - The column is one row taller, so if the legend crowds it, the SOON rows
    move to More rules (check this against the Host page shot).
- **`Page::Fighters`** is a copy of build_stages (:635), laid out as the CSS
  is: rows of 9, 9 and 7, in icons[] order.
  - It has chips for All, High tiers and Low tiers, plus a Custom chip that
    can't be selected.
  - The tiles show portrait names. Thumbnails are the details plan's problem;
    names are enough here.
  - Controls:
    - A toggles an entry. At least one always stays on.
    - Y keeps only the highlighted fighter.
    - X steps through the presets.
    - B goes back.
  - The hint box names the highlighted fighter. On Zelda it says Zelda and
    Sheik come together; on Ice Climbers it says Popo and Nana.
  - Wiring: the page enum (:349), title (:1433), legend (:1319) and back
    handling (:1364), the same places Stages has.
- **Lang:** `xeleenet.fighters.*` (the title, chip names, hint lines and the
  25 names) and `xeleenet.host.rows.fighter_list`, in resources/lang/en.yaml.
- **settings_json** (:283) gains `"fighter_list":"all|high|low|custom"`. The
  masks themselves are the details plan's.

Test: `tools/xn_preview.py` renders the Host page and the new page, then
`net_test_xn_menu.py`'s Host walk.

### F3. The CSS enforces the list
- `cssXnOffList(icon)` in mncharsel.c: a XeleeNet lobby, and that icon's
  `char_kind` bit isn't in `pc_net_fighters()`.
  - Every player computes it from RULES and the shared pads, so it can't
    desync.
- **Picking** (the hover loop, mncharsel.c:2864-2870):
  - An off-list portrait doesn't take the hand: the `state >= 1` test gains
    `&& !cssXnOffList(i)`.
  - A over it plays the error sound (`lbAudioAx_80024030(3)`), and logs
    `css: P%d A on off-list %d`.
  - The notice is for the overlay: `pc_xn_css_off_list(port, ckind)` shows the
    notice only when that port is one of ours (`pc_net_local_mask`).
- **Random** (mnCharSel_8025FB50, :2247-2250): the redraw loop also skips
  `cssXnOffList`. The draw uses the shared seed, so every player lands on the
  same fighter.
- **Kept picks** (cssXnKeepPicks, :5751) leave out off-list ckinds. The
  restore in gmonlinemode (`gmOnline_XnKeepPicks`'s consumer) drops them too,
  so a reload with a new list gives an empty door, never a random one.
- **The CSS entry's locked-fighter check** (:4761) turns a locked ckind into a
  random one. In a XeleeNet lobby an off-list ckind becomes ChKind_None instead.
- **Dimming:**
  - Each CSS frame, the off-list portraits' joints (`lb_80011E24` with
    `joint_id_vs`) get `HSD_MObjPcDim`. That's the stage select's
    `sss_dim_icon` (mnstagesel.c:225), moved to a shared helper.
  - `HSD_MObjPcDimClear` runs on CSS exit, as at mnstagesel.c:2204.

Test: the new scenario below. It must show no DESYNC, and every player's door
must show the same fighter after Random.

### F4. Text: banner, notice, service, LAN
- **The CSS banner's rules line** (Banner::update, xeleenet_menu.cpp, the
  `rules_line` call) appends the list name when it isn't All, e.g.
  "Free-for-all · 4 stocks · 8:00 · Low tiers (12)". A guest reads it from
  `PcXnListing.fighter_list`.
- **The notice slot** gets `xeleenet.css.off_list`: "{name} isn't on this
  lobby's list", for 2 s, local only.
- **The service:**
  - tools/xeleenet/checks.go Settings gains `FighterList`, through
    `pick(m["fighter_list"], {"all","high","low","custom"})`. A missing field
    reads as all, so old clients still validate.
  - test_service.py checks that a bad value is refused.
- **The client:** xeleenet.c `parse` reads `fighter_list` into
  `PcXnListing.fighter_list[8]`.
- **LAN:** the settings JSON gains about 24 bytes, which puts the longest at
  about 222 of the TXT string's 255. The masks don't fit, so they wait for the
  details plan's own TXT keys.

### F5. Tests
- **`net_test_xn_menu.py --scenario fighters`, 3 players:**
  - The host's rules file (set_rules) has `fighters=` set to the Low tiers
    mask.
  - A guest's hand goes to Fox and presses A: every log shows
    `css: P2 A on off-list`, and no `P2 picked`.
  - The guests then drop on Random: every log names the same fighter, and it's
    in the Low mask.
  - A host shot of the dimmed grid and the notice (on the guest).
  - Then a normal match, with no DESYNC.
- **The CSS reload keeps only on-list picks.** Covered once the overlay
  changes lists mid-lobby; until then, a unit-style check of the kept-pick
  filter.
- **The existing scenarios** (default, ready, leave-guest, multipad, sss-*)
  stay green: a lobby with All behaves exactly as before.

## Risks

- **Every player's CSS must agree on the mask** before the first frame of the
  CSS. RULES is applied before the CSS (`xnApplyRules`, then the handshake),
  as the stage mask already is. The risk is a guest seeing the CSS before its
  RULES land. It's the same as today's stage mask, which hasn't shown it.
- **The dim helper** runs every frame over 25 portraits. The stage select
  does 29 at no measurable cost.
- **Melee's random-all-unlocked path** (mncharsel.c:2833-2856) picks with
  `mnCharSel_8025FB50`, so it inherits the filter.
- **Sheik picked by holding A** at the match load is allowed whenever Zelda
  is, which is the point of decision 2.
