/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Gameplay screen (Options > Gameplay, in the stock Erase Data row's
 * place; the settings are settings.h's, the launcher's own). The fights'
 * rules: UCF, Slippi's fixes, the C-stick in 1P, hazardless stages and (on
 * a screen) the free camera; on the Quest the stage's placing, Before each
 * fight and Place stage now; then progress: Unlock everything, and Clear
 * game progress, which opens Melee's Erase Data page. Each applies as the
 * game runs. Rows on the Options pages' engine (settings_page.hpp), in the
 * native page of src/melee/mn/mnfight.c.
 */
#ifndef PC_GAMEPLAY_MENU_H
#define PC_GAMEPLAY_MENU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* As the Controls screen's (fight_menu.h): input is PC_FIGHT_MENU_*. */
void pc_gameplay_menu_show(bool show);
void pc_gameplay_menu_presence(float presence);
bool pc_gameplay_menu_input(int what);
const char* pc_gameplay_menu_hint(void);
/* Once a frame (vi.c): placement, the lasers. */
void pc_gameplay_menu_update(void);
/* Where the page asks to go, PC_OPTIONS_* (fight_menu.h): Erase Data, or
 * the stage's placing. */
int pc_gameplay_menu_leaving(void);

#ifdef __cplusplus
}
#endif

#endif
