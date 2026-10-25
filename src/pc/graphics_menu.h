/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Graphics screen (Options > Graphics, in the stock Sound row's
 * place; the settings are settings.h's, the launcher's own). On the Quest:
 * the eyes' resolution, SpaceWarp, foveation, custom textures, the stage's
 * shadow and the lighting; on a screen: the window, sync, resolution,
 * aspect, HUD, FPS counter, anti-aliasing, post-processing, textures and
 * the backend. What only applies at launch (SpaceWarp, foveation,
 * anti-aliasing, the backend) says so until then, and leaving the page
 * offers to restart (settings_page.hpp).
 * Rows on the Options pages' engine (settings_page.hpp), in the native page
 * of src/melee/mn/mnfight.c.
 */
#ifndef PC_GRAPHICS_MENU_H
#define PC_GRAPHICS_MENU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* As the Controls screen's (fight_menu.h): input is PC_FIGHT_MENU_*. */
void pc_graphics_menu_show(bool show);
void pc_graphics_menu_presence(float presence);
bool pc_graphics_menu_input(int what);
const char* pc_graphics_menu_hint(void);
/* The restart question's Later was picked: the page goes back (once). */
bool pc_graphics_menu_leaving(void);
/* Once a frame (vi.c). */
void pc_graphics_menu_update(void);

#ifdef __cplusplus
}
#endif

#endif
