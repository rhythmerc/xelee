/* SPDX-License-Identifier: GPL-3.0-or-later */
/* An Options page's screen: what's drawn in RmlUi inside a native page
 * (src/melee/mn/mnpage.c, which owns the frame, the tab, the hint box and
 * the input, and lists the pages). Controls (fight_menu.cpp) is a screen of
 * its own; Graphics, Audio and Gameplay (graphics_menu.cpp and the rest)
 * are rows on the settings engine (settings_page.hpp), which makes their
 * screens. docs/adding-menu-pages.md has how to add one.
 */
#ifndef PC_MENU_SCREEN_H
#define PC_MENU_SCREEN_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The menus' input, as the page hands it over. */
enum {
    PC_SCREEN_UP,
    PC_SCREEN_DOWN,
    PC_SCREEN_LEFT,
    PC_SCREEN_RIGHT,
    PC_SCREEN_CONFIRM,
    PC_SCREEN_BACK,
};

/* Where a screen asks its page to go (its `leaving`). */
enum {
    PC_SCREEN_STAY,
    PC_SCREEN_GO_BACK,        /* to the Options list */
    PC_SCREEN_GO_ERASE_DATA,  /* Melee's Erase Data page; B there comes back */
    PC_SCREEN_GO_PLACE_STAGE, /* the stage-placement scene; it comes back */
};

typedef struct PcMenuScreen {
    void (*show)(bool show);
    /* The page's fade, each frame while shown: 0 gone, 1 shown. Input waits
     * for 1. */
    void (*presence)(float presence);
    /* PC_SCREEN_*; false when there was nothing to do (B with nothing to
     * close: the page goes back). */
    bool (*input)(int what);
    /* What the hint box says for the cursor; NULL when not shown. */
    const char* (*hint)(void);
    /* Once a frame, shown or not: placement, the lasers. */
    void (*update)(void);
    /* PC_SCREEN_GO_* once when it asks, else PC_SCREEN_STAY; NULL if it
     * never asks. */
    int (*leaving)(void);
} PcMenuScreen;

extern const PcMenuScreen pc_controls_screen;
extern const PcMenuScreen pc_graphics_screen;
extern const PcMenuScreen pc_audio_screen;
extern const PcMenuScreen pc_gameplay_screen;

#ifdef __cplusplus
}
#endif

#endif
