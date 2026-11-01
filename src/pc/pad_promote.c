/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Port 1 for the pad in use (Android).
 *
 * The name keyboard and netplay read port 1 only (mnnamenew.c, net.c), and
 * SDL numbers its player slots by every gamepad it sees. A RedMagic phone
 * shows its own shoulder triggers as an "Xbox 360 Controller" that connects
 * before a player's real pad and takes port 1 for good, so the real pad
 * typed nothing and would have played nothing online (XL-H2ZDTD).
 *
 * The rule: while the pad on port 1 has never been pressed (or port 1 has
 * none), the first pad on another port to press a button moves to port 1,
 * and the one it displaces takes its old port. Never during a fight: there a
 * swap would hand a fighter to another pad. Through PADSetPortForIndex, so
 * the order is saved and the next launch starts with it.
 *
 * Android only (not in XR, which has its own rule in xr_scene.c);
 * MELEE_PAD_PROMOTE=1 turns it on anywhere, =0 off.
 *
 * MELEE_TEST_VPADS=N attaches N SDL virtual gamepads (the first as the
 * phone's Xbox 360 device, the rest as Xbox Series pads), and
 * MELEE_TEST_VPAD_PRESS="frame:pad:button[,...]" holds a button
 * (a, b, x, y, start, up, down, left, right) of virtual pad `pad` for 4
 * frames from game frame `frame`: a desktop check of the rule. */
#include "pc/input_poll.h"
#include "pc/pc.h"

#include <SDL3/SDL.h>
#include <dolphin/pad.h>
#include <melee/gm/forward.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#define XR_ACTIVE() aurora_xr_active()
#else
#define XR_ACTIVE() false
#endif

int scene_kind(void);

enum { MAX_TRACKED = 16, MAX_VPADS = 4, MAX_PRESSES = 32, PRESS_FRAMES = 4 };

static struct {
    SDL_JoystickID id;
    bool down; /* any button held last frame */
    bool used; /* pressed since it connected */
} s_pads[MAX_TRACKED];

static bool tracked_down(SDL_JoystickID id, bool down, bool* edge) {
    int free_slot = -1;
    for (int i = 0; i < MAX_TRACKED; i++) {
        if (s_pads[i].id == id) {
            *edge = down && !s_pads[i].down;
            s_pads[i].down = down;
            if (down)
                s_pads[i].used = true;
            return s_pads[i].used;
        }
        if (s_pads[i].id == 0 && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        free_slot = 0; /* more pads than anyone plugs in: reuse a slot */
    s_pads[free_slot].id = id;
    s_pads[free_slot].down = down;
    s_pads[free_slot].used = down;
    *edge = down;
    return down;
}

static bool gamepad_down(SDL_Gamepad* g) {
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b))
            return true;
    }
    /* A trigger counts when pulled most of the way. */
    return SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 24000 ||
           SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 24000;
}

static bool in_fight(void) {
    const int kind = scene_kind();
    return kind == GS_VS || kind == GS_SUDDEN_DEATH || kind == GS_TRAINING;
}

static const char* port_name(u32 port) {
    const char* name = PADGetName(port);
    return name != NULL ? name : "?";
}

/* A pad on no port (saved preferences can leave one so) takes the lowest
 * free port, so the rule below can see it. */
static void place_portless(void) {
    for (u32 index = 0; index < PADCount(); index++) {
        bool placed = false;
        int free_port = -1;
        for (u32 port = 0; port < PAD_CHANMAX; port++) {
            const s32 at = PADGetIndexForPort(port);
            if (at == (s32)index)
                placed = true;
            else if (at < 0 && free_port < 0)
                free_port = (int)port;
        }
        if (!placed && free_port >= 0) {
            PADSetPortForIndex(index, (u32)free_port);
            pc_log_line("pads: '%s' had no port, now port %d", port_name((u32)free_port), free_port + 1);
        }
    }
}

/* ---- MELEE_TEST_VPADS -------------------------------------------------------- */

static SDL_Joystick* s_vpad[MAX_VPADS];
static int s_nvpads;
static struct {
    int frame, pad;
    SDL_GamepadButton button;
} s_press[MAX_PRESSES];
static int s_npress;

static SDL_GamepadButton button_named(const char* s) {
    static const struct {
        const char* name;
        SDL_GamepadButton b;
    } names[] = {
        {"a", SDL_GAMEPAD_BUTTON_SOUTH},       {"b", SDL_GAMEPAD_BUTTON_EAST},
        {"x", SDL_GAMEPAD_BUTTON_WEST},        {"y", SDL_GAMEPAD_BUTTON_NORTH},
        {"start", SDL_GAMEPAD_BUTTON_START},   {"up", SDL_GAMEPAD_BUTTON_DPAD_UP},
        {"down", SDL_GAMEPAD_BUTTON_DPAD_DOWN}, {"left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
        {"right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(s, names[i].name) == 0)
            return names[i].b;
    }
    return SDL_GAMEPAD_BUTTON_INVALID;
}

static void test_vpads_init(void) {
    const char* n = getenv("MELEE_TEST_VPADS");
    s_nvpads = n != NULL ? atoi(n) : 0;
    if (s_nvpads > MAX_VPADS)
        s_nvpads = MAX_VPADS;
    if (s_nvpads > 0) /* a test window rarely has the focus, and SDL drops pad input without it */
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    for (int i = 0; i < s_nvpads; i++) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0x045e;
        desc.product_id = i == 0 ? 0x028e : 0x0b13;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = i == 0 ? "Test phone triggers (Xbox 360)" : "Test Xbox Series pad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        s_vpad[i] = id != 0 ? SDL_OpenJoystick(id) : NULL;
        if (s_vpad[i] != NULL) { /* a virtual axis starts at 0: half a trigger */
            SDL_SetJoystickVirtualAxis(s_vpad[i], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
            SDL_SetJoystickVirtualAxis(s_vpad[i], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
        }
        pc_log_line("pads: test virtual pad %d: instance %u%s", i, (unsigned)id, s_vpad[i] ? "" : " (failed)");
    }
    const char* p = getenv("MELEE_TEST_VPAD_PRESS");
    while (p != NULL && *p != '\0' && s_npress < MAX_PRESSES) {
        int frame, pad, len = 0;
        char button[16];
        if (sscanf(p, "%d:%d:%15[a-z]%n", &frame, &pad, button, &len) != 3)
            break;
        s_press[s_npress].frame = frame;
        s_press[s_npress].pad = pad;
        s_press[s_npress].button = button_named(button);
        s_npress++;
        p += len;
        if (*p == ',')
            p++;
    }
}

static void test_vpads_frame(int frame) {
    for (int i = 0; i < s_npress; i++) {
        const int pad = s_press[i].pad;
        if (pad < 0 || pad >= s_nvpads || s_vpad[pad] == NULL ||
            s_press[i].button == SDL_GAMEPAD_BUTTON_INVALID)
            continue;
        if (frame == s_press[i].frame) {
            SDL_SetJoystickVirtualButton(s_vpad[pad], (int)s_press[i].button, true);
            pc_log_line("pads: test press on virtual pad %d at frame %d", pad, frame);
        }
        else if (frame == s_press[i].frame + 2 && getenv("MELEE_PAD_PROMOTE_TRACE") != NULL) {
            SDL_Gamepad* g = SDL_GetGamepadFromID(SDL_GetJoystickID(s_vpad[pad]));
            pc_log_line("pads: test raw b%d=%d gamepad %p btn=%d player %d", (int)s_press[i].button,
                SDL_GetJoystickButton(s_vpad[pad], (int)s_press[i].button), (void*)g,
                g ? SDL_GetGamepadButton(g, s_press[i].button) : -1, g ? SDL_GetGamepadPlayerIndex(g) : -9);
        } else if (frame == s_press[i].frame + PRESS_FRAMES)
            SDL_SetJoystickVirtualButton(s_vpad[pad], (int)s_press[i].button, false);
    }
}

/* ---- per frame ------------------------------------------------------------------ */

void pc_pad_promote_frame(void) {
    static int enabled = -1;
    static int frame;
    if (enabled < 0) {
        const char* e = getenv("MELEE_PAD_PROMOTE");
#if defined(__ANDROID__)
        enabled = e == NULL || strcmp(e, "0") != 0;
#else
        enabled = e != NULL && strcmp(e, "1") == 0;
#endif
        test_vpads_init();
    }
    test_vpads_frame(frame++);
    if (!enabled || XR_ACTIVE())
        return;

    place_portless();
    SDL_JoystickID port_id[PAD_CHANMAX] = {0};
    bool used[PAD_CHANMAX] = {false}, edge[PAD_CHANMAX] = {false};
    for (u32 port = 0; port < PAD_CHANMAX; port++) {
        SDL_Gamepad* g = PADGetIndexForPort(port) >= 0 ? SDL_GetGamepadFromPlayerIndex((int)port) : NULL;
        if (g == NULL)
            continue;
        port_id[port] = SDL_GetGamepadID(g);
        used[port] = tracked_down(port_id[port], gamepad_down(g), &edge[port]);
    }
    static bool trace = false, trace_read = false;
    if (!trace_read) {
        trace_read = true;
        trace = getenv("MELEE_PAD_PROMOTE_TRACE") != NULL;
    }
    if (trace) {
        static char last[128];
        char now[128];
        snprintf(now, sizeof now, "ids %u/%u/%u/%u used %d%d%d%d edge %d%d%d%d fight %d", (unsigned)port_id[0],
            (unsigned)port_id[1], (unsigned)port_id[2], (unsigned)port_id[3], used[0], used[1], used[2], used[3],
            edge[0], edge[1], edge[2], edge[3], in_fight());
        if (strcmp(now, last) != 0) {
            pc_log_line("pads: promote %s", now);
            snprintf(last, sizeof last, "%s", now);
        }
    }
    if (port_id[0] != 0 && used[0])
        return; /* port 1's pad is in use: nothing moves */
    if (in_fight())
        return;
    for (u32 port = 1; port < PAD_CHANMAX; port++) {
        if (!edge[port])
            continue;
        const s32 mover = PADGetIndexForPort(port);
        const s32 idle = PADGetIndexForPort(0);
        char mover_name[64], idle_name[64];
        snprintf(mover_name, sizeof mover_name, "%s", port_name(port));
        snprintf(idle_name, sizeof idle_name, "%s", idle >= 0 ? port_name(0) : "no pad");
        PADSetPortForIndex((u32)mover, 0);
        if (idle >= 0)
            PADSetPortForIndex((u32)idle, port);
        pc_log_line("pads: '%s' pressed on port %u: now port 1; '%s' (never pressed) now port %u", mover_name,
            (unsigned)port + 1, idle_name, (unsigned)port + 1);
        char census[1536];
        pc_pad_census(census, sizeof census);
        pc_log_line("pads: now\n%s", census);
        return;
    }
}
