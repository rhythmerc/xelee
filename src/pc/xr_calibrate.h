/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The room light calibration (XR build, Lighting on Roomtone).
 *
 * Once a launch, before the first fight mode, Unranked, Direct or LAN, the
 * player looks at the room's bright lights so Roomtone's capture sees them
 * (the camera sees less than passthrough shows, and only where the head
 * looks). It runs as the first part of the placement scene
 * (gm/gmonlinemode.c's ONLINE_KIND_PLACE, src/pc/xr_place.c), its stage
 * hidden until it ends: an intro card (A starts, B backs out), then a
 * strip that counts the lights as each rings (Aurora rings them, and they
 * chime from where they are, src/pc/room_chime.h), A finishing: "Room light
 * set, N lights found" for 1.5 s, or with none found a card to keep
 * looking or go on without (the dark room's own when the room reads dark).
 * B backs out at any point and nothing starts; the next entry point asks
 * again. Options > Graphics > Calibrate room light runs it again.
 *
 * It asks only with a headset presenting, Lighting on Roomtone and the
 * headset cameras allowed, and not again once done this launch (in memory
 * only). Presentation only: no sim, wire or timing change, and never in a
 * match. MELEE_XR_CALIBRATE=0 never asks. */
#ifndef PC_XR_CALIBRATE_H
#define PC_XR_CALIBRATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef AURORA_ENABLE_OPENXR
/* An entry point should run it first: a headset presenting, Roomtone, the
 * cameras allowed, not done this launch. */
bool pc_xr_calibrate_needed(void);
/* Done this launch. */
bool pc_xr_calibrated(void);
/* This launch's result for Graphics' row: -1 not done, else the lights. */
int pc_xr_calibrate_result(void);

/* xr_place.c, the placement scene's: starts it at the intro, `next` being
 * what the done card says comes after (a name in calibrate-layout.txt's
 * "next:" pieces: "PLACE THE STAGE", "VS. MODE"...). */
void pc_xr_calibrate_begin(const char* next);
/* Each pass while it runs, with the buttons pressed and let go during it
 * and the stick's or the D-pad's new push (-1 up, 1 down, 0 none): 0
 * running, 1 done (calibrated this launch), 2 backed out. */
int pc_xr_calibrate_pass(uint32_t released, int move);
/* The scene ended under it: off, not done. */
void pc_xr_calibrate_abort(void);
#else
static inline bool pc_xr_calibrate_needed(void) { return false; }
static inline bool pc_xr_calibrated(void) { return false; }
static inline int pc_xr_calibrate_result(void) { return -1; }
#endif

#ifdef __cplusplus
}
#endif

#endif
