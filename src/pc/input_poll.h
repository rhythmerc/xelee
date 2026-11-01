/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_INPUT_POLL_H
#define PC_INPUT_POLL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the dedicated 1000 Hz (1 ms) input polling worker thread.
 * Polls physical and virtual controller inputs independently of the 60 Hz frame boundary,
 * eliminating input quantization and latency jitter. */
void pc_input_poll_init(void);

/* Stop and join the 1000 Hz input polling worker thread. */
void pc_input_poll_shutdown(void);

/* Query whether the 1000 Hz input polling thread is active. */
bool pc_input_poll_is_running(void);

/* Query total count of 1000 Hz poll iterations completed. */
uint64_t pc_input_poll_get_count(void);

/* Smoothed poll-thread rate (Hz) for the diagnostic HUD. */
float pc_input_poll_hz(void);

/* Every joystick SDL sees (gamepad or not, opened or skipped) with its SDL
 * player slot, then which pad each game port reads: a line each into `out`.
 * For the log when pads come and go and for Send logs, to tell why a lone
 * pad isn't on port 1. Returns the length written. */
size_t pc_pad_census(char* out, size_t cap);

/* Per frame, after the events (pad_promote.c): on Android, the first pad to
 * press a button takes port 1 from a pad that never has. */
void pc_pad_promote_frame(void);

#ifdef __cplusplus
}
#endif

#endif /* PC_INPUT_POLL_H */
