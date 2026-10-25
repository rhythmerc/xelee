/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The room light calibration's sounds (XR build, src/pc/xr_calibrate.c):
 * a light's chime, heard from where the light is in the room (Steam Audio,
 * pc_spatial_render_room), or plain from the middle without spatial audio;
 * and the Done card's sound, from the middle. Mono assets in resources/xr
 * at AX's 32 kHz (tools/xr_cards/make_calibration_sounds.py): one chime a
 * pentatonic step up for each of the first four lights, the fourth's for
 * the rest. No reverb. Output only. */
#ifndef PC_ROOM_CHIME_H
#define PC_ROOM_CHIME_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef AURORA_ENABLE_OPENXR
/* Game thread: the sounds read in (once; quiet if they aren't there). */
void pc_room_chime_load(void);
/* Game thread: light `number`'s chime (1 on), from `pos` (tracking space,
 * metres); the Done sound. */
void pc_room_chime_light(int number, const float pos[3]);
void pc_room_chime_done(void);
/* Audio thread, after pc_spatial_frame_begin and before
 * pc_spatial_frame_end: the sounds playing, into the interleaved stereo
 * mix `out` (`frames` at 32 kHz) or the positional bus. */
void pc_room_chime_mix(float* out, int frames);
#else
static inline void pc_room_chime_mix(float* out, int frames) {
    (void)out;
    (void)frames;
}
#endif

#ifdef __cplusplus
}
#endif

#endif
