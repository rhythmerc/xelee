# XeleeNet: a lobby's Details before joining

Plan, 2026-10-30. The design is sections 4 and 6 of the approved wireframe
`xeleenet-fighter-list-wireframe.html` (tailnet share, melee-pc/).

This plan assumes docs/xeleenet-fighter-list-plan.md (F1-F4) is done. That
plan leaves three things to this one:
- the masks in the listing;
- LAN's TXT keys;
- portrait thumbnails.

Line numbers in xeleenet_menu.cpp move as the fighter list lands, so its
function names are the anchors.

## Goal

A on a lobby row opens a Details page:
- the lobby's players, seat by seat;
- its fighters, as a miniature CSS grid with a names line;
- its stages, by name;
- its rules.

A on Details joins through the existing Joining flow. B goes back to the list
with the same row highlighted. Details is the same page on XeleeNet and on LAN;
only the place line differs. Each row's second line names the fighter and stage
lists when they aren't the defaults.

## Decisions

Approved (wireframe §4, §7):
1. **B1:** A on a row opens Details, and A on Details joins. B goes back with
   the same row highlighted.
2. **Full lobbies** can be highlighted and opened. Their Details shows only B.
3. **Row summaries** end with the fighter list, then the stage list, each only
   when it isn't the default (All fighters, Competitive stages). The stage part
   is dropped first when the line runs out of room.
   - Fighters read "High tiers", "Low tiers", "Marth only" or "6 fighters".
   - Stages read "All stages", "Final Dest. only" or "Custom (n)".
4. **Details is one page:**
   - the top lines: the name, seats taken, the place (the region, or the host's
     address on LAN), the host and the rules line;
   - Players: one row per seat in port order, with HOST and XR tags, open seats
     dashed;
   - Fighters: a 9/9/7 portrait grid, dimmed where off, with a names line under
     it ("Off: …" or "Only: …", whichever is shorter, or "Every fighter");
   - Stages: by name.
5. **Off-list fighters are dimmed only, never struck** (the user's call, to
   match the stage select).
6. **Text lines never go below 12 wireframe units.** B1 maps the wireframe's 14
   to 27 px, so 12 units is about **23 px** on the 1120×660 board.
7. **The presets** are All, High tiers and Low tiers (`XN_FIGHTERS_*`,
   src/pc/xeleenet_fighters.h).

Taken here:
8. **The seated players go in every list row,** not in a per-lobby GET:
   - Details opens at once, with no loading or failure state.
   - It's the same shape LAN gets from the announcement.
   - The list already refreshes every 10 s.
   - The worst case is about 33 KB for 50 rows.
9. **Off-list stages in Details are dimmed too,** not struck, following
   decision 5.
10. **Portraits come from MnSlChr, loaded at menu time.** The Details grid
    falls back to text cells until they're decoded (D5).
11. **The masks travel as numbers:** `fighter_mask` and `stage_mask` in the
    service's settings object. On LAN they get TXT keys of their own (`fm`,
    `sm`), because they don't fit in `s`.

## What the code does today

| Area | Where | Today |
|---|---|---|
| Rows | `xeleenet_menu.cpp` `build_lobbies` | The second line is `host#tag4 · stocks · time · first to/endless`. A full row is `Item{…, !full}`, so it can't be highlighted. A on a row calls `join()` directly. |
| Legend | `legend()` | On Lobbies, A reads "Join". |
| Polling | `poll_browse`, `update` | Both run only while `page == Page::Lobbies`. |
| Listing | `xeleenet.h` `PcXnListing` | A count (`taken`/`seats`), the list names (`stage_list`, `fighter_list`) and `xr`. No players, no masks. |
| Parse | `xeleenet.c` `parse_settings`, `parse_listing` | Read the service's settings object and row. |
| Reply buffer | `xeleenet.c:27` `REPLY_CAP 16384` | A full 50-row list is already about 17.5 KB. The HTTP sink truncates it (`slp_http.c:31-40`), the JSON fails to parse, and the list fails. **This is a bug today**, and D1 fixes it. |
| XR | `xeleenet.c` host/join body, heartbeat | Both send `"xr":false` hard-coded, so B1's XR badge never shows. |
| Service list | `tools/xeleenet/lobby.go` `list` | Each row has code, name, host, country, `taken`, `settings`, `state`, `xr` and `created`. Members (`Member`, with name, tag, xr and port) are kept but not listed. `Extra` holds multi-pad seats. |
| Settings | `checks.go` `Settings`, `settings()` | Named lists only, through `pick()`. |
| LAN announce | `xeleenet_lan.c` `host_announce` | Eleven keys: `v rev disc id name host tag seats taken port s`. `s` is the settings JSON. Re-announced every 3 s and on joins and leaves. No xr, playing or address. |
| LAN browse | `xeleenet_lan.c` `browse_tick` | Rows come from TXT and `pc_xn_listing_settings`. The record's `ip` (`net_mdns.h:23`) isn't kept. |
| TXT limits | `net_mdns.h:17` `PC_MDNS_TXT_MAX 16` | One string per key, at most 255 bytes including `key=`. A non-printable value drops the whole record (`net_mdns.c:387`). |
| LAN wire | `LJoin` | Has a spare `reserved[2]`, covered by the signature. `LWelcome.settings[320]`. Both sides are the same build (`same_build`), so the wire can change without a version step. |
| SSS thumbnails | `mnstagesel.c` `xnSss_Thumbs` | Hands each icon's C8 image and palette to `pc_xn_sss_thumb`. That decodes to RGBA, served by the `xnstage://` provider (`register_sss_textures`, `aurora::rmlui::register_texture_provider`). |
| CSS art | `mncharsel.c` | MnSlChr is loaded only by the CSS. `cssSlippiTitle` already builds the menu model outside Melee's flow. `icons[]` has `joint_id_vs`. |
| Menu archives | `mnxeleenet.c` | Already loads and frees MnExtAll for the keyboard. MnSlChr is in the preload list (`file_cache.cpp:65`). |

## The data

**The service** (`checks.go` `Settings`):
- Gains `FighterMask` (`fighter_mask`) and `StageMask` (`stage_mask`).
- `settings()` checks each one:
  - not zero;
  - fits (26 bits for fighters, CharacterKind 0-0x19; 29 for stages);
  - Sheik's bit equals Zelda's;
  - a named list matches its mask.
- A missing mask reads as its named list's mask, so old clients and
  `xn_cli.py` still validate. `custom` with no mask is `bad_settings`.
- The Go constants copy xeleenet_fighters.h and `kStages`, with a comment
  pointing at both.

**The service** (`lobby.go` `list`):
- Each row gains `"players"` in port order: the host at port 0, each member as
  `{port, name, tag, xr}`, and each `Extra` seat as `{port, extra:true}`.
- Never keys or candidates.
- The row's `xr` stays the OR over the players.

**The client** (`xeleenet.h`):

```c
typedef struct PcXnSeat { char name[16], tag[12]; uint8_t port; bool xr, extra; } PcXnSeat;
/* in PcXnListing: */
uint32_t fighter_mask, stage_mask; /* 0 when unknown: read as the named list */
int nplayers; PcXnSeat players[4];
char address[48];                  /* LAN: the host's lobby address (":port" off 42424) */
```

`PcXnMember` gains `bool xr`.

**LAN TXT**, 15 of 16 keys:
- `s`: the settings JSON without the masks. `settings_json` writes the masks
  last, and the host cuts `s` before them (about 222 bytes).
- `fm`, `sm`: the masks, in hex.
- `g`: `1` while playing.
- `p`: the seats, each written as
  `<port><xr 0/1><tag, 8 chars><name length, 1 hex digit><name>`, with no
  separators to escape. An extra pad is `<port>0--------0`. At most 92 bytes.
- `WELCOME.settings[320]` carries the full JSON, masks included.

## The pages

**B1** (`build_lobbies`):
- Every row is enabled, and A opens Details.
- The legend's A reads Details.
- The row hint is "See this lobby's fighters, stages and players before you
  join."
- The summary is `fighters_summary(x)` and `stages_summary(x)`. The stage part
  is dropped when `Rml::ElementUtilities::GetStringWidth` on a hidden `.ls`
  measure element says the line is wider than `.lw` (600 px). On the first
  frame, before that element has a font, a 62-character budget stands in.

**Details** (`Page::Details`, title "Lobby"), on the board:
- **Top lines:**
  - the name at 38 px, an IN GAME badge, "{taken} of {seats} seats", and the
    place right-aligned at 23 px;
  - then `host#tag · rules_line(…) · First to n / Endless` at 24 px.
- **Left column:**
  - PLAYERS: one 46 px row per seat, up to `seats`, showing `P{n}`,
    `name#tag4` at 25 px, and HOST or XR badges. An extra pad reads "Second
    controller", and an open seat is a dashed "Open seat".
  - STAGES, with the list label right-aligned: name chips three across at
    23 px, in `kStages` order (up to 15 fit, which covers the worst case). It
    shows the on-list names, or the off-list ones under "Off:" when that's
    shorter. With all 29 on, it reads "Every stage".
- **Right column:**
  - FIGHTERS, with `fighter_list_label` right-aligned.
  - The grid: `xn_fighter_order` in rows of 9, 9 and 7, with 60×52 cells.
    Off-list cells are at opacity 0.3.
  - Under the grid, the names line at 23 px, at most 3 lines. It names the
    shorter side (ties go to "Off:"). With all 25 on, it reads "Every fighter".
- **Hint box**, depending on the lobby:
  - "1 seat open. You'd be P{n}.";
  - "Full. It shows because Show full is on.";
  - "In a game. Join, and you're seated when they're back on the CSS.";
  - "This lobby closed."
- **Legend:** A Join (only when it can be joined), and B Back.

## Steps

### D1 · Service and client data (medium)
1. `checks.go`: the two mask fields and a `mask(v, bits, named, what)` helper.
2. `lobby.go` `list`: `players`, from `l.Host`, `l.Members` (kept sorted by
   port) and `l.Extra`.
3. `xeleenet.c`:
   - `parse_settings` reads the masks, falling back from the list id;
   - `parse_listing` reads `players`;
   - **the browse reply buffer goes to 64 KB.**
4. `xeleenet.c`'s host/join body and heartbeat send `aurora_xr_active()`;
   `parse_members` reads `xr`.
5. `settings_json` appends `,"fighter_mask":%u,"stage_mask":%u`, last.

Test: test_service.py (below), and net_test_xn_menu.py (3 players) still
passes.

### D2 · LAN data (medium)
1. `host_announce`: cut `s` before the masks, and add `fm`, `sm`, `g` and `p`.
2. Log the sizes once.
3. `pc_xn_playing()` for `host_announce`.
4. A guest's XR goes in `LJoin.reserved[0]` bit 0, and from there into
   `PcXnMember.xr`.
5. `browse_tick` parses `fm`, `sm`, `g` and `p`, and fills `x->address` from
   the record's ip and port.

Test: `net_test_xn_menu.py --net lan` passes. The host logs `s` at 240 bytes
or less. A guest's Details log names the same players and masks the host set.

### D3 · The Details page, text cells (medium-large)
1. `Page::Details`: the enum, title and build. `set_board` sets the page's
   class from the page's name, so the class table can't drift again (it did
   when Fighters landed; fixed there).
2. A B1 row's `act` keeps the row (`details`, `detailsCode`) and goes to
   Details. The row is enabled even when full.
3. One item, `dtop`, enabled when the lobby isn't full and is still listed. Its
   `act` is today's row join. Pointer users click it.
4. `poll_browse` and the 10 s refresh also run on Details. A new set of rows
   replaces `details` by code. A lobby that has gone stays shown, with the
   "closed" hint and no A.
5. `back()` from Details goes to `go(Page::Lobbies, "lb<k>")`, with no
   `browse()` (no "Looking…" flash). `to_lobbies` gains the same focus.
6. The legend, the hint, the text (`xeleenet.details.*`) and the `#details`
   styles.
7. The log: `xeleenet pages: details CODE taken/seats players=… fighters=%08x
   stages=%08x`.
8. Previews: `details` (XeleeNet, 3/4, Low tiers, Competitive) and
   `lan-details` (full, in a game, Custom stages), added to xn_preview.py.
9. net_test_xn_menu.py presses A twice wherever a guest joins off the list.

D3 can be tested with previews alone, before D1 and D2.

### D4 · Row summaries (small)
`fighters_summary` and `stages_summary`, the measured drop, and their text. A
preview row with a long host name shows the stage part dropping.

### D5 · Portraits (medium, the uncertain part)
1. `mnCharSel_PcPortraits()` in mncharsel.c:
   - loads MnSlChr and builds the menu model, as `cssSlippiTitle` does;
   - for each entry, takes the largest C8 texture under `joint_id_vs` and
     calls `pc_xn_fighter_thumb(entry, …)`;
   - frees it all.
2. It runs once per launch, on the first frame after the first Details.
3. The C8 decode is shared with the stage select. Portraits are served as
   `xnfighter://<entry>`.
4. A cell shows the image once it's decoded, and the name until then.
5. Fallbacks, in order:
   - decoders for other formats (I8, IA8, RGB5A3);
   - harvesting in the CSS (`xnCss_Thumbs`), kept for the launch;
   - text cells are always the floor.
6. Optional: the Fighters picker uses the same cells.

Test: the `details` preview shows portraits on a cold launch. Memory is logged
before and after.

### D6 · Scenario and the headset (small)
`net_test_xn_menu.py --scenario details`, on the service and with `--net lan`:
1. The host has 2 seats, Low tiers and Final Dest. only.
2. Guest 1's Details log matches the host's masks and shows P1 as host. A
   joins.
3. Guest 2 turns on Show full and opens the full lobby. A logs no `joining`,
   and B goes back to the same row.

Then a Quest pass: read Details on the flat panel.

## Tests

- **tools/xeleenet/test_service.py:**
  - rows carry `players` in port order, the host first, with extra seats
    marked, and no `key` or `candidates`;
  - the masks are echoed back;
  - zero, out-of-range, Zelda≠Sheik and mismatched named lists are refused;
  - missing masks fall back to the named list;
  - the Go High constant matches xeleenet_fighters.h's;
  - 50 lobbies of 4 players fit under 64 KB.
- **tools/net_test_xn_menu.py:** the default run, `--net lan`, the existing
  scenarios (with the double A), and D6.
- **tools/xn_preview.py:** `lobbies`, `details`, `lan-details` and
  `lan-lobbies`.

## Risks

- **Public lists now show seated players' names and tags** (never keys or
  addresses). Private lobbies aren't listed.
- **mDNS:** 15 of 16 TXT keys are used. Guests' names now ride `p`, so a
  non-printable name drops the host's whole record. The same risk already
  exists for `name` and `host`.
- **MnSlChr in the menu:** a heap spike while the model is built, freed at
  once. Measure it on the Quest.
- **The portrait joint (`joint_id_vs`) isn't verified.** F3's dimming relies
  on it too, and D5's log settles it.
- **XR:** B1's XR badge goes from never shown to real. A guest's XR is
  captured at join only.

## Open questions

1. Should the place line show the region as a code ("US"), as B1 does, or as a
   name?
2. Is "Second controller" the right label for a multi-pad seat? The CSS calls
   it "RHYTHM 2".
3. Should headings and HOST/XR tags keep their small caps (15 px)? Only lines
   of words are held at 23 px.
4. Should B1's second line (17 px today) also rise to 12 units?
5. Should Details get a visible JOIN button for pointer users, or is clicking
   the top block enough?
