/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/xr_place.h"

#ifdef AURORA_ENABLE_OPENXR

#include "pc/net.h"
#include "pc/pc.h"
#include "pc/settings.h"
#include "pc/xr_calibrate.h"

#include <SDL3/SDL.h>
#include <aurora/xr.h>
#include <dolphin/pad.h>
#include <sysdolphin/baselib/controller.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PNG (the cards) and JPEG (the clips), private to this file:
 * thp_jpeg.cpp has its own JPEG-only copy. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "pc/stb_image.h"

/* The stage to offer next pass (-1: none) and the one held (-1: none). */
static int s_pending = -1;
static int s_holding = -1;
/* The how-to cards are showing. Y toggles them, and the choice carries to
 * every later hold until the app restarts. */
static bool s_cards = true;
/* Buttons down in the newest pad sample last pass, and the ones that went
 * down during the hold (a release only counts for a press seen here). */
static uint32_t s_prev;
static uint32_t s_down;
/* The placement dummy scene (gm/gmonlinemode.c, ONLINE_KIND_PLACE) is the
 * scene: its stage is held for placing like a fight's, but the simulation
 * keeps ticking (the two fighters idle, the pads never reach them) and A
 * adopts the placement for every stage (B resets it, as in a fight's hold). */
static bool s_dummy;
static int s_dummy_result; /* 0 running, 1 adopted, 2 calibrated only, 3 backed out */
static bool s_decided;     /* placed or skipped once this launch */
/* What the next placement scene does (pc_xr_place_plan), and this one:
 * the room light calibration first (running now), placing after it; lit
 * as Vanilla for now (placing before the room light is calibrated). */
static bool s_plan_calibrate, s_plan_place = true;
static char s_plan_next[40];
static bool s_calibrating, s_place = true, s_vanilla, s_cancelled;
static int s_move; /* the stick's or the D-pad's push last pass: -1 up, 1 down */
static bool s_every_fight = true; /* the launcher's "Before each fight" */
static int s_scene = -1;          /* the GameSceneKind running */

void pc_xr_place_set_hold(bool every_fight) {
    s_every_fight = every_fight;
}

void pc_xr_place_set_scene(int kind) {
    s_scene = kind;
}

static bool enabled(void) {
    const char* v = getenv("MELEE_XR_PLACE");
    return v == NULL || *v != '0';
}

void* pc_xr_load_file(const char* name, size_t* size) {
    char path[512];
    snprintf(path, sizeof path, "resources/xr/%s", name);
    void* data = SDL_LoadFile(path, size);
    if (data == NULL) {
        const char* base = SDL_GetBasePath();
        if (base != NULL) {
            snprintf(path, sizeof path, "%sresources/xr/%s", base, name);
            data = SDL_LoadFile(path, size);
        }
    }
    if (data == NULL) {
        pc_log_line("xr: placing picture %s not found", name);
    }
    return data;
}

unsigned char* pc_xr_load_rgba(const char* name, int* w, int* h) {
    size_t size = 0;
    void* data = pc_xr_load_file(name, &size);
    if (data == NULL) {
        return NULL;
    }
    int n = 0;
    unsigned char* rgba = stbi_load_from_memory(data, (int)size, w, h, &n, 4);
    SDL_free(data);
    if (rgba == NULL) {
        pc_log_line("xr: placing picture %s unreadable", name);
    }
    return rgba;
}

void pc_xr_free_rgba(unsigned char* rgba) {
    stbi_image_free(rgba);
}

static void load_image(int which, const char* name) {
    int w = 0, h = 0;
    unsigned char* rgba = pc_xr_load_rgba(name, &w, &h);
    if (rgba != NULL) {
        aurora_xr_set_placing_image(which, w, h, rgba);
        stbi_image_free(rgba);
    }
}

/* place-clips.txt: one clip a line, "set name frames cols frame_w frame_h
 * fps x y w h" (tools/xr_cards/make_cards.py), set being controllers or
 * hands, whose clips go in slots 0-2 and 3-5 in the order move, turn,
 * scale; its atlas is place-clip-<set>-<name>.jpg. */
static void load_clips(void) {
    size_t size = 0;
    char* text = pc_xr_load_file("place-clips.txt", &size);
    if (text == NULL) {
        return;
    }
    char* copy = malloc(size + 1);
    memcpy(copy, text, size);
    copy[size] = '\0';
    SDL_free(text);
    static const char* const names[3] = {"move", "turn", "scale"};
    for (char* line = strtok(copy, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        char set[16], name[32];
        int frames, cols, fw, fh, x, y, w, h;
        float fps;
        if (sscanf(line, "%15s %31s %d %d %d %d %f %d %d %d %d", set, name, &frames, &cols, &fw, &fh, &fps, &x, &y,
                   &w, &h) != 11) {
            continue;
        }
        int which = -1;
        for (int i = 0; i < 3; i++) {
            if (strcmp(name, names[i]) == 0) {
                which = (strcmp(set, "hands") == 0 ? 3 : 0) + i;
            }
        }
        if (which < 0) {
            continue;
        }
        char file[80];
        snprintf(file, sizeof file, "place-clip-%s-%s.jpg", set, name);
        int aw = 0, ah = 0;
        unsigned char* rgba = pc_xr_load_rgba(file, &aw, &ah);
        if (rgba != NULL) {
            aurora_xr_set_placing_clip(which, aw, ah, rgba, frames, cols, fw, fh, fps, x, y, w, h);
            stbi_image_free(rgba);
        }
    }
    free(copy);
}

/* The buttons held on any connected port in the newest queued pad sample. */
static uint32_t newest_buttons(void) {
    const PadLibData* p = &HSD_PadLibData;
    if (p->qcount == 0 || p->queue == NULL) {
        return s_prev;
    }
    const int newest = p->qwrite != 0 ? p->qwrite - 1 : p->qnum - 1;
    uint32_t buttons = 0;
    for (int port = 0; port < 4; port++) {
        const PADStatus* s = &p->queue[newest].stat[port];
        if (s->err == 0) {
            buttons |= s->button;
        }
    }
    return buttons;
}

static void end_hold(void) {
    aurora_xr_set_placing(0);
    s_holding = -1;
}

bool pc_xr_place_available(void) {
    return enabled() && aurora_xr_active();
}

bool pc_xr_place_decided(void) {
    return s_decided;
}

bool pc_xr_place_needed(void) {
    return pc_xr_place_available() && !s_decided && !aurora_xr_stage_adopted();
}

bool pc_xr_place_needed_online(void) {
    return pc_xr_place_available() && !s_decided;
}

void pc_xr_place_plan(bool calibrate, bool place, const char* next) {
    s_plan_calibrate = calibrate;
    s_plan_place = place;
    snprintf(s_plan_next, sizeof s_plan_next, "%s", next != NULL ? next : "");
}

bool pc_xr_place_take_cancelled(void) {
    const bool r = s_cancelled;
    s_cancelled = false;
    return r;
}

/* The roomtone setting, for lighting the placing as Vanilla. */
static bool roomtone_on(void) {
    PcGraphicsSettings g;
    pc_settings_graphics(&g);
    return g.xr_lighting == 1;
}

void pc_xr_place_dummy_start(void) {
    s_dummy = true;
    s_dummy_result = 0;
    s_pending = -1;
    s_holding = -1;
    s_calibrating = s_plan_calibrate;
    s_place = s_plan_place;
    s_cancelled = false;
    s_plan_calibrate = false;
    s_plan_place = true;
    s_prev = newest_buttons(); /* held coming in: not a press */
    s_down = 0;
    s_move = 0;
    if (s_calibrating) {
        pc_xr_calibrate_begin(s_plan_next);
    } else if (!pc_xr_calibrated() && roomtone_on()) {
        /* Place stage before the room light is calibrated: no fight starts
         * uncalibrated and the ghost is translucent, so it's lit as
         * Vanilla, with nothing said. */
        s_vanilla = true;
        aurora_xr_set_room_light_held(true);
    }
}

int pc_xr_place_dummy_result(void) {
    const int r = s_dummy_result;
    if (r != 0) {
        s_dummy = false;
        s_dummy_result = 0;
        if (r == 1) {
            s_decided = true;
        }
        s_cancelled = r == 3;
        if (s_calibrating) {
            pc_xr_calibrate_abort();
            s_calibrating = false;
        }
        if (s_vanilla) {
            s_vanilla = false;
            aurora_xr_set_room_light_held(false);
        }
        if (s_holding >= 0) {
            end_hold();
        }
        s_pending = -1; /* a stage the calibration kept waiting isn't held after the scene */
    }
    return r;
}

void pc_xr_place_stage_ready(int grkind) {
    if (s_holding < 0) {
        s_pending = grkind;
    }
}

bool pc_xr_place_holding(void) {
    return s_holding >= 0 || s_pending >= 0;
}

/* The stick's or the D-pad's push on any connected port in the newest
 * queued pad sample: -1 up, 1 down, 0 neither. */
static int newest_move(void) {
    const PadLibData* p = &HSD_PadLibData;
    if (p->qcount == 0 || p->queue == NULL) {
        return 0;
    }
    const int newest = p->qwrite != 0 ? p->qwrite - 1 : p->qnum - 1;
    for (int port = 0; port < 4; port++) {
        const PADStatus* s = &p->queue[newest].stat[port];
        if (s->err != 0) {
            continue;
        }
        if ((s->button & PAD_BUTTON_UP) || s->stickY > 48) {
            return -1;
        }
        if ((s->button & PAD_BUTTON_DOWN) || s->stickY < -48) {
            return 1;
        }
    }
    return 0;
}

/* The placement scene's room light calibration, each pass: its buttons
 * (a release counts for a press seen here), then on to placing (or past
 * it), or back out. */
static void calibrate_pass(void) {
    const uint32_t buttons = newest_buttons();
    const uint32_t released = s_prev & ~buttons;
    s_down |= buttons & ~s_prev;
    s_prev = buttons;
    const uint32_t done = released & s_down;
    s_down &= ~released;
    const int move = newest_move();
    const int pushed = move != s_move ? move : 0;
    s_move = move;
    HSD_PadFlushQueue(HSD_PAD_FLUSH_QUEUE_THROWAWAY);
    const int r = pc_xr_calibrate_pass(done, pushed);
    if (r == 0) {
        return;
    }
    s_calibrating = false;
    if (r == 2) {
        s_dummy_result = 3;
    } else if (!s_place) {
        s_dummy_result = 2; /* the stage's spot is set: straight on */
    }
}

bool pc_xr_place_hold(void) {
    /* The room light first: no stage shown, the fighters idling. */
    if (s_dummy && s_calibrating) {
        calibrate_pass();
        return false;
    }
    if (s_dummy && s_dummy_result != 0) {
        return false; /* ending: no hold for its stage */
    }
    if (s_holding < 0) {
        if (s_pending < 0) {
            if (!s_dummy || aurora_xr_active()) {
                return false;
            }
            s_pending = 0; /* no XR scene to announce the stage: flat testing */
        }
        const int grkind = s_pending;
        s_pending = -1;
        /* Only with a headset already presenting: an XR build playing flat
         * (no runtime, no session yet) never waits for one. Netplay: the
         * peer starts the match on the agreed frame whatever this side
         * shows, so the stage keeps its default placement. */
        if (!s_dummy && (!enabled() || !aurora_xr_active() || pc_net_active())) {
            return false;
        }
        /* Sudden Death: the fight just played, on the stage just placed. */
        if (!s_dummy && s_scene == 3) { /* GS_SUDDEN_DEATH */
            return false;
        }
        /* "Start right away": once there's a stage spot, from it. */
        if (!s_dummy && !s_every_fight && (s_decided || aurora_xr_stage_adopted())) {
            return false;
        }
        /* All-Star's rest area comes back after every fight: placed once a
         * session, it keeps that. */
        static bool rest_area_placed;
        if (!s_dummy && grkind == 0x42) { /* Gr_Kind_Heal */
            if (rest_area_placed) {
                return false;
            }
            rest_area_placed = true;
        }
        /* The pictures that say "fight" for a fight's hold, "done" for the
         * Place Stage menu flow; loaded again when the other one is needed. */
        static int loaded = -1;
        if (loaded != (s_dummy ? 1 : 0)) {
            const bool first = loaded < 0;
            loaded = s_dummy ? 1 : 0;
            load_image(0, s_dummy ? "place-cards-done.png" : "place-cards.png");
            load_image(1, s_dummy ? "place-legend-done.png" : "place-legend.png");
            load_image(2, s_dummy ? "place-cards-hands-done.png" : "place-cards-hands.png");
            load_image(3, s_dummy ? "place-legend-hands-done.png" : "place-legend-hands.png");
            if (first) {
                load_clips();
            }
        }
        s_holding = grkind;
        s_prev = newest_buttons(); /* held coming in: not a press */
        s_down = 0;
        aurora_xr_set_placing(s_cards ? 2 : 1);
        pc_log_line("xr: holding stage %d before the fight for placing", grkind);
    }

    const uint32_t buttons = newest_buttons();
    const uint32_t released = s_prev & ~buttons;
    s_down |= buttons & ~s_prev;
    s_prev = buttons;
    const uint32_t done = released & s_down;
    s_down &= ~released;
    /* Nothing that came in while held reaches the game. */
    HSD_PadFlushQueue(HSD_PAD_FLUSH_QUEUE_THROWAWAY);

    /* A headset taken off (session not running) keeps the hold: the fight
     * shouldn't start unseen. A or Start on any pad still ends it. */
    if (done & PAD_BUTTON_Y) {
        s_cards = !s_cards;
        aurora_xr_set_placing(s_cards ? 2 : 1);
    }
    if (done & PAD_BUTTON_B) {
        aurora_xr_reset_stage();
        pc_log_line("xr: stage %d back at its default placement", s_holding);
    }
    if (s_dummy) {
        if (done & (PAD_BUTTON_A | PAD_BUTTON_START)) {
            aurora_xr_adopt_stage();
            pc_log_line("xr: placement adopted for every stage");
            s_dummy_result = 1;
        }
        return false; /* the fighters keep idling */
    }
    if (done & (PAD_BUTTON_A | PAD_BUTTON_START)) {
        pc_log_line("xr: stage %d placed; the fight begins", s_holding);
        end_hold();
    }
    return true;
}

#endif
