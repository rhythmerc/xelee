#ifndef MELEE_MN_MNFIGHT_H
#define MELEE_MN_MNFIGHT_H

#include "forward.h"

#include <sysdolphin/baselib/forward.h>

/* PC only: the Options pages drawn in RmlUi, Controls (MENU_KIND_SETTINGS_FIGHT,
 * src/pc/fight_menu.h), Audio (MENU_KIND_SETTINGS_AUDIO, src/pc/audio_menu.h)
 * and Graphics (MENU_KIND_SETTINGS_GRAPHICS, src/pc/graphics_menu.h). Options
 * screens like Sound: the Options rows leave,
 * the frame's tab says the page's name (Erase Data's panel animation, its
 * title swapped for SIS text), and B goes back to the page's row. The hint
 * box says what the cursor is on. */

/* Entered from the Options menu's think, as mnSound_8024A09C is. */
void mnFight_Open(MenuKind kind);
/* A PC page's hint-box text (in the menu's text canvas): one line, or two
 * at a smaller size when it's long. Freed as any HSD_Text. */
HSD_Text* mnFight_HintText(const char* text);

/* Each frame, from the panel's think (fn_80229BF4): the tab's title. The
 * panel is `panel`, showing `cur` (and leaving `prev` when `leaving`). */
void mnFight_PanelTitle(HSD_JObj* panel, MenuKind cur, MenuKind prev, bool leaving);
/* A scene that frees every SIS text (HSD_SisLib_803A5E70) and its panel to
 * build another screen calls this: the title went with them. */
void mnFight_PanelTitleForget(void);
/* The menu scene starting: a page left open when the last one ended
 * (Gameplay's Place stage) went with its GObjs, and its screen is hidden. */
void mnFight_SceneForget(void);

#endif
