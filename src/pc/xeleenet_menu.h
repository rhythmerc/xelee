/* SPDX-License-Identifier: GPL-3.0-or-later */
/* XeleeNet's pages (docs/xeleenet-plan.md; the wireframe's H1, H2, B1,
 * J1-J4), drawn in RmlUi into the red Online frame by
 * src/melee/mn/mnxeleenet.c, as the Slippi pages are:
 *   - Host (H1): the lobby's settings in two columns under a preset bar, with
 *     the stage list (H2) and More rules as pages of their own. START or OPEN
 *     LOBBY opens it, and the host goes on to the CSS, which is the lobby.
 *   - Join: the public lobbies (B1); Y types a code instead (J1). Then the
 *     joining card (J2), Find - Connect - Lobby with a line per player, a
 *     game in progress (J4), or why it failed (J3).
 * Names are typed on Melee's keyboard (the player's, the lobby's): the pages
 * ask for it and the page host opens it.
 *
 * In the CSS the lobby's banner (C1) sits over Melee's rules bar: its name,
 * its rules and its code. */
#ifndef PC_XELEENET_MENU_H
#define PC_XELEENET_MENU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PC_XN_MENU_HOST,
    PC_XN_MENU_JOIN,
    PC_XN_MENU_NAME,
};

/* The menus' input: PC_SCREEN_* (menu_screen.h), and these. */
#define PC_XN_IN_START 100
#define PC_XN_IN_X 101
#define PC_XN_IN_Y 102

/* What a visit ended with, once (pc_xn_menu_result). */
enum {
    PC_XN_MENU_STAY,
    PC_XN_MENU_LEAVE,  /* back to the XeleeNet menu */
    PC_XN_MENU_HOSTED, /* the lobby is open: the host goes to the CSS */
    PC_XN_MENU_JOINED, /* the session is up: on to the CSS on the handshake's frame */
};

void pc_xn_menu_begin(int what);
void pc_xn_menu_end(void);
void pc_xn_menu_show(bool show);
void pc_xn_menu_presence(float presence);
void pc_xn_menu_input(int what);
/* True once, with what to put in it, when a page wants the keyboard. */
bool pc_xn_menu_keyboard(char initial[32], int* max_len, const char** prompt);
void pc_xn_menu_keyboard_done(bool confirmed, const char* text);
int pc_xn_menu_result(void);

/* What Host set (H1), for the game to put into its rules before the CSS:
 * the RULES handshake then hands them to every player. */
typedef struct PcXnRules {
    int seats;          /* 2-4 */
    bool teams;
    bool stock;         /* stock, else time */
    int stocks;         /* 1-99 */
    int minutes;        /* 0: no limit */
    int items;          /* -1 off, 0-4 very low to very high */
    bool friendly_fire;
    int handicap;       /* 0 off, 1 auto, 2 on */
    int damage_ratio;   /* tenths: 5-20 */
    bool pause;
    unsigned stage_mask; /* GamePrefs.stage_mask: bit = Random Stage Switch id */
    bool frozen_stadium;
} PcXnRules;
void pc_xn_menu_rules(PcXnRules* out);

/* The tab's sub-title and the hint box's text. */
const char* pc_xn_menu_title(void);
const char* pc_xn_menu_hint(void);
/* Once a frame (vi.c): the punch's progress, placement, the lasers. */
void pc_xn_menu_update(void);

/* C1: the lobby's banner over the CSS's rules bar, while in a XeleeNet CSS. */
void pc_xn_banner_show(bool show);

#ifdef __cplusplus
}
#endif

#endif
