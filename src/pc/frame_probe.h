/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MELEE_FRAME_PROBE=1: where a slow game frame's time goes. The scene loop
 * (gm_801A4D34) marks the frame's start, the end of its simulation ticks
 * and the end of its draws; the GObj loops time each update proc and
 * render callback. A frame over MELEE_FRAME_PROBE_MS (11) from start to
 * drawn logs its split and the slowest update and draw, as libmelee
 * offsets (addr2line -f -e libmelee.so). Off: nothing is timed. */
#ifndef PC_FRAME_PROBE_H
#define PC_FRAME_PROBE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

extern bool pc_frame_probe_on;

void pc_frame_probe_begin(void);
/* The pad alarm's sample is in: the ticks start. */
void pc_frame_probe_ready(void);
void pc_frame_probe_sim_end(int ticks);
void pc_frame_probe_drawn(void);
/* Around one update proc or render callback: `fn` and its p_link or gx
 * link. */
unsigned long long pc_frame_probe_now(void);
void pc_frame_probe_proc(const void* fn, int link, unsigned long long start);
void pc_frame_probe_render(const void* fn, int link, unsigned long long start);
/* The ticks' audio update (lbAudioAx_80027DF8, the stage ambience), since
 * `start`. */
void pc_frame_probe_audio(unsigned long long start);

#ifdef __cplusplus
}
#endif

#endif
