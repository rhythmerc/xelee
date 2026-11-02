# Game modes, starting with Capture the Flag (plan)

Status: draft, 2026-11-01. Nothing is built. This is for review before the
wireframes and the build.

A **game mode** changes how a match is played and won. Modes are offered in
XeleeNet lobbies, LAN lobbies and offline VS. A mode can narrow the lobby's
settings: which stages, whether teams are on, and what wins. The first mode is
**Capture the Flag (CTF)**, on Hyrule Temple only until custom maps exist
(`docs/custom-content-report.md`).

Related: `docs/xeleenet-plan.md`, `docs/xeleenet-ready-plan.md`,
`docs/adding-menu-pages.md`.

## 1. Decisions

User decisions, 2026-11-01:

1. **The flag is a real item,** picked up like any other. Touching it does
   nothing for either team.
2. **No attacks while holding it.** Throwing is the only attack, and a throw
   can hand the flag to the enemy.
3. **Bases on Temple:** bottom-left and top-right corners.
4. **You respawn at your base.** The respawn delay is the only cost of a KO;
   KOs don't score.
5. **A capture** is the enemy flag reaching your base, held or thrown. A
   thrown flag that lands in your base counts.
6. **Your own dropped flag:** when your team picks it up, it goes home at once.
7. **Your own flag at your base stays put.** Your team can't lift it; only the
   enemy can.
8. **"Your flag must be home to score"** is a setting.
9. **Offline too,** not only XeleeNet and LAN. Its setup needs a wireframe.
10. **Specials are blocked while carrying.**
11. **Getting hit:** Melee's chance roll, biased higher than for a stock light
    item.
12. **Teams swap bases each game of a set.**
13. **Auto-return:** a free flag nobody picks up for 10 s goes home. It blinks
    for the last 3 s as a warning.
14. **Win, for now:** first to 3 captures, 8 min limit. A tie at time is **No
    Contest**, with no sudden death. To be tuned by playtesting.
15. **Carrying slows you,** on by default.
16. **Other items off** by default in CTF.
17. **The stage select is never skipped.** Stages the mode can't use are
    dimmed and disabled with the existing off-list logic, which leaves room for
    more CTF maps later.
18. **A capture is a sound only,** with no on-screen banner. The score strip
    ticks up.
20. **Players start the match at their base** too, not only respawn there.
    Melee's own start points for the stage are not used in CTF.
21. **The wireframe** (`game-modes-ctf-wireframe.html` on the share) was
    approved with every recommended choice: a Game Modes row in VS Mode, a
    rules page before the CSS, a Mode bar above the VS presets, a Z mode filter
    on the lobby list, red/blue only, a top-centre score strip, an outline on an
    empty stand, and a sides card at the start of every game.

## 2. The mode framework

### 2.1 What a mode declares

A mode is a static table entry in the sim (`src/melee/gm/gmmode.c`, new):

| Field | CTF |
|---|---|
| id, name key | `ctf`, `mode.ctf` (lang YAML) |
| Required stage capability | a CTF layout (§3.2) |
| Teams | forced on, exactly 2 teams (red, blue) |
| Seats | 2-4, any split (1v1, 2v1, 2v2) |
| Win | captures, plus a time limit |
| Stocks | none: endless respawns (Melee's time mode underneath) |
| Its own settings | §3.6 |
| Hooks | match start, per-tick update, item pickup gate, respawn position, match end |

**Stages are a capability, not a list.** A stage is legal for a mode when it
provides that mode's layout data. Today only Temple has a CTF layout, so CTF is
Temple-only. A custom map that ships a CTF layout later becomes legal with no
change to the mode.

**The stage select always runs** (decision 17), even with one legal stage.
The mode's legal set is ANDed with the lobby's stage list. Everything outside
it uses the existing off-list path: dimmed, can't be picked, never drawn by
Random (`sss_off_list`, `mnstagesel.c:218`). Today that path is gated on
`gmOnline_XnFlow()`. It becomes "XeleeNet, or a mode is in force", so offline
CTF dims the same way. The picks card, timer and host FINAL are unchanged; with
Temple the only legal stage, every pick lands on Temple. When the mode is on,
the host's stage list row shows the mode's legal stages only.

### 2.2 How the mode travels

| Layer | Change |
|---|---|
| Host settings | `HostSettings` gains `game_mode` (`xeleenet_menu.cpp:213`). Mode is the first row, above the preset bar. Picking CTF locks Teams on, hides Stocks, limits the stage list to the mode's legal stages (Temple today), and shows CTF's rows. `plays_same()`, `from_rules()`, `rule_changes()` and `xeleenet-lobby.txt` learn the field. |
| Game rules | `PcXnRules` gains the mode and its settings (`xeleenet_menu.h:57`). `xnApplyRules` (`gmonlinemode.c:1648`) sets the mode on the match. The match start goes through a `start_cb` like `rankedRules` (`gmonlinemode.c:351`). |
| Wire | The RULES `Rules` payload gains the mode byte plus the CTF settings, before `unlock_hash`, so `rules_content_hash` covers them (`net_internal.h:265`). This bumps `PC_NET_PROTO_VERSION`. |
| Service | `"mode"` already means teams/ffa, so the new key is **`game_mode`**: `vs` or `ctf`, checked by `pick()` (`checks.go:289`). It goes before the masks, which LAN truncates (`xeleenet_lan.c:325`). The listing carries it, and the browser shows a mode tag and can filter by mode. |
| Version gate | Nothing new needed: `Compat` is an exact match on build (`checks.go:150`), so older clients never see a CTF lobby. |

### 2.3 Offline

VS Mode gets a 6th row, **Game Modes**. It goes to a mode picker (CTF only at
first), then a CTF rules page, then the CSS with teams forced, then the stage select (Temple only lit). It
is a new `GM_*` mode modelled on `gmgiant.c`, reusing `gmVsMelee_*` with a
`start_cb`. The stage select runs with the mode's off-list dimming (§2.1). It is not a Special Melee entry, because
Special Melee already uses all 10 row joints (to be confirmed). The screens
need a wireframe (§6, M5).

## 3. CTF

### 3.1 The flag item

- **Template: the Poké Ball** (`it/kinds/itmball.c`), a light throwable
  (`x0_78 == 0`) with simple states: ground idle, fall, held, thrown, air.
  Drop its opening states (5/6), and make landing return to ground idle. Bob-omb
  and Mr. Saturn carry far more logic.
- **Heavy items don't fit.** A heavy item blocks jumping, and going from the
  bottom-left to the top-right on Temple needs jumps.
- **New kind, with no ItCo.dat article.** It gets a slot appended to the logic
  and render tables (`it_803F14C4`, `it_803F1418`). It reuses the Poké Ball
  article for attributes and hurtbox. Drawing is our own: a pole and a cloth in
  the team colour, drawn by a PC-side draw callback.
- **Actions while holding** (`ftCo_Wait.c:44-66`):
  - A, smash, Z, C-stick and air A throw it (LightThrow*,
    `ftCo_ItemThrow.c`). That's Melee's behaviour already.
  - Jump, shield and dodges stay. Throwing out of an air dodge stays possible
    (`ftCo_EscapeAir.c:84`).
  - **Specials are blocked** (decision 10). Melee doesn't block them for
    light items, so we gate the four ground special checks and
    `ftCo_SpecialAir_CheckInput` on `itGetKind(fp->item_gobj) == flag`. A
    carrier knocked off stage has no up-B: throw the flag back on stage, or
    lose it to the blast zone (it goes home).
- **Getting hit** (decision 11): Melee drops a light item when
  `HSD_Randi(x418) < this hit's damage` (`Fighter_8006CDA4`, `fighter.c:2749`;
  the damage is `dmg.x1838_percentTemp`; x418 is from PlCo.dat, value not yet
  read). For the flag, the hit's damage is multiplied by a bias before the roll
  (start at ×3, tuned by play), and nothing else changes. The roll draws from
  the seeded HSD RNG, so it is deterministic.
- **Traps to avoid** (from the survey):
  - The pickup despawn timer (`it_2725.c:960`). The flag sets `xDD0_flag.b3`.
  - The random destroy after N throws (`it_8026DDFC`, `itgroundcoll.c:320`).
    The flag lands through `it_8026E414`, which skips that check.
- **Items: Off.** ItCo.dat loads in VS regardless (`gmvs.c:2371`), so the mode
  spawns its flags with random items off. Not yet tested at runtime. Whether
  other items spawn in CTF is a CTF setting (§3.6).

### 3.2 The layout (per stage)

The CTF layout for a stage is data in a table keyed by stage id. Later, custom
maps provide it as a file. For each team:

- **Flag stand:** a position. The flag spawns there and returns there.
- **Base zone:** an axis-aligned box. A capture is the enemy flag's position
  inside this box.
- **Spawn points:** up to 2. Players start the match there, and respawn
  there (decision 20). A team's players take them in port order.
  - **Respawn:** override per port after `fn_8016719C` (`gm_1601.c:3489`)
    with `Player_80032FA4(slot, -1)` and `Player_SetSpawnPlatformPos`.
  - **Match start:** each player's start position comes from the stage's
    points 0-3 (`Stage_80224E64`). CTF puts its own positions there instead
    (M2).

**Temple (proposed, from the M0 survey, 2026-11-01).**
`MELEE_DEBUG_CTF_SURVEY=1` dumps the stage's collision lines, bounds and
named points; `tools/ctf_survey_draw.py` draws them to scale
(`temple-ctf-survey.png` on the share). Units are the game's.

| | Red: bottom-left | Blue: right edge of the ground floor |
|---|---|---|
| Floor it sits on | lines 14-15, y -88.6, x -134.0 to -75.2 (the cave under the left mass; ceiling at y -28) | lines 36-37, y about -5, x 124 to 225.6 (the main floor's right end; the high platform's lower deck is above, at y 47) |
| Flag stand | (-105, -88.6) | (200, -5.3) |
| Base zone | x -134 to -75, y -88.6 to -50 (59 wide) | x 170 to 225.6, y -5.3 to 30 (56 wide) |
| Spawn points (start and respawn) | (-125, -70), (-88, -70) | (182, 22), (214, 22) |

The user moved blue from the top-right platform (2026-11-01): from there a red
flag could be thrown into red's base, but nothing could be thrown up into
blue's. The right edge is open to the blast zone (x 315), while red's base is
inside the cave. Watch that asymmetry in playtests.

Others from the dump:
- **Blast zone:** x ±315, y -257.4 to 207.
- **Camera bounds:** x ±279, y -201.6 to 180.
- **Melee's own VS points (unused in CTF):** starts (-92.7, 21.6), (91.8, 4.5),
  (-27.0, 74.7), (27.0, -10.8); the respawn (18, 84.6), the same for every port.

Starting and respawning inside the cave need checking in M2. Melee's rebirth
drops in from the camera's top, and its match entry may assume open space
above a start point.

### 3.3 Rules, tick by tick

All of this runs in the sim, once per tick, after fighters and items.

- **Pickup gate:** a team can't pick up its own flag while it's on its stand.
  This hooks the item pickup check; where Melee decides pickup eligibility is
  still to be found.
- **Return:** your team picks up its own flag anywhere else, and it teleports
  to its stand (in `picked_up`, before the fighter really holds it).
- **Capture:** the enemy flag's position is inside your base zone, either held
  by your teammate or as a free item (thrown, dropped, or landed). If "Flag
  must be home" is on, your own flag must also be on its stand. On a capture,
  the team scores, the flag returns to its stand, and a sound plays (no banner: decision 18).
- **Blast zone / off stage:** if the flag goes past the blast zone or falls
  below the stage, it returns to its stand.
- **Auto-return:** a free flag that stays free for 10 s returns to its stand.
  The timer resets when someone picks it up. For the last 3 s the flag blinks;
  the draw reads the timer, and the sim is unaffected.
- **Win:** first to N captures ends the match at once. When time runs out,
  most captures wins; a tie is **No Contest** (no winner; in a set it doesn't
  count for either side).
- **Bases swap each game of a set:** the team that had bottom-left gets
  top-right next game.

### 3.4 State, rollback and desync checks

- **CTF state lives in statics under `src/melee`** (`gmctf.c`): flag item
  handles, stand/free/held state, captures, timers. Those statics are in
  `libmelee_game.a`'s `.data`/`.bss`, which the rollback snapshot saves
  (`net_snapshot.c:694`). Nothing in `src/pc/*`.
- **The per-frame checksum** (`frame_checksum`, `net_snapshot.c:308`) covers
  fighters only. Fold in the captures and each flag's state and holder, so a CTF
  split shows the frame it happens. This is part of the same protocol bump as
  §2.2.
- **The draw callback and HUD only read state;** they never write it.

### 3.5 HUD and results

- **The HUD is an overlay outside the sim,** like the XeleeNet overlays. It
  shows captures per team and each flag's state (home, carried, free, with the
  return countdown). In 3D it sits on the stage frame; flat, it's a top strip.
- **The results screen:** Melee's time-mode results rank by KOs. CTF sets the
  winning team from captures; the captures column is still to be designed. In
  a set, the score is games won.
- **The camera:** on a flat screen, Temple from corner to corner zooms far
  out. In 3D, the tabletop view of the whole map suits it. Flat camera tweaks
  wait for real play.

### 3.6 CTF settings

| Setting | Default | Range |
|---|---|---|
| Captures to win | 3 | 1-9 |
| Time limit | 8 min | 3-15, or none |
| Flag must be home to score | on | on / off |
| Auto-return | 10 s (blinks the last 3 s) | 5-30, or off |
| Respawn delay | Melee's default | proposal: a setting later, if play says so |
| Carrying slows you | on | off / on (walk and run speed ×0.8 to start, tuned by play) |
| Other items | off | off, or the lobby's item settings |

## 4. Tests

- **Frame-script scenarios** with `MELEE_DEBUG_VS_STAGE=14`, unpaced
  (`docs/fast-tests-plan.md`): pick up the enemy flag, carry it home, capture;
  throw it into base, capture; get hit, drop; teammate picks up own flag,
  returns; own flag on stand, can't pick up; off stage, returns; auto-return;
  first to N ends the match.
- **Determinism:** `MELEE_NET_SYNCTEST` over a scripted CTF match, then
  `tools/net_test_n.py --players 4` with a CTF preset at 50 ms / 2%.
- **Service:** `test_service.py` checks for `game_mode` (valid, invalid,
  listed, filtered).

## 5. Risks

- **Adding an item kind past ItCo.dat's articles.** Loops are bounded by
  `It_Kind_L_Gun_Ray` or by 238 (`itspawn.c:56`, :212, :255). This is the first
  spike.
- **Melee's results and stats** assume KOs or stocks. CTF match end may hit
  paths that look up stock winners.
- **No up-B while carrying** may make Temple's edges too punishing. Watch it
  in playtests; the fix would be letting up-B through only.
- **Temple's camera on flat screens** may be unplayable at corner-to-corner
  distances. Play first, then decide.
- **Snapshot coverage:** a mode static in a translation unit excluded by
  `melee_state.ld` would silently not roll back. Check `gmctf.c`'s placement.

## 6. Milestones

| | What | Done when |
|---|---|---|
| M0 | Spikes: a new item kind spawns, is held, thrown and lands (Poké Ball clone); the Temple survey (corner positions, captures shared) | the clone flies on Temple; base numbers proposed |
| M1 | The flag item: states, no despawn, no random destroy, specials gate, drop rule, team-colour draw | frame-script pickup/throw/drop pass |
| M2 | CTF rules in the sim: stands, pickup gate, return, capture (held and thrown), blast zone, auto-return, win, start and respawn at base, checksum | §4 scenarios and synctest pass |
| M3 | HUD and results | screenshots flat and in 3D |
| M4 | The mode in XeleeNet and LAN: host row, wire, service `game_mode`, browser tag, SSS off-list dims by mode | 4P CTF net test; service checks |
| M5 | Offline: Game Modes row, mode picker, CTF rules page (wireframe first) | wireframe approved, then a played match |
| M6 | Headset and real play, then tuning (camera, defaults) | a played set across the network |

M5's wireframe can be done in parallel with M0-M2. M4 also needs the
XeleeNet mode row in the wireframe.

## 6a. M0 results (2026-11-01)

- **The item spike passes on Temple:**
  - `It_Kind_Ctf_Flag` (238, past every disc table) borrows the Poke Ball's
    article.
  - A frame script on port 1 picked it up with A, carried it, jumped and threw
    it with an air A; it landed back to its ground state.
- **Facts learned:**
  - **Pickup:** an item is grabbable while `xDC8 x15` is set (`Item_IsGrabbable`),
    so every landing sets it (`it_8026B390`).
  - **No despawn:** the despawn timer and its blink apply only to kinds below
    `It_Kind_L_Gun_Ray`, so the flag never despawns.
  - **No random destroy:** landings go through `it_8026E414`, which skips the
    random-destroy count.
- **Debug hooks** (`gmctf.c`, only when the variable is set):
  - `MELEE_DEBUG_CTF_FLAG=1|x,y` spawns a flag.
  - `MELEE_DEBUG_CTF_LOG=N` logs port 1's and the flag's positions every N
    frames.
  - `MELEE_DEBUG_CTF_SURVEY=1` dumps the stage's collision.
- **Not done in M0:** rollback determinism with a flag in play. It goes with
  M2's synctest and net test, once the rules exist.

## 6b. M1 results (2026-11-01)

- **The look** (`itCtfFlag_Draw`; the borrowed model is never drawn):
  - an 18-unit pole (Melee's lit hitbox capsule, `lbColl_80008FC8`) with a
    gold knob;
  - a two-tone cloth in the team colour, red or blue. It is unlit, waves on
    wall time, and trails behind a carrier.
  - Held, it is drawn at the hand part stored at pickup (`ip->xDC4`). The
    item's root jobj stays where it was picked up; Melee pins a model bone to
    the hand instead.
  - It blinks while the return timer is in its last 3 s (wired up in M2).
- **Item variables:** `xDD4_itemVar.ctfflag` holds `{team, return_left}`.
  `MELEE_DEBUG_CTF_TEAM` sets the debug flag's team.
- **Specials blocked** while carrying. The gate is on `itCtfFlag_Is(fp->item_gobj)`
  in the six special input checks (`ftCo_SpecialS.c`, `ftCo_Attack100.c` ×4,
  `ftCo_SpecialAir.c`).
- **Drop bias:** Melee's roll is `HSD_Randi(60) < hit damage`. The flag
  multiplies the damage by 3: a 12% hit drops it 60% of the time (20% for a
  normal light item), and 20% or more always does.
- **Carry slowdown:** walk max, dash initial and dash max are ×0.8, applied
  in `ftCo_800D105C`. gmctf.c's tick re-runs it for a fighter when its
  carrying changes.
- **Melee's CPUs** pick up and carry the flag like any item.
- **Tests:** `tools/ctf_test.py` runs `carry` and `drop` unpaced (about 6 s
  for both); both pass.
- **Captures:** window screenshots go stale under Xwayland after the first.
  Record with ffmpeg x11grab instead (see the memory on menu frame capture).

## 6c. M2 results (2026-11-01)

**The rules** (`gmctf.c`, statics of the game library, so they roll back):
- **Sending a flag home** is always destroy-and-respawn on the stand. A holder
  lets go through `ftLib_ReleaseItem`. It happens on:
  - a capture;
  - your own team picking it up away from home (any pickup path, then the
    next tick);
  - the return timer;
  - leaving the stage (`gmCtf_FlagDestroyed`, then a respawn);
  - being shoved more than 12 units off its stand while home.
- **Pickup gate:** `gmCtf_PickupBlocked` in both of `ftpickupitem.c`'s range
  checks.
- **Capture:** the enemy flag's position (its holder's while held) inside
  your zone, with your own flag home if that rule is on.
- **Start:** a hook at the top of `gmvs.c`'s `getSpawnPoint`.
- **Respawn:** `gmCtf_Respawn` after `fn_8016719C`. It keeps Melee's revival
  point and offsets it (`Player_SetSomePos`): a descent to a bare position
  collides with the stage and stopped on the roof of red's cave.
- **Match end:** `gm_GetMatchOutcome` reports a team win once a team reaches
  its captures (GAME!, then the red or blue team wins). A tie at time is no
  contest. The results score (`fn_8016588C`) is the team's captures.
- **Rules carriage:** `GmCtfRules` is set after `gm_SetupRulesDefaults`, which
  clears it, so no other match inherits it. The debug match is
  `MELEE_DEBUG_CTF=1` plus `_WIN`, `_HOME`, `_RETURN`, `_SWAP`, `_BASES` and
  `_FREE`.
- **Checksum:** `frame_checksum` folds in `gmCtf_Checksum` during CTF only, so
  every other match's checksum is unchanged.

**Tests:**
- `tools/ctf_test.py` runs 15 scenarios unpaced; all pass: carry, drop,
  capture, throw, gate, own, timer, blast, needhome, nohome, win, tie, start,
  respawn, swap.
- **Two-peer net tests,** 2 min at 50 ms and 2% loss, CPUs at level 9; both
  PASS with no DESYNC and about 1,500 rollbacks a side.
  - On the real layout the CPUs never touched the flags.
  - On the close test layout there were 10+ pickups: carried and thrown
    captures, timers and knock-offs, all under rollback.

**Seen on the way:**
- **Item reach is long.** A CPU grabbed a flag about 18 units away while
  standing in its own zone. Base zones on future maps must sit well clear of
  the enemy stand.
- **No contest shows GAME!,** not TIME!, even when time ran out (Melee's
  outcome banner). The results screen says NO CONTEST. Revisit with M3.
- **Duplicated log lines** in netplay logs are rollback re-runs of the tick.

## 6d. M3 results (2026-11-01)

- **The HUD** (`src/melee/if/ifctf.c`) is Melee's own SIS text on the HUD
  camera (`ifnet.c`'s recipe) plus a GX panel on the same link (0xB), so it
  draws on the headset's HUD plane too. That's unverified on a headset. An
  RmlUi overlay was ruled out: RmlUi never draws into XR eye passes.
  - **The strip,** under the timer: red's flag state, the score in team
    colours, blue's flag state. A state reads HOME, `P# HAS IT` or
    `FREE n`; FREE blinks in its last 3 s.
  - **The sides card** during READY (the first 2 s), below Melee's "Ready"
    and gone before "GO!" covers it: CAPTURE THE FLAG or SIDES SWAPPED,
    then RED <base> and BLUE <base>.
- **The stands** (`itCtfFlag_DrawStand`, drawn from gmctf.c's process on the
  stage's link): a team-colour plate always, and a translucent ghost pole and
  knob while the flag is away.
- **The capture sound** is the trophy chime, SFX `0xAA` (`itcoin.c:368`, the
  Classic/Adventure trophy catch). It is the user's suggestion; I haven't
  heard it yet.
- **Tests:** `tools/ctf_test.py` 15/15. The CPU net test with the close
  layout passes with 4 captures and the HUD live: no desync, about 1,480
  rollbacks a side.
- **Left:** the GAME!-for-no-contest banner (Melee's), a look on the headset,
  and the blink and countdown on a real flag in a long match.

## 6e. The HUD, redesigned (approved 2026-11-02)

The M3 HUD (SIS text and flat panels) wasn't polished enough. A
high-fidelity mockup on real 4K frames (`ctf-hud-mockup/` on the share) was
approved as shown:

- **Layout A, either side of the timer, on its row.** Red is mirrored left,
  blue right: the score nearest the clock, the flag state outermost, 255 px
  (at 4K) clear of the timer.
- **Layout B, no timer:** the same two blocks meet at the top centre.
- **The score:** Melee's own damage digits (the 32x36 IfAll textures), white
  with a black outline, with a team-coloured flag emblem behind each number,
  as the series emblem sits behind a damage %. No plate.
- **Flag states as icons, no text:**
  - **home:** the flag on its plate;
  - **carried:** the flag plus the carrier's stock icon (Melee's own, in
    their costume colours);
  - **free:** the flag tipped inside a gold ring that drains to the return.
    In the last 3 s the ring turns white and the flag fades, blinking. No
    seconds number.
- **READY:** a light beam rises from each base, a column with team-coloured
  rings rising up it, with no dark edges. The READY camera frames both bases
  (6ada59cf), so both beams are on screen.
- **The capture sound:** the trophy chime, SFX 0xAA.
- **Replaced:** the M3 strip under the timer and the sides card.
- **Build notes:**
  - The digits and stock icons come from Melee's HUD assets (the same models
    and textures the damage display uses).
  - The emblem, flag icons, ring and rings-on-beam are new textures, drawn
    as GX quads on the HUD camera.
  - The beams are drawn in the stage's 3D pass, so in the headset they stand
    on the stage.
- **Mockup tooling:**
  - `MELEE_TEXTURE_DUMPS=1` dumps textures, red/blue swapped through
    ImageMagick.
  - `MELEE_DEBUG_CTF_NOHUD=1` gives clean frames, and
    `MELEE_DEBUG_CTF_NOTIME=1` a match without a timer.

## 6f. The HUD build (2026-11-02)

The approved design (§6e) is in the game.
- **Score** (`ifctf.c`): Melee's damage-number model (`damage_num_joint`)
  per team, with only the ones digit showing, set by its texture animation
  (frame 2 × digit).
- **Carrier icon:** the stock model, built as the HUD's own heads are in a
  time match (`ifStock_802F98E8` mode 1). The other nodes are hidden one by
  one, and only the icon's texture is animated, since the joint animation's
  visibility tracks would hide it again. `ifStock_802F96D0` doesn't draw
  here.
- **New art** (`tools/gen_ctf_art.py` → `src/pc/ctf_art_data.c`, GX RGBA8,
  160 px): the flags with and without the plate, and the white emblem.
  Drawn as textured quads (`pc_ctf_art_quad`).
- **The ring:** feathered arcs (`pc_ctf_arc`).
- **The beams** (`pc_ctf_beam`): a coloured body, an additive core, a floor
  ring and rising rings, drawn in the stage's pass during READY with depth
  testing.
- **Layout:** A when the match has a timer, B otherwise. The row is the
  timer's (17.65 HUD units up).
- **Traps:**
  - Linking a GObj that is already linked (`GObj_SetupGXLink` twice) makes
    a cycle in the draw list, and the game hangs.
  - `src/pc` files can't include `cobj.h` (they don't get the game's compat
    header).
- **Tests:** `tools/ctf_test.py` 15/15, and the CPU net test passes (5
  captures, no desync).

## 6g. M4 results (2026-11-02)

CTF is a XeleeNet (and LAN) lobby mode.
- **Host page:** a Mode bar beside the Preset bar (VS / Capture the Flag).
  The CTF page shows captures to win (1-9, default 3), flag must be home,
  return time (never, 5-60 s, default 10), carrying slows, plus the usual
  seats, minutes, and fighters. Up/Down walks Mode, then Preset, then the
  rows; Left/Right on a bar changes it. The stage list is locked to Temple ("Temple only in Capture
  the Flag").
- **Forced by the mode:** teams on, a timed match, and the Temple stage
  mask. Forced everywhere: the menu (`pc_xn_menu_rules`), the RULES the
  host sends, and the service (`checks.go` refuses a CTF lobby without
  them). The CSS allows only two teams in a CTF lobby (`cssTeamCount`).
- **Wire:** `PcNetMode {mode, caps, home, return_s, slow}` rides at the end
  of RULES (+5 bytes, Rules is 33). `PC_NET_PROTO_VERSION` is 14, so a
  13 peer can't join a 14 lobby.
- **Match start** (`xnModeRules` in `gmonlinemode.c`): the RULES in force
  set `gmCtf_SetRules`. The sides swap every other game of a session. The
  count restarts when the session seed changes, so every peer agrees.
- **Lobby list:** a red "CTF" tag and "First to N caps" on CTF rows. Z
  filters the list by mode (all / VS / CTF).
- **Service:** `game_mode` and `ctf_caps` in the listing JSON.
  `test_service.py` covers both: a CTF lobby without teams, time, or the
  Temple mask is refused.
- **Tests:** `net_test_xn_menu.py --scenario ctf` (a 3-player lobby through
  the real menus and the local service: every player's match is CTF on
  Temple from the host's rules) passes (no desync; a
  scoreless 1-minute match ends No Contest). The default scenario still
  passes; its Preset press now steps Down from the Mode bar first. `tools/ctf_test.py` is 15/15.

## 7. Open questions

All seven from the first draft were answered 2026-11-01 (decisions 10-17).
Left for playtesting: the drop bias, the carry slowdown, the win defaults,
and the flat camera.
