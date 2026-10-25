/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/xr_calibrate.h"

#ifdef AURORA_ENABLE_OPENXR

#include "pc/pc.h"
#include "pc/room_chime.h"
#include "pc/settings.h"
#include "pc/xr_place.h"

#include <SDL3/SDL.h>
#include <aurora/xr.h>
#include <dolphin/pad.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The done card's time before play goes on. */
#define DONE_MS 1500
/* "Room changed. Look again." stays this long on the strip. */
#define CHANGED_MS 4000

typedef struct Rect {
    int x, y, w, h;
} Rect;

/* A straight-alpha RGBA8 picture. */
typedef struct Pic {
    unsigned char* rgba;
    int w, h;
} Pic;

/* The pictures (tools/xr_cards/make_calibration.py): the cards, the
 * no-light and dark-room cards with the cursor on either choice, the
 * pieces put on them, and where those go (calibrate-layout.txt). */
static Pic s_intro, s_strip, s_done, s_choice[2][2], s_text;
static Rect s_slot_count, s_slot_changed, s_slot_found, s_slot_next;
#define PIECES_MAX 64
static struct {
    char name[40];
    Rect r;
} s_pieces[PIECES_MAX];
static int s_piece_count;
static bool s_loaded, s_ok;

enum { PHASE_OFF, PHASE_INTRO, PHASE_LOOKING, PHASE_CHOICE, PHASE_DONE };
static int s_phase;
static bool s_dark;  /* the choice card is the dark room's */
static int s_cursor; /* on it: 0 look again, 1 go on without a light */
static char s_next[40];
static int s_shown_rung, s_shown_changed;
static bool s_shown_line;
static uint64_t s_done_at, s_changed_until;
static bool s_calibrated;
static int s_result = -1;

static bool load_pic(Pic* p, const char* name) {
    p->rgba = pc_xr_load_rgba(name, &p->w, &p->h);
    return p->rgba != NULL;
}

static void load_layout(void) {
    size_t size = 0;
    char* text = pc_xr_load_file("calibrate-layout.txt", &size);
    if (text == NULL) {
        return;
    }
    char* copy = malloc(size + 1);
    memcpy(copy, text, size);
    copy[size] = '\0';
    SDL_free(text);
    for (char* line = strtok(copy, "\n"); line != NULL; line = strtok(NULL, "\n")) {
        Rect r;
        char name[40];
        int used = 0;
        if (sscanf(line, "slot %39s %d %d %d %d", name, &r.x, &r.y, &r.w, &r.h) == 5) {
            Rect* slot = strcmp(name, "count") == 0     ? &s_slot_count :
                         strcmp(name, "changed") == 0   ? &s_slot_changed :
                         strcmp(name, "found") == 0     ? &s_slot_found :
                         strcmp(name, "next") == 0      ? &s_slot_next :
                                                          NULL;
            if (slot != NULL) {
                *slot = r;
            }
        } else if (sscanf(line, "piece %d %d %d %d %n", &r.x, &r.y, &r.w, &r.h, &used) == 4 && used > 0 &&
                   s_piece_count < PIECES_MAX)
        {
            snprintf(s_pieces[s_piece_count].name, sizeof s_pieces[0].name, "%s", line + used);
            s_pieces[s_piece_count].r = r;
            s_piece_count++;
        }
    }
    free(copy);
}

/* The legends and the markers go to Aurora once; the cards as they change. */
static void load(void) {
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    s_ok = load_pic(&s_intro, "calibrate-intro.png") && load_pic(&s_strip, "calibrate-strip.png") &&
           load_pic(&s_done, "calibrate-done.png") && load_pic(&s_choice[0][0], "calibrate-nolight-0.png") &&
           load_pic(&s_choice[0][1], "calibrate-nolight-1.png") && load_pic(&s_choice[1][0], "calibrate-dark-0.png") &&
           load_pic(&s_choice[1][1], "calibrate-dark-1.png") && load_pic(&s_text, "calibrate-text.png");
    load_layout();
    static const char* const once[3] = {"calibrate-legend.png", "calibrate-legend-hands.png", "calibrate-markers.png"};
    for (int i = 0; i < 3; i++) {
        Pic p;
        if (load_pic(&p, once[i])) {
            aurora_xr_set_calibration_image(1 + i, p.w, p.h, p.rgba);
            pc_xr_free_rgba(p.rgba);
        }
    }
    pc_room_chime_load();
    if (!s_ok) {
        pc_log_line("xr: calibration pictures missing; the step runs without its cards");
    }
}

static const Rect* piece(const char* name) {
    for (int i = 0; i < s_piece_count; i++) {
        if (strcmp(s_pieces[i].name, name) == 0) {
            return &s_pieces[i].r;
        }
    }
    return NULL;
}

/* `from` of the pieces' picture over `dst` at x, y (straight alpha). */
static void over(Pic* dst, const Rect* from, int x, int y) {
    if (from == NULL || s_text.rgba == NULL) {
        return;
    }
    for (int j = 0; j < from->h; j++) {
        const int dy = y + j, sy = from->y + j;
        if (dy < 0 || dy >= dst->h || sy < 0 || sy >= s_text.h) {
            continue;
        }
        for (int i = 0; i < from->w; i++) {
            const int dx = x + i, sx = from->x + i;
            if (dx < 0 || dx >= dst->w || sx < 0 || sx >= s_text.w) {
                continue;
            }
            const unsigned char* s = &s_text.rgba[(sy * s_text.w + sx) * 4];
            unsigned char* d = &dst->rgba[(dy * dst->w + dx) * 4];
            const float sa = s[3] / 255.f, da = d[3] / 255.f;
            const float oa = sa + da * (1.f - sa);
            if (oa <= 0.f) {
                continue;
            }
            for (int c = 0; c < 3; c++) {
                d[c] = (unsigned char)((s[c] * sa + d[c] * da * (1.f - sa)) / oa + 0.5f);
            }
            d[3] = (unsigned char)(oa * 255.f + 0.5f);
        }
    }
}

/* Pieces side by side, centred in `slot`. */
static void row(Pic* dst, const Rect* slot, const Rect* const* parts, int n) {
    int w = 0;
    for (int i = 0; i < n; i++) {
        w += parts[i] != NULL ? parts[i]->w : 0;
    }
    int x = slot->x + (slot->w - w) / 2;
    for (int i = 0; i < n; i++) {
        if (parts[i] == NULL) {
            continue;
        }
        over(dst, parts[i], x, slot->y + (slot->h - parts[i]->h) / 2);
        x += parts[i]->w;
    }
}

/* A number's digits, as `kind`'s pieces ("big", "small"), into parts. */
static int digits(int n, const char* kind, const Rect** parts, int max) {
    char text[12];
    snprintf(text, sizeof text, "%d", n < 0 ? 0 : n);
    int k = 0;
    for (const char* c = text; *c && k < max; c++) {
        char name[16];
        snprintf(name, sizeof name, "%s-%c", kind, *c);
        parts[k++] = piece(name);
    }
    return k;
}

/* The card, made from `base` and handed to Aurora. */
static void show(const Pic* base, void (*fill)(Pic*)) {
    if (base->rgba == NULL) {
        return;
    }
    Pic card = {malloc((size_t)base->w * base->h * 4), base->w, base->h};
    memcpy(card.rgba, base->rgba, (size_t)base->w * base->h * 4);
    if (fill != NULL) {
        fill(&card);
    }
    aurora_xr_set_calibration_image(0, card.w, card.h, card.rgba);
    free(card.rgba);
}

static void fill_strip(Pic* card) {
    const Rect* parts[8];
    const int n = digits(s_shown_rung, "big", parts, 8);
    row(card, &s_slot_count, parts, n);
    if (s_shown_line) {
        over(card, piece("changed"), s_slot_changed.x, s_slot_changed.y);
    }
}

static int s_done_found;
static void fill_done(Pic* card) {
    const Rect* parts[10];
    int n = digits(s_done_found, "small", parts, 8);
    parts[n++] = piece(s_done_found == 1 ? "light-found" : "lights-found");
    row(card, &s_slot_found, parts, n);
    char name[48];
    snprintf(name, sizeof name, "next:%s", s_next);
    const Rect* next = piece(name);
    row(card, &s_slot_next, &next, 1);
}

static bool roomtone_on(void) {
    PcGraphicsSettings g;
    pc_settings_graphics(&g);
    return g.xr_lighting == 1;
}

bool pc_xr_calibrate_needed(void) {
    const char* v = getenv("MELEE_XR_CALIBRATE");
    if ((v != NULL && *v == '0') || s_calibrated || !aurora_xr_active() || !roomtone_on()) {
        return false;
    }
    if (!pc_settings_camera_granted()) {
        /* Taken away in the headset's settings as the game ran: Lighting
         * goes back to Vanilla at the next launch. */
        pc_log_line("xr: room light calibration skipped: the headset cameras aren't allowed");
        return false;
    }
    return true;
}

bool pc_xr_calibrated(void) {
    return s_calibrated;
}

int pc_xr_calibrate_result(void) {
    return s_result;
}

static void looking(void) {
    s_phase = PHASE_LOOKING;
    s_shown_rung = -1; /* drawn at the next pass */
    aurora_xr_set_calibrating(2);
}

void pc_xr_calibrate_begin(const char* next) {
    load();
    snprintf(s_next, sizeof s_next, "%s", next != NULL ? next : "");
    s_phase = PHASE_INTRO;
    s_shown_changed = 0;
    s_shown_line = false;
    s_changed_until = 0;
    show(&s_intro, NULL);
    aurora_xr_set_calibrating(1);
    pc_log_line("xr: room light calibration (then %s)", s_next);
}

void pc_xr_calibrate_abort(void) {
    if (s_phase != PHASE_OFF) {
        s_phase = PHASE_OFF;
        aurora_xr_set_calibrating(0);
    }
}

static int finish(int found) {
    s_phase = PHASE_OFF;
    s_calibrated = true;
    s_result = found;
    aurora_xr_set_calibrating(0);
    pc_log_line("xr: room light calibrated: %d lights", found);
    return 1;
}

int pc_xr_calibrate_pass(uint32_t released, int move) {
    if (s_phase == PHASE_OFF) {
        return 1;
    }
    AuroraXrCalibration st = {0};
    aurora_xr_calibration(&st);
    /* Each light that rang chimes from where it is. */
    int number;
    float pos[3];
    while (aurora_xr_calibration_ring(&number, pos)) {
        pc_room_chime_light(number, pos);
    }
    const uint64_t now = SDL_GetTicks();
    switch (s_phase) {
    case PHASE_INTRO:
        if (released & PAD_BUTTON_A) {
            looking();
        } else if (released & PAD_BUTTON_B) {
            pc_xr_calibrate_abort();
            pc_log_line("xr: room light calibration backed out of");
            return 2;
        }
        break;
    case PHASE_LOOKING: {
        if (st.changed != s_shown_changed) {
            s_shown_changed = st.changed;
            s_changed_until = now + CHANGED_MS;
        }
        const bool line = now < s_changed_until;
        if (st.rung != s_shown_rung || line != s_shown_line) {
            s_shown_rung = st.rung;
            s_shown_line = line;
            show(&s_strip, fill_strip);
        }
        if (released & PAD_BUTTON_A) {
            if (st.found > 0) {
                /* Every light found counts, rung here or not. */
                s_phase = PHASE_DONE;
                s_done_found = st.found;
                s_done_at = now;
                show(&s_done, fill_done);
                aurora_xr_set_calibrating(1);
                pc_room_chime_done();
            } else {
                s_phase = PHASE_CHOICE;
                s_dark = st.dark;
                s_cursor = 0;
                show(&s_choice[s_dark][0], NULL);
                aurora_xr_set_calibrating(1);
            }
        } else if (released & PAD_BUTTON_B) {
            pc_xr_calibrate_abort();
            pc_log_line("xr: room light calibration backed out of");
            return 2;
        }
        break;
    }
    case PHASE_CHOICE:
        if (move != 0 && (move > 0) != (s_cursor == 1)) {
            s_cursor = move > 0 ? 1 : 0;
            show(&s_choice[s_dark][s_cursor], NULL);
        }
        if ((released & PAD_BUTTON_A) && s_cursor == 1) {
            return finish(0); /* done without a light, or kept dark */
        }
        if (released & (PAD_BUTTON_A | PAD_BUTTON_B)) {
            looking(); /* keep looking: what was seen is kept */
        }
        break;
    case PHASE_DONE:
        if (now - s_done_at >= DONE_MS) {
            return finish(s_done_found);
        }
        break;
    default:
        break;
    }
    return 0;
}

#endif
