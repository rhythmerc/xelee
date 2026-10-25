#ifndef MELEE_MN_MNPAGE_H
#define MELEE_MN_MNPAGE_H

#include "forward.h"

#include <sysdolphin/baselib/forward.h>

/* PC only: the Options pages, each a screen drawn in RmlUi
 * (src/pc/menu_screen.h) inside a native page, all of menu kind
 * MENU_KIND_SETTINGS_PAGE. They are Options screens like Sound: the Options
 * rows leave, the frame's tab says the page's name (Erase Data's panel
 * animation, its title swapped for SIS text), the hint box says what the
 * cursor is on, and B goes back to the page's row. mnpage.c's list of pages
 * gives each one's Options row, name, description, preview and screen; the
 * Options menu takes its PC rows from it. docs/adding-menu-pages.md. */

/* The Options menu's rows (mnmain.c): whether `row` is a page's (the rest
 * are hidden on PC); its label and description; the first row shown, how
 * many rows there are, and each page's preview frames put in `rows`, the
 * menu's preview table of `count` entries. */
bool mnPage_HasRow(int row);
const char* mnPage_Label(int row);
const char* mnPage_Description(int row);
int mnPage_FirstRow(void);
int mnPage_RowCount(void);
void mnPage_Previews(AnimLoopSettings* rows, int count);

/* Opens the page in Options row `row`, from the Options menu's think (as
 * mnSound_8024A09C is); false when the row has none. */
bool mnPage_Open(int row);
/* Back from where a page sent the player (Erase Data's B): opens that page
 * again; false when none did. */
bool mnPage_Return(void);
/* Back from placing the stage, in a new menu scene: the Options row of the
 * page that sent the player, forgotten as it's given (the menu scene opens
 * it for MENU_KIND_SETTINGS_PAGE in its enter data). */
int mnPage_ReturnRow(void);
/* Once a frame (vi.c): every page's screen. */
void mnPage_Update(void);

/* A PC page's hint-box text (in the menu's text canvas): one line, or two
 * at a smaller size when it's long. Freed as any HSD_Text. */
HSD_Text* mnPage_HintText(const char* text);

/* Each frame, from the panel's think (fn_80229BF4): the tab's title. The
 * panel is `panel`, showing `cur` (and leaving `prev` when `leaving`). */
void mnPage_PanelTitle(HSD_JObj* panel, MenuKind cur, MenuKind prev, bool leaving);
/* A scene that frees every SIS text (HSD_SisLib_803A5E70) and its panel to
 * build another screen calls this: the title went with them. */
void mnPage_PanelTitleForget(void);
/* The menu scene starting: a page left open when the last one ended
 * (Gameplay's Place stage) went with its GObjs, and its screen is hidden. */
void mnPage_SceneForget(void);

#endif
