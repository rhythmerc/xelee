/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Audio screen (Options > Audio, in the stock Sound row's place; the
 * settings are settings.h's, the launcher's own).
 *
 * Two groups of rows in Melee's line work: Volume (master, music, effects as
 * bars of twenty steps, and mute) and Sound (reverb, spatial audio, the
 * expanded stage sounds), then Restore defaults. Drawn in RmlUi into the
 * native Options page's frame (src/melee/mn/mnfight.c, which owns the frame,
 * the tab, the hint box and the input) as the Controls screen is. Up and down
 * move between rows, left and right change the value, A flips a switch;
 * every change is live and saved. Lasers and the mouse point, and a click on
 * a bar's step sets it there.
 */
#ifndef PC_AUDIO_MENU_H
#define PC_AUDIO_MENU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* As the Controls screen's (fight_menu.h): input is PC_FIGHT_MENU_*. */
void pc_audio_menu_show(bool show);
void pc_audio_menu_presence(float presence);
bool pc_audio_menu_input(int what);
const char* pc_audio_menu_hint(void);
/* Once a frame (vi.c): placement, the lasers. */
void pc_audio_menu_update(void);

#ifdef __cplusplus
}
#endif

#endif
