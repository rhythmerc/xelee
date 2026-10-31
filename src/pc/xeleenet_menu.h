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
#include <stdint.h>

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
/* The CSS's or the stage select's overlay is up: the game's frame should be
 * the size it's shown at (vi.c), not the render scale's. */
bool pc_xn_overlay_full_size(void);

/* C1: the lobby's banner over the CSS's rules bar, while in a XeleeNet CSS. */
void pc_xn_banner_show(bool show);

/* XeleeNet's CSS draws smaller about the screen's centre by this (its camera
 * pulled back, mncharsel.c), for the notice slot and the legend's room. */
#define PC_XN_CSS_ZOOM 0.84f
/* And the stage select, for the picks card's room under the grid (P5). */
#define PC_XN_SSS_ZOOM 0.88f

/* XeleeNet's CSS Ready (mncharsel.c), once a frame, for the overlay over it:
 * the READY count, the notice slot, the doors' tags and the legend. */
typedef struct PcXnCssState {
    uint8_t humans, ready, picked; /* port bits */
    uint8_t waiting;               /* the one everyone else waits on, or 0 */
    int countdown;                 /* frames to the stage select; -1: none */
    int countdown_total;
    int hold, hold_total; /* this player's B hold toward leaving (P4), in frames */
    bool hand_ok;         /* their hand on screen, as fractions of it */
    float hand_x, hand_y;
    int seats; /* the doors shown, the lobby's seats: those of ports 0 to seats-1, centred */
    /* This machine's seats (docs/lan-multipad-plan.md): its ports, which one
     * `hold` is, its first connected pad with no seat (-1 none), and its pads
     * waiting for one (bit = pad). */
    uint8_t local;
    int hold_port;
    int spare;
    uint8_t claimed;
} PcXnCssState;
void pc_xn_css_frame(const PcXnCssState* state);
/* For the CSS's doors (mncharsel.c): the lobby's seats (2-4, 4 when not
 * known), and each seated player's name ("NAME" of "NAME#TAG") in
 * names[port], with bit `port` of the result set; *local is our own port. */
int pc_xn_css_seats(void);
unsigned pc_xn_css_names(char names[4][16], int* local);
/* A player left the lobby (gmonlinemode.c): the notice slot says so for a few
 * seconds, through the CSS's reload. */
void pc_xn_css_left(const char* name);
/* START on a spare pad (physical pad `phys`) with no seat open: the notice
 * slot says the lobby is full, on this machine only. */
void pc_xn_css_seat_full(int phys);
/* A line for the next XeleeNet page (pc_xn_menu_begin), e.g. why the player
 * is back there. A lang key. */
void pc_xn_menu_note(const char* key);
/* A lobby that just closed under us: the list leaves it out for a while,
 * though the service may not have heard yet. */
void pc_xn_menu_hide_lobby(const char* code);

/* The stage select in a XeleeNet lobby, once a frame: the lobby's stage list
 * (RULES stage_mask), for the overlay's pill and legend (mnstagesel.c dims
 * the stages off it). Shows the overlay; pc_xn_sss_show(false) on the way
 * out. */
void pc_xn_sss_frame(uint32_t mask);
void pc_xn_sss_show(bool show);

/* The stage select's picks (P5 of docs/xeleenet-ready-plan.md), once a frame
 * from mnstagesel.c, for the picks card, the outlines on the grid and the
 * draw. Cells index the stage select's table, 30 being Random; ids are the
 * cells' Random Stage Switch ids, which name the stages (kStages). */
typedef struct PcXnSssState {
    uint8_t ports;          /* the seated players' port bits */
    int8_t pick[4];         /* each one's pick (a cell or 30), -1 still choosing */
    int8_t pick_id[4];      /* its stage's id, -1 for Random */
    bool outline_ok[4];     /* the pick's icon frame on screen: x, y, w, h as fractions */
    float outline[4][4];
    uint8_t outline_bevels[4]; /* which of its corners are cut: bit 0 top left, then clockwise */
    int timer, timer_total; /* frames left to pick, -1 before the first pick */
    int8_t hover, hover_id; /* the stage our cursor is on while we've no pick, else -1 */
    /* The draw, once the host's FINAL is in: */
    bool final;
    int8_t cell[4], cell_id[4]; /* what each card shows, Random drawn */
    uint8_t random, timed_out;  /* cards that were Random, and of those who ran out of time */
    int chosen;                 /* the port whose card won */
    int t, draw, rest;          /* frames into the draw (<0 before), its length, the rest after */
    int lit;                    /* the port whose card is lit */
} PcXnSssState;
void pc_xn_sss_picks(const PcXnSssState* state);
/* An icon's picture for the cards (GX C8 with its palette, as the stage
 * select draws it), decoded once and served to RmlUi as xnstage://<cell>. */
void pc_xn_sss_thumb(int cell, const void* c8, int w, int h, const void* tlut, int tlut_fmt, int entries);
bool pc_xn_sss_has_thumb(int cell);

#ifdef __cplusplus
}
#endif

#endif
