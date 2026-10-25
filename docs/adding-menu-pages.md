# Adding menu rows and pages to Melee's menus

How to add a PC-only page to Options (a sub-screen like Sound), or a
PC-only row to another of Melee's menus, so it looks and moves like the
stock ones. First written 2026-10-24 for Options > Fight Controls, the
first PC page; rewritten 2026-10-25 when the Options pages moved onto one
registry (`src/melee/mn/mnpage.c`) and one screen interface
(`src/pc/menu_screen.h`). Graphics, Audio, Gameplay and Controls are the
worked examples.

## Adding an Options page

A settings page is its rows and one line in the registry. Gameplay is the
example (`src/pc/gameplay_menu.cpp`):

1. **The rows**, in `src/pc/<name>_menu.cpp`: a function giving
   `std::vector<pc::settings::Row>`, a `pc::settings::Page` over it, and its
   screen:

   ```cpp
   pc::settings::Page s_page("options-page.rml", rows);
   } // namespace
   PC_SETTINGS_SCREEN(pc_gameplay_screen, s_page);
   ```

   The rows' settings usually come from a struct in `src/pc/settings.h`,
   read from and saved to the launcher's preferences in `launcher.cpp`
   (`pc_settings_gameplay`, `pc_settings_set_gameplay`), so the launcher
   and the page stay one setting.
2. **Declare the screen** in `src/pc/menu_screen.h`
   (`extern const PcMenuScreen pc_gameplay_screen;`).
3. **Register it** in `mnPage_pages` (`mnpage.c`):

   ```c
   { SEL_SETTINGS_ERASE, "Gameplay", "Set how fights play, and your saved progress.",
     { 1550, 1599, 1570 }, &pc_gameplay_screen },
   ```

   That's the Options row it takes, its name (the row's label and the
   tab's title), its description (the row's and the page's first hint), the
   preview pane's frames while the row is lit, and the screen.
4. **The build:** add the `.cpp` to `PC_SOURCES` in `CMakeLists.txt`.

Everything else follows from the registry:
- **Options shows only rows with a page** (`mn_80229938` locks the rest, so
  they're hidden and skipped).
- **The row's label, description and preview** come from the entry
  (`mnOnline_Label`, `mnOnline_Description`, `mnPage_Previews`).
- **A opens the page** (the Options think, `mnPage_Open`), and B goes back to
  its row.
- **The row count** reaches the last page's row (`mnPage_RowCount`). Options
  opens on the first page's row (`mnPage_FirstRow`).
- **Every page is menu kind `MENU_KIND_SETTINGS_PAGE`,** so Melee's tables
  indexed by kind (the panel's frames, `MenuKindData`, the light colour) have
  one entry for all of them. `mnPage_open` says which page it is.
- **Each frame,** `vi.c` calls `mnPage_Update`, which updates every screen.

### Which row

A page takes one of the Options rows, and the rows' order is the menu's:
- `SEL_SETTINGS_*` 0 to 5 are the stock rows; Graphics, Audio and Gameplay
  use Sound's, Screen Display's and Erase Data's.
- 6 (Place Stage's, now unused) and 7 (Controls) are PC rows.
- Free today: 0 (Rumble's), 3, 4 (Language's) and 6.

A ninth row means growing `SettingsMenuSelection`, the preview table
(`mn_803EB4F8`) and the description indices (`mn_803EB684`). The panel has
10 row joints (`mn_803EAE68`), and `mn_pcLabelPart` holds 8 labels. Five or
six rows is about what reads well.

### The rows

`src/pc/settings_page.hpp` has the full list. A row is:
- a **bar**, with steps; left and right move it, and a click sets a step;
- a **switch**, Off and On;
- a **choice**, its values cycled by left, right and A;
- an **action**, which A runs.

A row's `group` puts a heading above it. Each kind has its own optional
fields:

| Field | What it's for |
|---|---|
| `available` | dims the row and makes it inert (Place stage without a headset) |
| `note` | small text beside the value ("SET BY ENVIRONMENT") |
| `hint_now` | replaces the hint while it returns one (Lighting's camera prompts) |
| `tick` | runs each frame while shown, and redraws on a change (Lighting waiting on the permission answer) |
| `pending` | the row applies at launch, and differs from what the game started with: "AFTER RESTART" beside it, and B asks Restart now or Later (`pc_restart`) |

The last row is the bottom button (Restore defaults).

An action can **leave the page** with `page().go(PC_SCREEN_GO_*)`:
- `GO_ERASE_DATA` opens Melee's Erase Data page; its B comes back
  (`mnPage_Return`).
- `GO_PLACE_STAGE` runs the placement scene; the menu scene comes back on the
  page (`mnPage_ReturnRow`, `MENU_KIND_SETTINGS_PAGE` in its enter data).
- `GO_CALIBRATE` runs that scene's room light calibration alone
  (Graphics' Calibrate room light); it comes back the same way.
- `GO_BACK` returns to Options.

The page reopens on the row that left. A new destination needs:
- a `PC_SCREEN_GO_*`;
- a `case` in `mnPage_Think`;
- a way back.

### A screen of its own

A layout the rows can't make implements `PcMenuScreen` directly, as Controls
does (`src/pc/fight_menu.cpp`, `pc_controls_screen`):
- `show`, `presence`, `input` and `hint`, as described under "Native widgets
  or an RmlUi overlay" below;
- `update` each frame;
- `leaving` (or NULL).

It registers the same way.

## How a stock menu screen is built

Three pieces are on screen at once, and they belong to different code:

| What you see | Where it lives |
|---|---|
| Grid backdrop, swirl, lighting | `MenMainBack_Top` (`mn_80229B2C`), always running |
| Gold frame, its tabs ("Options / Sound"), the inner outlines, the silver hint box | The panel, `MenMainPanel_Top` (`mn_80229DC0`, think `fn_80229BF4`) |
| The page's own controls (rows, sliders, pictures) | The page's model, e.g. `MenMainConSo_Top` for Sound, and its GObjs |

- **The panel is one model for every menu.** Joint `0x29` (41) plays a frame range per menu kind from `mn_803EAE8C[kind]`: `[0]` going in, `[1]` coming out, `[2]` the idle loop.
- **Those frames carry the menu's look.** The tab's title texture, and the inner outlines with their labels (Sound's CHANNEL and VOLUME), come with the range. A new kind that borrows a range gets that screen's look, title included.
- **The panel's title pieces** sit at y 9.55, z 17.2 in panel space:
  - joint 84 is the menu's title ("Options");
  - joint 85 is the page's sub-title ("Sound", "Erase Data"), its own mesh;
  - joint 43 is the frame and tab shape;
  - joint 42's subtree is the frame, tabs and hint box border, but not the titles.
- **The menu state** is `mn_804A04F0` (`MenuFlow`): `cur_menu`, `prev_menu`, `hovered_selection`, `entering_menu`. The panel's think compares `cur_menu` with what it last showed, and plays the in or out range on its own.

## Adding a row to another menu

Options takes its PC rows from the registry. Other menus get a row by hand.
The main menu's Online Mode (`SEL_MAIN_ONLINE`) and the Online Mode menus
(`mnonline.c`) are the worked examples, and the steps below are what the
registry does for Options.

1. **A selection id** in `src/melee/mn/forward.h`, under `#ifdef TARGET_PC` (`SEL_MAIN_ONLINE = 2`, with the main menu's later rows moved down one).
2. **`mnmain.c`, the menu's tables**, each one entry longer under `#ifdef TARGET_PC`:
   - the preview animation array (`mn_803EB3FC` for the main menu): borrow a stock row's frames;
   - the description SIS indices: any stock id; the literal text replaces it.
3. **The row count:** where the menu is built (near `mn_803EB6B0[...].selection_count`), set it to include the new rows.
   - A row that is sometimes unavailable is **locked** in `mn_80229938`, not left out of the count. Locked rows are hidden and skipped, as stock `SEL_SETTINGS_3` is. That keeps later rows' ids fixed; Options hides every row without a page this way.
4. **Text:** `mnOnline_Label` and `mnOnline_Description` (`mnonline.c`) return literal strings for PC rows.
   - `mn_8022B3A0` hides the row's label texture, and `mn_UpdatePcLabels` draws SIS text over the slot, leaning as the joint does.
   - The description goes through the literal path in `mnmain.c`.
5. **Dispatch:** the menu's think (`mn_8022D104` for Options) gets a `case` for the id.
6. **Room:** the panel has 10 row joints (`mn_803EAE68`) and `mn_pcLabelPart` holds 8 labels.

## Inside mnpage.c

How a page runs, for changing the host rather than adding a page. It follows
the stock Sound page (`mnsound.c`).

1. **Entering** (`mnPage_Open`, after `mnSound_8024A09C`):
   - it sets `mn_804D6BC8.cooldown = 5`, `prev_menu = cur_menu`, `cur_menu = MENU_KIND_SETTINGS_PAGE`, `hovered_selection = 0`;
   - it creates the page's GObj (class `HSD_GOBJ_CLASS_ITEM`, plink 7) and a think GObj (`GObj_Create(0, 1, 0x80)`, `flags_3 = HSD_GObj_804D783C`);
   - the Options think calls it after `sfxForward()`, then `HSD_GObjFree(gp)`. The Options rows leave by themselves.
2. **The panel's frames:** `MENU_KIND_SETTINGS_PAGE` borrows Erase Data's (4200-4399) in `mn_803EAE8C`, whose inside is plain, because Erase Data's rows are its own model. Its `MenuKindData` is `{ NULL, 0, NULL, 0x01, NULL }`, for a page that runs its own GObjs, and it's in the Options colour family (`return 3;`).
3. **Waiting for the panel:** stock pages keep their content hidden for 20 frames while the old rows leave (Sound's `cursor = 0x14`).
4. **Input:** `Menu_GetAllInputs()` in the think, after `if (mn_804D6BC8.cooldown) { Menu_DecrementAnimTimer(); return; }`.
   - It merges the keyboard, Touch, SDL gamepads and laser clicks, with the menus' repeat.
   - The think hands the screen `PC_SCREEN_*`; sounds are `sfxBack()`, `sfxForward()`, `sfxMove()`.
5. **Going back:** `sfxBack(); mn_804A04F0.entering_menu = 0; mn_80229894(MENU_KIND_SETTINGS, <row>, 3);`. That rebuilds Options in its fly-in state with `<row>` lit, frees the think GObj, and starts the Options think.
   - The page's own GObj frees itself on its next frame, once `cur_menu` isn't `MENU_KIND_SETTINGS_PAGE` (`mnPage_Frame`). It also frees its SIS texts there.
   - **A scene ending with a page open** (placing the stage) drops its GObjs without their user data's frees, so `mnMain_Scene_OnEnter` calls `mnPage_SceneForget` first. Without it the next `mnPage_Open` frees a stale GObj and crashes.
6. **The tab's title:** the borrowed range shows Erase Data's title. Each frame the panel's think calls `mnPage_PanelTitle`:
   - it hides joint 85 and draws SIS text where it is (`lb_8000B1CC` for the joint's position), so the text rides the tab's slide;
   - "ours" means `cur_menu` is a PC page's kind, or leaving one (`state == 1` with `prev_menu`), so the title stays up through the slide out;
   - after that it puts joint 85 back (clears `JOBJ_HIDDEN`), since the next menu needs it.
   - **Matching the stock titles:** they are slanted grey textures. The SIS font is upright, so the text's `render_callback` (`mnPage_TitleItalic`) loads a position matrix: the joint's world rotation with an x-by-y shear (k 0.2), about the joint's origin. That's the same trick `mn_PcLabelLean` uses for the row labels.
   - Colour (0x9C, 0x9C, 0x9C). Size 0.031 x 0.038, a little condensed, so 14 letters fit the tab that holds "Erase Data". At 0.05 it runs past the tab.
7. **The hint box:** literal SIS text (`mnPage_HintText`), placed as the PC descriptions are: `HSD_SisLib_803A6754(0, mn_804D6BB4)`, pos (-9, 9.3, 17), kerning 1, size 0.022.
   - Over 40 letters, it breaks at the space nearest the middle into two lines at 0.0175, centred where the one line sits.
   - The Options descriptions and the Slippi pages use it too. Stock pages use `Menu_InitCenterText(menu, sis_id)` for SIS strings.
8. **The build:** `cmake/game_sources.manifest` lists every game source. A new `src/melee/...` file must be added there in sorted order, or configure fails with "game source manifest drift". `-DMELEE_UPDATE_SOURCE_MANIFEST=ON` rewrites it.

## Finding joints in a model

A temporary probe, never committed. In the model's think, under `TARGET_PC`:
- log every joint once: index, translation, dobj, child, next, flags, via `lb_80011E24(root, &j, i, -1)` for i up to 200;
- hide the indices in an env var with `HSD_JObjSetFlagsAll(j, JOBJ_HIDDEN)`.

Hide subtrees first, then single joints, and screenshot each run. Joints are numbered depth first, so a sibling of a big subtree can sit at index 80+. Stock page thinks clear `JOBJ_HIDDEN` on their own models after their wait, so hide in the think each frame, not at load.

## Checking it against a stock page

Drive the desktop build to the page and capture a frame sequence:
- **Driving:** `MELEE_BOOT_SCENE=menu MELEE_NO_ATTRACT=1 MELEE_KEY_FIFO=<fifo>`. FIFO lines are "Down 120", "X 120" (A), "Z 120" (B).
- **Capturing:** `xdotool search --name '^Xelee$'` for the window, then `import -window <id>` in a loop, with `XAUTHORITY=/run/user/1000/xauth_*`.

Put the same capture of the stock page beside it (Options > Sound, for an Options page) and compare going in and coming back. The FIFO keys reach the game's pads only, never RmlUi.

## Native widgets or an RmlUi overlay

Both are fine. Pick per page:
- **Native** (borrowed models, SIS text) for what Melee already has a widget for: rows of choices, a value changed with left and right (the Rules rows), lists (Erase Data). These get Melee's font, cursor, sounds and fly-ins for free. Most launcher settings are this kind.
- **An RmlUi overlay** on a native page for layouts Melee has no widget for. Controls is the example: a device picture, leader lines, two columns of boxes and a pop-up list. RmlUi draws at the window's resolution, sharper than SIS text at the game's internal resolution, and its look is a stylesheet edit.

**An overlay must stay part of the page.** In `src/pc/fight_menu.cpp` with `mnpage.c`:
- **No backdrop, title or footer of its own.** The body is transparent; the page's frame, tab and hint box are Melee's. The menu keeps running underneath: no `VIWaitForRetrace` hold, no `PADBlockInput`.
- **Inside the frame.** The layout is scaled into the frame's inside, measured on screen as fractions of the picture (Erase Data's frames: 12.3-87.8% across, 17.0-84.2% down), and never over the hint box.
- **The page's input.** The page's think reads `Menu_GetAllInputs()` and hands it over (the screen's `input`). The overlay reads no devices itself, or a press would count twice. A B with nothing to close returns false, and the page goes back.
- **Lasers and the mouse** move RmlUi's pointer (`aurora_xr_screen_pointer` into `ProcessMouseMove`) for hover. A laser's click already reaches the page as the menus' A (`xr_pointer.c`), so the overlay ignores RmlUi's own click while a laser points.
- **The hint box stays native.** The overlay says what the cursor is on (the screen's `hint`), and the page writes it as SIS text when it changes.
- **Frame-synced animation.** The page's GObj counts frames and sets the overlay's presence each frame (the screen's `presence`, 0 to 1), never with CSS timers:
  - it is hidden for the page's 20-frame wait;
  - it fades and rises in over 10 frames;
  - it fades out over 8 while the parent's rows come back. The page's GObj outlives `cur_menu` changing for those frames, so it frees its hint at once, since the parent writes its own, and goes after the fade.
  - Entering again mid-fade ends the old fade at once.
  - Input waits for presence 1.
  - Small pop-ups (the list) animate per frame in the overlay's update.
