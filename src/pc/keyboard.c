/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Keyboard -> controller port 1.
 *
 * Gamepads are handled by aurora's SDL_Gamepad mapping; this covers the
 * no-controller case. Mapping:
 *   Arrows / WASD  main stick      IJKL     C stick
 *   X = A          Z = B           C = X    V = Y
 *   Q = L          E = R           Tab = Z  Enter = Start
 *   D-pad: T/G/F/H
 *
 * Real keys only count while the window holds keyboard focus; MELEE_KEY_FIFO
 * keys always count (see the comment on s_focused below). MELEE_INPUT_TRACE=1
 * logs one "pad: " line per change of the published pad, with the focus and
 * fifo state that produced it.
 *
 * The FIFOs also take frame scripts (docs/fast-tests-plan.md T2): a line that
 * counts in frames ("X+Down 7f", "tap X 3f", "wait 30f", "until <text> [600f]",
 * "tapuntil Return 9f 60f <text> [3600f]", "mark <id>") is queued and run on the game thread at each frame boundary,
 * so its presses start and end on exact frames however fast the game runs.
 */
#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>
#include <aurora/event.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dolphin/pad.h>
#include <errno.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "pc/pc.h"
#include "pc/net.h"
#include "pc/touch.h"
#include "pc/xr_pointer.h"

static bool s_key[SDL_SCANCODE_COUNT];
static bool s_key_latched[SDL_SCANCODE_COUNT];
static bool s_active;
static bool s_suppressed[SDL_SCANCODE_COUNT];
extern bool pc_menu_is_open(void);

/* Real keys are gated on window focus; MELEE_KEY_FIFO keys are not.
 *
 * SDL's keyboard state array is process-global and SDL_PumpEvents fills it
 * from whatever key events reach the process - and a key event can reach a
 * window that does not hold focus. XSendEvent does exactly that (aim a
 * KeyPress at a window by title and SDL3's X11 backend takes it: it tests
 * xany.send_event only for ConfigureNotify), and under Wine a
 * WM_KEYDOWN can be delivered to a stale foreground window. Without a focus
 * rule the game is driven by whatever is typed elsewhere on the desktop, and
 * because PAD_CONFIRM is A|START (gm_1A36.c:118) a single stray Return walks
 * a menu or drops out of a match.
 *
 * The fifo is asymmetric on purpose: every netplay harness drives menus in
 * windows that are unfocused, and some that are never focused at all, so
 * s_fifo_key, the latch it sets and the s_active arm it performs are all
 * ungated. Do not fold the two sources into one gate - that breaks every
 * harness under tools/.
 *
 * Sampled on the event-pump thread (SDL_GetKeyboardFocus is documented
 * main-thread-only) and read by the 1 kHz poll thread. */
static atomic_bool s_focused;
static bool s_trace; /* MELEE_INPUT_TRACE=1: one line per published pad change */

static const struct {
    SDL_Scancode key;
    u16 button;
} s_button_map[] = {
    {SDL_SCANCODE_X, PAD_BUTTON_A},
    {SDL_SCANCODE_Z, PAD_BUTTON_B},
    {SDL_SCANCODE_C, PAD_BUTTON_X},
    {SDL_SCANCODE_V, PAD_BUTTON_Y},
    {SDL_SCANCODE_Q, PAD_TRIGGER_L},
    {SDL_SCANCODE_E, PAD_TRIGGER_R},
    {SDL_SCANCODE_TAB, PAD_TRIGGER_Z},
    {SDL_SCANCODE_RETURN, PAD_BUTTON_START},
    {SDL_SCANCODE_T, PAD_BUTTON_UP},
    {SDL_SCANCODE_G, PAD_BUTTON_DOWN},
    {SDL_SCANCODE_F, PAD_BUTTON_LEFT},
    {SDL_SCANCODE_H, PAD_BUTTON_RIGHT},
};

static s8 axis(SDL_Scancode neg, SDL_Scancode pos) {
    /* A real GameCube stick reads about +-80 at full deflection. */
    const bool neg_on = s_key[neg] || s_key_latched[neg];
    const bool pos_on = s_key[pos] || s_key_latched[pos];
    return (s8)((pos_on ? 80 : 0) - (neg_on ? 80 : 0));
}

static int i8abs(s8 v) {
    return v < 0 ? -(int)v : v;
}

static SDL_Mutex* s_key_mutex;

/* MELEE_KEY_FIFO=path: test driver that does not depend on X focus. Each
 * line is "Name[+Name...] hold_ms" with SDL scancode names ("Return 150",
 * "X+Down 200"); the keys read as held for hold_ms, one line at a time.
 * MELEE_KEY_FIFO_P2..P4 drive physical pads 2-4 the same way, each on a
 * timeline of its own: a machine playing more than one port online reads
 * its other pads (docs/lan-multipad-plan.md), and the harness needs a hand
 * on them. Those take the keyboard's mapping, from their FIFO only. */
static bool s_fifo_key[SDL_SCANCODE_COUNT];
static bool s_fifo_started;
static bool s_pfifo_key[4][SDL_SCANCODE_COUNT];   /* pads 2-4 (index 1-3) */
static bool s_pfifo_latch[4][SDL_SCANCODE_COUNT]; /* a press shorter than a frame */
static const char* s_fifo_path[4];

/* ---- frame scripts (docs/fast-tests-plan.md T2) -------------------------------
 * Each FIFO has a queue of commands, run at frame boundaries (publish_locked,
 * the game thread): a hold lasts exactly its frames, a wait idles, an until
 * waits for a log line (pc_script_log_seen, any thread), a mark logs that the
 * script got there. A harness writes a whole run ahead and checks the marks,
 * instead of holding keys for milliseconds and sleeping between them. */
typedef enum { SC_HOLD, SC_TAP, SC_WAIT, SC_UNTIL, SC_TAPUNTIL, SC_MARK } ScKind;
/* A tapuntil gives up after this many frames: a step that never comes. */
#define SC_TAPUNTIL_CAP 3600
typedef struct ScCmd {
    ScKind kind;
    int frames; /* a hold's, a tap's (held, then as long released), a wait's; an until's cap, 0 none */
    int every;  /* a tapuntil's: a tap starts this often until its text is seen */
    int cap;    /* a tapuntil's: frames before it gives up */
    SDL_Scancode keys[8];
    int nkeys;
    char text[120]; /* an until's text, a mark's id */
} ScCmd;
#define SC_QUEUE 256
typedef struct Script {
    ScCmd q[SC_QUEUE]; /* under s_key_mutex */
    int head, n;
    bool running; /* cur has started; game thread only from here */
    ScCmd cur;
    int left;  /* frames left of cur (of its phase) */
    int phase; /* a tap: 0 held, 1 released */
} Script;
static Script s_script[4];
static unsigned s_script_frame; /* frames stepped, for the log */
/* Netplay sampled the pad (pc_keyboard_sim_frame) since the last frame
 * boundary: the boundary leaves the scripts to it. Game thread. */
static bool s_sim_sampled;
/* An until's text, which pc_script_log_seen matches against every log line. */
static SDL_Mutex* s_until_mutex;
static atomic_bool s_until_ready;
static char s_until_text[4][120];
static bool s_until_armed[4], s_until_hit[4];

static int script_frames(const char* tok) {
    char* end = NULL;
    const long n = tok != NULL ? strtol(tok, &end, 10) : -1;
    return tok != NULL && end != tok && end[0] == 'f' && end[1] == '\0' && n >= 0 && n < 1000000 ? (int)n : -1;
}

static int script_keys(char* tok, SDL_Scancode* keys) {
    int n = 0;
    for (char* k = strtok(tok, "+"); k != NULL && n < 8; k = strtok(NULL, "+")) {
        SDL_Scancode sc = SDL_GetScancodeFromName(k);
        if (sc != SDL_SCANCODE_UNKNOWN) {
            keys[n++] = sc;
        }
    }
    return n;
}

/* A frame-script line into a command; false for the old "keys ms" form. */
static bool script_parse(const char* in, ScCmd* c) {
    char line[160];
    snprintf(line, sizeof line, "%s", in);
    line[strcspn(line, "\r\n")] = '\0';
    memset(c, 0, sizeof *c);
    char* rest = strchr(line, ' ');
    if (rest == NULL) {
        return false;
    }
    *rest++ = '\0';
    char* last = strrchr(rest, ' ');
    const char* tail = last != NULL ? last + 1 : rest;
    if (strcmp(line, "mark") == 0) {
        c->kind = SC_MARK;
        snprintf(c->text, sizeof c->text, "%s", rest);
        return true;
    }
    if (strcmp(line, "wait") == 0) {
        c->kind = SC_WAIT;
        c->frames = script_frames(rest);
        return c->frames >= 0;
    }
    if (strcmp(line, "until") == 0) {
        c->kind = SC_UNTIL;
        c->frames = last != NULL ? script_frames(tail) : -1;
        if (c->frames >= 0) {
            *last = '\0'; /* the cap is no part of the text */
        } else {
            c->frames = 0;
        }
        snprintf(c->text, sizeof c->text, "%s", rest);
        return c->text[0] != '\0';
    }
    if (strcmp(line, "tapuntil") == 0) {
        /* "tapuntil Keys Nf Mf <text>": tap every M frames until <text>. */
        char* keys = rest;
        char* a = strchr(keys, ' ');
        char* b = a != NULL ? strchr(a + 1, ' ') : NULL;
        if (a == NULL || b == NULL) {
            return false;
        }
        *a++ = '\0';
        *b++ = '\0';
        char* text = b;
        char* sp = strchr(a, ' ');
        if (sp != NULL) {
            *sp = '\0';
        }
        c->kind = SC_TAPUNTIL;
        c->frames = script_frames(a);
        b = text;
        char* every = b;
        text = strchr(every, ' ');
        if (text == NULL) {
            return false;
        }
        *text++ = '\0';
        c->every = script_frames(every);
        /* An optional last "Nf": the cap, no part of the text. */
        c->cap = SC_TAPUNTIL_CAP;
        char* tail_sp = strrchr(text, ' ');
        if (tail_sp != NULL && script_frames(tail_sp + 1) > 0) {
            c->cap = script_frames(tail_sp + 1);
            *tail_sp = '\0';
        }
        snprintf(c->text, sizeof c->text, "%s", text);
        c->nkeys = script_keys(keys, c->keys);
        return c->nkeys > 0 && c->frames > 0 && c->every > c->frames && c->text[0] != '\0';
    }
    if (strcmp(line, "tap") == 0) {
        if (last == NULL || (c->frames = script_frames(tail)) <= 0) {
            return false;
        }
        *last = '\0';
        c->kind = SC_TAP;
        c->nkeys = script_keys(rest, c->keys);
        return c->nkeys > 0;
    }
    /* "Keys Nf": a hold. */
    if (last != NULL || (c->frames = script_frames(rest)) <= 0) {
        return false;
    }
    c->kind = SC_HOLD;
    c->nkeys = script_keys(line, c->keys);
    return c->nkeys > 0;
}

void pc_script_log_seen(const char* line) {
    if (!atomic_load_explicit(&s_until_ready, memory_order_acquire) || line == NULL ||
        strncmp(line, "script: ", 8) == 0)
    {
        return;
    }
    SDL_LockMutex(s_until_mutex);
    for (int pad = 0; pad < 4; pad++) {
        if (s_until_armed[pad] && !s_until_hit[pad] && strstr(line, s_until_text[pad]) != NULL) {
            s_until_hit[pad] = true;
        }
    }
    SDL_UnlockMutex(s_until_mutex);
}

static void script_keys_set(int pad, const ScCmd* c, bool on) {
    bool* held = pad == 0 ? s_fifo_key : s_pfifo_key[pad];
    for (int i = 0; i < c->nkeys; i++) {
        held[c->keys[i]] = on;
    }
}

static bool script_until_hit(int pad, bool disarm) {
    SDL_LockMutex(s_until_mutex);
    const bool hit = s_until_hit[pad];
    if (disarm) {
        s_until_armed[pad] = s_until_hit[pad] = false;
    }
    SDL_UnlockMutex(s_until_mutex);
    return hit;
}

/* One frame boundary of every queue (s_key_mutex held): what is held this
 * frame. A command that ends lets the next start on the same boundary. */
static void script_tick_locked(void) {
    s_script_frame++;
    for (int pad = 0; pad < 4; pad++) {
        Script* s = &s_script[pad];
        for (int guard = 0; guard < SC_QUEUE; guard++) {
            if (!s->running) {
                if (s->n == 0) {
                    break;
                }
                s->cur = s->q[s->head];
                s->head = (s->head + 1) % SC_QUEUE;
                s->n--;
                s->running = true;
                s->left = s->cur.frames;
                s->phase = 0;
                switch (s->cur.kind) {
                case SC_MARK:
                    pc_log_line("script: P%d mark %s at frame %u", pad + 1, s->cur.text, s_script_frame);
                    s->running = false;
                    continue;
                case SC_UNTIL:
                case SC_TAPUNTIL:
                    if (s_trace) {
                        pc_log_line("script: P%d until \"%s\" from frame %u", pad + 1, s->cur.text, s_script_frame);
                    }
                    if (s->cur.kind == SC_TAPUNTIL) {
                        s->left = s->cur.cap;
                        s->phase = 0; /* frames into the current tap's cycle */
                        script_keys_set(pad, &s->cur, true);
                        if (pad == 0) {
                            s_active = true;
                        }
                    }
                    SDL_LockMutex(s_until_mutex);
                    snprintf(s_until_text[pad], sizeof s_until_text[pad], "%s", s->cur.text);
                    s_until_armed[pad] = true;
                    s_until_hit[pad] = false;
                    SDL_UnlockMutex(s_until_mutex);
                    break;
                case SC_HOLD:
                case SC_TAP:
                    script_keys_set(pad, &s->cur, true);
                    if (pad == 0) {
                        s_active = true;
                    }
                    break;
                case SC_WAIT:
                    break;
                }
                break; /* it has this frame */
            }
            switch (s->cur.kind) {
            case SC_HOLD:
                if (--s->left > 0) {
                    break;
                }
                script_keys_set(pad, &s->cur, false);
                s->running = false;
                continue;
            case SC_TAP:
                if (--s->left > 0) {
                    break;
                }
                if (s->phase == 0) {
                    script_keys_set(pad, &s->cur, false);
                    s->phase = 1;
                    s->left = s->cur.frames;
                    break;
                }
                s->running = false;
                continue;
            case SC_WAIT:
                if (--s->left > 0) {
                    break;
                }
                s->running = false;
                continue;
            case SC_UNTIL:
                if (script_until_hit(pad, false)) {
                    script_until_hit(pad, true);
                    pc_log_line("script: P%d saw \"%s\" at frame %u", pad + 1, s->cur.text, s_script_frame);
                    s->running = false;
                    continue;
                }
                if (s->cur.frames > 0 && --s->left <= 0) {
                    script_until_hit(pad, true);
                    pc_log_line("script: P%d timeout at \"%s\" after %d frames; the rest of its script is dropped",
                        pad + 1, s->cur.text, s->cur.frames);
                    s->running = false;
                    s->n = 0;
                }
                break;
            case SC_TAPUNTIL:
                if (script_until_hit(pad, false)) {
                    script_until_hit(pad, true);
                    script_keys_set(pad, &s->cur, false);
                    pc_log_line("script: P%d saw \"%s\" at frame %u", pad + 1, s->cur.text, s_script_frame);
                    s->running = false;
                    continue;
                }
                if (--s->left <= 0) {
                    script_until_hit(pad, true);
                    script_keys_set(pad, &s->cur, false);
                    /* Unlike an until, it goes on: whoever wrote it checks
                     * whether the text came, and a mark after it still
                     * tells them the tapping is over. */
                    pc_log_line("script: P%d gave up tapping for \"%s\" after %d frames", pad + 1, s->cur.text,
                        s->cur.cap);
                    s->running = false;
                    continue;
                }
                /* Held for `frames`, released until `every`, again. */
                s->phase = (s->phase + 1) % s->cur.every;
                script_keys_set(pad, &s->cur, s->phase < s->cur.frames);
                break;
            case SC_MARK:
                break;
            }
            break;
        }
    }
}

/* The FIFO's lines, read with read() rather than stdio: this thread waits in
 * the read for good (the FIFO never reads as EOF, below), and inside fgets it
 * would hold the FILE's lock there, which exit() takes to flush every stream
 * -- the game hung at exit until the harness killed it. Windows meets a FIFO
 * only under Wine (the test harness), through the CRT's own calls. */
#ifdef _WIN32
#define fifo_open_rw(path) _open((path), _O_RDWR | _O_BINARY)
#define fifo_read(fd, p, n) _read((fd), (p), (unsigned)(n))
#define fifo_close _close
#else
#define fifo_open_rw(path) open((path), O_RDWR)
#define fifo_read read
#define fifo_close close
#endif

typedef struct LineIn {
    int fd;
    size_t n;
    char buf[512];
} LineIn;

static bool line_next(LineIn* in, char* out, int size) {
    for (;;) {
        char* nl = memchr(in->buf, '\n', in->n);
        if (nl != NULL || in->n >= sizeof in->buf) {
            size_t len = nl != NULL ? (size_t)(nl - in->buf) + 1 : sizeof in->buf;
            size_t keep = len < (size_t)size - 1 ? len : (size_t)size - 1;
            memcpy(out, in->buf, keep);
            out[keep] = '\0';
            in->n -= len;
            memmove(in->buf, in->buf + len, in->n);
            return true;
        }
        long r = (long)fifo_read(in->fd, in->buf + in->n, sizeof in->buf - in->n);
        if (r > 0) {
            in->n += (size_t)r;
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
}

static int fifo_thread(void* arg) {
    const int pad = (int)(intptr_t)arg;
    const char* path = s_fifo_path[pad];
    bool* held = pad == 0 ? s_fifo_key : s_pfifo_key[pad];
    char line[128];
    for (;;) {
        /* Read-write: this end then counts as a writer too, so the FIFO
         * never reads as EOF and is never reopened. Read-only, a writer
         * that opened it between EOF and the reopen had its line dropped
         * with the closing pipe -- harness threads writing one FIFO (a
         * match's workout and the main thread's waits) lost a mark that
         * way, and the step waited out its 120 s. The Windows build under
         * Wine lost them the same way. */
        LineIn in = { .fd = fifo_open_rw(path) };
        if (in.fd < 0) {
            SDL_Delay(200);
            continue;
        }
        while (line_next(&in, line, sizeof line)) {
            ScCmd cmd;
            if (script_parse(line, &cmd)) {
                /* A frame command: into its queue, for the frame boundaries. */
                SDL_LockMutex(s_key_mutex);
                while (s_script[pad].n == SC_QUEUE) {
                    SDL_UnlockMutex(s_key_mutex);
                    SDL_Delay(5);
                    SDL_LockMutex(s_key_mutex);
                }
                Script* s = &s_script[pad];
                s->q[(s->head + s->n) % SC_QUEUE] = cmd;
                s->n++;
                SDL_UnlockMutex(s_key_mutex);
                continue;
            }
            SDL_Scancode keys[8];
            int n = 0, hold = 120;
            char* sp = strchr(line, ' ');
            if (sp != NULL) {
                *sp = '\0';
                hold = atoi(sp + 1);
            }
            for (char* tok = strtok(line, "+\n"); tok != NULL && n < 8; tok = strtok(NULL, "+\n")) {
                SDL_Scancode sc = SDL_GetScancodeFromName(tok);
                if (sc != SDL_SCANCODE_UNKNOWN) {
                    keys[n++] = sc;
                }
            }
            if (n == 0) {
                continue;
            }
            SDL_LockMutex(s_key_mutex);
            for (int i = 0; i < n; i++) {
                held[keys[i]] = true;
                if (pad == 0) {
                    s_key_latched[keys[i]] = true;
                } else {
                    s_pfifo_latch[pad][keys[i]] = true;
                }
            }
            s_active = true;
            SDL_UnlockMutex(s_key_mutex);
            SDL_Delay((Uint32)hold);
            SDL_LockMutex(s_key_mutex);
            for (int i = 0; i < n; i++) {
                held[keys[i]] = false;
            }
            SDL_UnlockMutex(s_key_mutex);
            SDL_Delay(100);
        }
        fifo_close(in.fd);
    }
    return 0;
}

/* Pads 2-4 from their FIFOs (s_key_mutex held): the keyboard's mapping. */
static void publish_pad_fifos_locked(void) {
    for (int pad = 1; pad < 4; pad++) {
        if (s_fifo_path[pad] == NULL) {
            continue;
        }
        const bool* k = s_pfifo_key[pad];
        const bool* l = s_pfifo_latch[pad];
#define ON(sc) (k[sc] || l[sc])
        PADStatus st = {0};
        for (size_t i = 0; i < sizeof(s_button_map) / sizeof(s_button_map[0]); i++) {
            if (ON(s_button_map[i].key)) {
                st.button |= s_button_map[i].button;
            }
        }
        st.stickX = (s8)((ON(SDL_SCANCODE_RIGHT) ? 80 : 0) - (ON(SDL_SCANCODE_LEFT) ? 80 : 0));
        st.stickY = (s8)((ON(SDL_SCANCODE_UP) ? 80 : 0) - (ON(SDL_SCANCODE_DOWN) ? 80 : 0));
        st.substickX = (s8)((ON(SDL_SCANCODE_L) ? 80 : 0) - (ON(SDL_SCANCODE_J) ? 80 : 0));
        st.substickY = (s8)((ON(SDL_SCANCODE_I) ? 80 : 0) - (ON(SDL_SCANCODE_K) ? 80 : 0));
        st.triggerLeft = ON(SDL_SCANCODE_Q) ? 255 : 0;
        st.triggerRight = ON(SDL_SCANCODE_E) ? 255 : 0;
#undef ON
        memset(s_pfifo_latch[pad], 0, sizeof s_pfifo_latch[pad]);
        /* Connected from the start, at rest between lines: a pad, not no pad. */
        PADSetVirtualStatus((u32)pad, &st);
    }
}

static void lock_keys(void) {
    if (s_key_mutex == NULL) {
        s_key_mutex = SDL_CreateMutex();
    }
    if (s_key_mutex != NULL) {
        SDL_LockMutex(s_key_mutex);
    }
}

static void unlock_keys(void) {
    if (s_key_mutex != NULL) {
        SDL_UnlockMutex(s_key_mutex);
    }
}

void pc_keyboard_event(const SDL_Event* e) {
    if (e->type != SDL_EVENT_KEY_DOWN && e->type != SDL_EVENT_KEY_UP) {
        return;
    }
    if (e->key.scancode >= SDL_SCANCODE_COUNT || e->key.repeat) {
        return;
    }
    /* This runs on the event-pump thread, so SDL_GetKeyboardFocus is legal. A
     * press that arrives without focus is refused outright: it must not arm
     * s_active and it must not set the latch. Releases still run, they only
     * ever clear state. */
    if (e->type == SDL_EVENT_KEY_DOWN && SDL_GetKeyboardFocus() == NULL) {
        static bool logged;
        if (!logged) {
            logged = true;
            pc_log_line("pad: refusing keyboard input, window is not focused"
                        " (MELEE_KEY_FIFO is unaffected)");
        }
        return;
    }
    lock_keys();
    if (e->type == SDL_EVENT_KEY_DOWN) {
        s_key[e->key.scancode] = true;
        s_key_latched[e->key.scancode] = true;
    } else {
        s_key[e->key.scancode] = false;
    }
    s_active = true;
    unlock_keys();
}

static void trace_locked(const PADStatus* st) {
    static PADStatus last;
    static bool have_last;
    size_t i;
    bool fifo_held = false;
    if (have_last && memcmp(&last, st, sizeof last) == 0) {
        return;
    }
    last = *st;
    have_last = true;
    for (i = 0; i < SDL_SCANCODE_COUNT; i++) {
        if (s_fifo_key[i]) {
            fifo_held = true;
            break;
        }
    }
    /* "focus 0 fifo 0" with a non-zero button means the input came from
     * neither source this file owns - which is the one thing a log could not
     * tell us when a Wine build steered its own menus. */
    pc_log_line("pad: btn %04x stick %d,%d sub %d,%d trig %u,%u focus %d fifo %d", st->button,
        st->stickX, st->stickY, st->substickX, st->substickY, (unsigned)st->triggerLeft,
        (unsigned)st->triggerRight, atomic_load_explicit(&s_focused, memory_order_relaxed) ? 1 : 0,
        fifo_held ? 1 : 0);
}

/* Merge the three sources - polled SDL keyboard state (focus-gated), fifo keys
 * and the latch - into port 0's virtual pad. Call with s_key_mutex held. */
/* `script_tick`: run the frame scripts' boundary first. `consume_latch`:
 * this is the frame boundary's publish, which hands a press shorter than a
 * frame to exactly one read and clears it. */
static void publish_locked(bool script_tick, bool consume_latch) {
    if (!s_fifo_started) {
        static const char* const names[4] = {
            "MELEE_KEY_FIFO", "MELEE_KEY_FIFO_P2", "MELEE_KEY_FIFO_P3", "MELEE_KEY_FIFO_P4"};
        s_fifo_started = true;
        s_trace = getenv("MELEE_INPUT_TRACE") != NULL;
        for (int pad = 0; pad < 4; pad++) {
            const char* fifo = getenv(names[pad]);
            if (fifo != NULL && fifo[0] != '\0') {
                s_fifo_path[pad] = fifo;
                if (s_until_mutex == NULL) {
                    s_until_mutex = SDL_CreateMutex();
                    atomic_store_explicit(&s_until_ready, s_until_mutex != NULL, memory_order_release);
                }
                SDL_DetachThread(SDL_CreateThread(fifo_thread, "keyfifo", (void*)(intptr_t)pad));
                s_active = s_active || pad == 0;
            }
        }
    }
    if (script_tick && atomic_load_explicit(&s_until_ready, memory_order_relaxed)) {
        script_tick_locked();
    }
    publish_pad_fifos_locked();
    PADStatus st = {0};
    bool any_active = false;
    unsigned engaged = 0; /* port 1's devices showing input, for rumble */
    if (s_active) {
        size_t i;
        int n = 0;
        /* SDL3 documents SDL_GetKeyboardState as safe to call from any thread
         * (SDL_keyboard.h: "It is safe to call this function from any
         * thread"), so the poll thread reading it while the main thread pumps
         * is not a race by SDL's contract. */
        const bool* keys = SDL_GetKeyboardState(&n);
        if (n > SDL_SCANCODE_COUNT) {
            n = SDL_SCANCODE_COUNT;
        }
        memset(s_key, 0, sizeof s_key);
        if (atomic_load_explicit(&s_focused, memory_order_relaxed)) {
            memcpy(s_key, keys, (size_t)n);
        }
        for (i = 0; i < SDL_SCANCODE_COUNT; i++) {
            s_key[i] |= s_fifo_key[i];
        }
        for (i = 0; i < SDL_SCANCODE_COUNT; i++) {
            if (pc_menu_is_open())
                s_suppressed[i] = s_key[i];
            else if (!s_key[i])
                s_suppressed[i] = false;
            if (s_suppressed[i])
                s_key[i] = false;
        }
        for (i = 0; i < sizeof(s_button_map) / sizeof(s_button_map[0]); i++) {
            SDL_Scancode key = s_button_map[i].key;
            if (s_key[key] || s_key_latched[key]) {
                st.button |= s_button_map[i].button;
            }
        }
        st.stickX = axis(SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT);
        if (st.stickX == 0) {
            st.stickX = axis(SDL_SCANCODE_A, SDL_SCANCODE_D);
        }
        st.stickY = axis(SDL_SCANCODE_DOWN, SDL_SCANCODE_UP);
        if (st.stickY == 0) {
            st.stickY = axis(SDL_SCANCODE_S, SDL_SCANCODE_W);
        }
        st.substickX = axis(SDL_SCANCODE_J, SDL_SCANCODE_L);
        st.substickY = axis(SDL_SCANCODE_K, SDL_SCANCODE_I);
        st.triggerLeft = (s_key[SDL_SCANCODE_Q] || s_key_latched[SDL_SCANCODE_Q]) ? 255 : 0;
        st.triggerRight = (s_key[SDL_SCANCODE_E] || s_key_latched[SDL_SCANCODE_E]) ? 255 : 0;
        if (consume_latch) {
            memset(s_key_latched, 0, sizeof(s_key_latched));
        }
        any_active = true;
        if (pc_pad_engaged(&st))
            engaged |= 1u << PC_PAD_DEVICE_KEYS;
    }

#ifdef AURORA_ENABLE_OPENXR
    /* Headset controllers in XR builds (extern/aurora/lib/xr): same merge as
     * the touch overlay below. */
    PADStatus xr_st = {0};
    if (aurora_xr_get_pad(&xr_st)) {
        if (pc_pad_engaged(&xr_st))
            engaged |= 1u << PC_PAD_DEVICE_XR;
        st.button |= xr_st.button;
        if (xr_st.stickX != 0 || xr_st.stickY != 0) {
            st.stickX = xr_st.stickX;
            st.stickY = xr_st.stickY;
        }
        if (xr_st.substickX != 0 || xr_st.substickY != 0) {
            st.substickX = xr_st.substickX;
            st.substickY = xr_st.substickY;
        }
        if (xr_st.triggerLeft > st.triggerLeft)
            st.triggerLeft = xr_st.triggerLeft;
        if (xr_st.triggerRight > st.triggerRight)
            st.triggerRight = xr_st.triggerRight;
        any_active = true;
    }
#endif

    PADStatus touch_st = {0};
    if (pc_touch_get_status(&touch_st)) {
        if (pc_pad_engaged(&touch_st))
            engaged |= 1u << PC_PAD_DEVICE_KEYS;
        st.button |= touch_st.button;
        if (touch_st.stickX != 0 || touch_st.stickY != 0) {
            st.stickX = touch_st.stickX;
            st.stickY = touch_st.stickY;
        }
        if (touch_st.substickX != 0 || touch_st.substickY != 0) {
            st.substickX = touch_st.substickX;
            st.substickY = touch_st.substickY;
        }
        if (touch_st.triggerLeft > st.triggerLeft) {
            st.triggerLeft = touch_st.triggerLeft;
        }
        if (touch_st.triggerRight > st.triggerRight) {
            st.triggerRight = touch_st.triggerRight;
        }
        any_active = true;
    }

    /* Port 1 carries a real GC-adapter controller too (gcadapter.c leaves this
     * slot to us). Keyboard/touch layer on top: dominant stick, OR buttons,
     * max triggers -- the same merge aurora does for virtual pads. Clear once
     * when every source goes quiet so a released button does not stick. */
    PADStatus gc_st;
    if (pc_gcadapter_status(0, &gc_st)) {
        if (pc_pad_engaged(&gc_st))
            engaged |= 1u << PC_PAD_DEVICE_GCADAPTER;
        st.button |= gc_st.button;
        if (i8abs(gc_st.stickX) > i8abs(st.stickX))
            st.stickX = gc_st.stickX;
        if (i8abs(gc_st.stickY) > i8abs(st.stickY))
            st.stickY = gc_st.stickY;
        if (i8abs(gc_st.substickX) > i8abs(st.substickX))
            st.substickX = gc_st.substickX;
        if (i8abs(gc_st.substickY) > i8abs(st.substickY))
            st.substickY = gc_st.substickY;
        if (gc_st.triggerLeft > st.triggerLeft)
            st.triggerLeft = gc_st.triggerLeft;
        if (gc_st.triggerRight > st.triggerRight)
            st.triggerRight = gc_st.triggerRight;
        any_active = true;
    }

    /* Clicks on the menus with the headset's lasers (xr_pointer.c). Pointing
     * isn't playing on a device: a pinch or a trigger on a laser leaves the
     * active device (rumble's, and the one Options > Controls shows) as it
     * was, so a hand can set the gamepad's controls. */
    const u16 pointer_buttons = pc_xr_pointer_frame(&st);
    if (pointer_buttons != 0) {
        st.button |= pointer_buttons;
        any_active = true;
    }
    pc_pad_rumble_engaged(engaged);

    static bool s_published0;
    if (any_active) {
        if (s_trace) {
            trace_locked(&st);
        }
        PADSetVirtualStatus(0, &st);
        s_published0 = true;
    } else if (s_published0) {
        PADClearVirtualStatus(0);
        s_published0 = false;
    }
}

/* Frame boundary, on the thread that pumps SDL events (pc_frame_boundary,
 * src/pc/vi.c). This is the ONLY writer of port 0's virtual pad, and it runs
 * exactly once per frame, right after the pump: SDL's keyboard array only
 * changes when the main thread pumps events, so nothing between two frame
 * boundaries can be observed here that the boundary cannot observe. A 1 kHz
 * poll used to call this too (src/pc/input_poll.c, deleted): it re-published
 * identical bytes 16 times a frame into state PADRead merges unlocked, and it
 * cleared the sub-frame latch 1 ms after a press, so what the simulation saw
 * depended on thread interleaving. The latch is consumed here instead: PADRead
 * happens once per pad-alarm period (1/60 s cap, lb_0195.c:84-86), so a press
 * shorter than a frame survives to exactly one read and no further. */
void pc_keyboard_apply(void) {
    pc_gcadapter_apply();
    const bool focused = SDL_GetKeyboardFocus() != NULL;
    lock_keys();
    if (atomic_exchange_explicit(&s_focused, focused, memory_order_relaxed) && !focused) {
        /* Focus just left. Drop the real-key latch so alt-tabbing mid-hold
         * cannot leave a button pressed, but keep what the fifo holds. */
        for (size_t i = 0; i < SDL_SCANCODE_COUNT; i++) {
            if (!s_fifo_key[i]) {
                s_key_latched[i] = false;
            }
        }
    }
    /* Where netplay simulates, the scripts step where it samples the pad
     * (pc_keyboard_sim_frame): a frame presented there is not always one
     * simulated, and a hold must last its frames of the simulation. Any
     * other frame (offline, or a scene netplay doesn't tick, such as the
     * results) steps here. */
    const bool sampled = s_sim_sampled;
    s_sim_sampled = false;
    publish_locked(!(pc_net_active() && sampled), true);
    unlock_keys();
}

bool pc_keyboard_scripted(void) {
    return atomic_load_explicit(&s_until_ready, memory_order_relaxed);
}

void pc_keyboard_sim_frame(void) {
    if (!atomic_load_explicit(&s_until_ready, memory_order_relaxed)) {
        return; /* no frame scripts: the boundary's publish stands */
    }
    lock_keys();
    s_sim_sampled = true;
    publish_locked(true, false);
    unlock_keys();
}
