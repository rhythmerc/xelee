/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Positional sound effects (the launcher's Audio setting, on by default, or
 * MELEE_SPATIAL_AUDIO=0/1 at run time; built with the MELEE_SPATIAL_AUDIO
 * CMake option, which needs the Steam Audio SDK).
 *
 * A sound a fighter or an item starts is heard from where that fighter or
 * item is: in the XR build relative to the head, with the stage placed on
 * the table; elsewhere relative to the fight camera. A few stage sounds are
 * placed by a table in spatial_audio.c (Brinstar's lava). A stereo sound's
 * two voices (left only and right only, HSD_SynthSFXUpdateMix) each have
 * their own point: one for a fighter or an item, two apart for a stage
 * sound. The crowd (sound groups 5 and 6, sfx/crowdsfx.c) surrounds the
 * listener instead: its voices mix into a stereo bus heard from seats in a
 * ring, each a little later than the last. Music is not part of the
 * fight's world, so as VR games commonly do it stays mostly plain stereo:
 * 30% of its power comes from a stereo pair ahead of the stage and up, and
 * in the XR menus 30% of the whole mix from a pair beside the flat screen.
 * The announcer and every other sound are plain stereo, as are the reverb
 * busses.
 *
 * The path, in four steps:
 *   1. Emitter. The GObj proc loop (gobj.c) names the fighter or item whose
 *      proc is running; a sound start (axdriver.c) takes that emitter and
 *      its position and keeps them beside its sound machine record.
 *   2. Voice. When the audio thread gives the record AX voices
 *      (AXDriverExec), the voices are tagged with the emitter; acquiring or
 *      freeing a voice clears its tag.
 *   3. Positions. Once per drawn fight frame (cm/camera.c) the game thread
 *      publishes every fighter's and item's position, and the camera, into
 *      a snapshot the audio thread copies once per 5 ms AX frame.
 *   4. Render. A tagged voice is mixed mono (its L/R pan dropped, its level
 *      kept) and spatialized with an HRTF and a distance gain (Steam Audio's
 *      binaural and direct effects, at 48 kHz: resampled from and back to
 *      AX's 32 kHz) into the dry mix. Its aux sends (reverb) are unchanged.
 *
 * Output only. Nothing here is read by the simulation, nothing is in a
 * rollback snapshot (this file is not in libmelee_game.a, and axdriver.c's
 * statics are excluded from it), and none of it changes which sounds start
 * or when. Netplay: an emitter rides beside the sound start, never through
 * pc/net_sfx.c's handles; a start that net_sfx.c defers to the end of a
 * rollback finds its emitter by handle in a side table here. Resimulated
 * frames start no sounds and publish no positions, so a rollback leaves
 * playing voices with their tags and the latest published positions.
 *
 * Without the CMake option every call below is an empty inline. */
#ifndef PC_SPATIAL_AUDIO_H
#define PC_SPATIAL_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef MELEE_SPATIAL_AUDIO

/* The Audio setting (on by default), any time: it takes effect at the next
 * mix. MELEE_SPATIAL_AUDIO=0/1 overrides it (0: never made at all). */
void pc_spatial_set_enabled(bool on);

/* ---- game thread ---- */

/* Around one GObj proc (HSD_GObj_RunProcs): a fighter or item becomes the
 * current emitter, anything else (or NULL) clears it. */
void pc_spatial_proc(const void* gobj);
/* For the start of sound `sound_id` in sound group `group`: its emitter (the
 * crowd, the current fighter or item, or a placed stage sound) and where its
 * left and right voices are now (game units; the same point twice for a
 * fighter or an item). False, with *gobj NULL, when there is none or the
 * feature is off. */
bool pc_spatial_take_emitter(int sound_id, int group, const void** gobj, float pos[2][3]);
/* Netplay: the start that pc/net_sfx.c handed `handle` had an emitter;
 * remembered for a start it plays later (pc_spatial_handle_begin). */
void pc_spatial_note_handle(int32_t handle, int sound_id, int group);
/* Around a start pc/net_sfx.c plays at the end of a rollback: its emitter is
 * the one noted for `handle`. */
void pc_spatial_handle_begin(int32_t handle);
void pc_spatial_handle_end(void);
/* Big Blue (grbigblue.c), around the start of a car's engine loop: it
 * follows lane `lane`'s car of `part` (the cars' part) for the match, as
 * each new car takes the lane; (NULL, -1) after. */
void pc_spatial_stage_lane(const void* part, int lane);
/* Once per drawn fight frame, with the fight camera's view matrix (world ->
 * camera, GX Mtx layout): publishes positions and the listener. */
void pc_spatial_frame(const float view[3][4]);

/* ---- audio thread (pc/audio.c and AXDriverExec, under the audio lock) ---- */

/* AX voice `voice` now plays a sound from `gobj`, last seen at `pos`: the
 * sound's left voice (channel 0), its right (1), or its only one (-1, between
 * the two points). */
void pc_spatial_voice_tag(int voice, const void* gobj, const float pos[2][3], int channel);
void pc_spatial_voice_clear(int voice);
/* Once per AX frame, before the voices mix: takes the newest positions and
 * the listener. */
void pc_spatial_frame_begin(void);
/* Whether `voice` is rendered by pc_spatial_render this frame. */
bool pc_spatial_voice_active(int voice);
/* Spatializes the voice's dry mono frame (`mono`, 160 samples) into the
 * positional bus. */
void pc_spatial_render(int voice, const float* mono);
/* For a crowd voice this frame: the interleaved stereo bus (160 frames) to
 * mix it into, as it would mix into the output. NULL for any other voice. */
float* pc_spatial_voice_bus(int voice);
/* During a fight, after pc_spatial_frame_begin: the interleaved stereo bus
 * (160 frames) the music mixes into this frame, in place of the output;
 * pc_spatial_output adds it. NULL otherwise (music into the output). */
float* pc_spatial_music_bus(void);
/* Audio thread: the crowd's bus for sounds of the place itself (stage
 * ambience), heard from the same seats all around; NULL: mix them plain. */
float* pc_spatial_ambient_bus(void);
/* Spatial audio is working now: made, and on (the setting or
 * MELEE_SPATIAL_AUDIO). Any thread. */
bool pc_spatial_enabled(void);
/* Audio thread: a fight draws with spatial audio bypassed (the dev tools'
 * A/B): what would be placed is mixed plain. */
bool pc_spatial_bypassed(void);
/* Audio thread, during a fight after pc_spatial_frame_begin: a sound made
 * here (stage ambience), `mono` (160 samples at 32 kHz), heard from `pos`
 * (game units) as a voice is, through its own slot (0-7: one per source,
 * each keeping its HRTF state from frame to frame). False, doing nothing,
 * when nothing is placed this frame. */
bool pc_spatial_render_placed(int slot, const float pos[3], const float* mono);
/* Audio thread, after pc_spatial_frame_begin, in a fight or not (XR): a
 * sound placed in the room rather than the stage (the room light
 * calibration's chimes), `mono` as for pc_spatial_render_placed, heard from
 * `pos` (tracking space, metres) at its own level, through its own slot
 * (0-2). False, doing nothing, without a headset's head pose or with
 * spatial audio off: the caller mixes it plain. */
bool pc_spatial_render_room(int slot, const float pos[3], const float* mono);
/* Audio thread, during a fight after pc_spatial_frame_begin: how far `pos`
 * (game units) is from the listener, in game units (for Doppler). False
 * when nothing is placed this frame. */
bool pc_spatial_listener_distance(const float pos[3], float* units);
/* After the voices: adds the positional bus to the interleaved stereo mix
 * `out` (160 frames). */
void pc_spatial_frame_end(float* out);
/* The finished stereo frame `out` (160 frames, before the master volume):
 * adds the fight's music bus, partly from its speakers, and in the XR menus
 * plays a share of the whole mix from speakers beside the screen. Audio
 * thread, outside the audio lock. */
void pc_spatial_output(float* out);
/* Main thread, from AXInit: creates the Steam Audio objects when
 * spatial audio is on. */
void pc_spatial_init(int rate, int frame);

#else

static inline void pc_spatial_set_enabled(bool on) { (void)on; }
static inline void pc_spatial_proc(const void* gobj) { (void)gobj; }
static inline bool pc_spatial_take_emitter(int sound_id, int group, const void** gobj, float pos[2][3]) {
    (void)sound_id;
    (void)group;
    (void)pos;
    *gobj = 0;
    return false;
}
static inline void pc_spatial_note_handle(int32_t handle, int sound_id, int group) {
    (void)handle;
    (void)sound_id;
    (void)group;
}
static inline void pc_spatial_handle_begin(int32_t handle) { (void)handle; }
static inline void pc_spatial_handle_end(void) {}
static inline void pc_spatial_stage_lane(const void* part, int lane) {
    (void)part;
    (void)lane;
}
static inline void pc_spatial_frame(const float view[3][4]) { (void)view; }
static inline void pc_spatial_voice_tag(int voice, const void* gobj, const float pos[2][3], int channel) {
    (void)voice;
    (void)gobj;
    (void)pos;
    (void)channel;
}
static inline void pc_spatial_voice_clear(int voice) { (void)voice; }
static inline void pc_spatial_frame_begin(void) {}
static inline bool pc_spatial_voice_active(int voice) {
    (void)voice;
    return false;
}
static inline void pc_spatial_render(int voice, const float* mono) {
    (void)voice;
    (void)mono;
}
static inline float* pc_spatial_voice_bus(int voice) {
    (void)voice;
    return 0;
}
static inline float* pc_spatial_music_bus(void) { return 0; }
static inline float* pc_spatial_ambient_bus(void) { return 0; }
static inline bool pc_spatial_enabled(void) { return false; }
static inline bool pc_spatial_bypassed(void) { return false; }
static inline bool pc_spatial_render_placed(int slot, const float pos[3], const float* mono) {
    (void)slot;
    (void)pos;
    (void)mono;
    return false;
}
static inline bool pc_spatial_render_room(int slot, const float pos[3], const float* mono) {
    (void)slot;
    (void)pos;
    (void)mono;
    return false;
}
static inline bool pc_spatial_listener_distance(const float pos[3], float* units) {
    (void)pos;
    (void)units;
    return false;
}
static inline void pc_spatial_frame_end(float* out) { (void)out; }
static inline void pc_spatial_output(float* out) { (void)out; }
static inline void pc_spatial_init(int rate, int frame) {
    (void)rate;
    (void)frame;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
