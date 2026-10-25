/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The settings the in-game Options pages change (src/pc/audio_menu.h). They
 * are the launcher's preferences (src/pc/launcher_data.hpp), so a change
 * here is live at once, saved, and what the launcher shows next time. */
#ifndef PC_SETTINGS_H
#define PC_SETTINGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PcAudioSettings {
    float volume, music, sfx; /* 0..1 */
    bool mute, reverb;
    bool spatial;      /* fighters' and items' sounds placed */
    bool stage_sounds; /* our stage ambience; needs spatial */
} PcAudioSettings;

void pc_settings_audio(PcAudioSettings* out);
/* Applies and saves. */
void pc_settings_set_audio(const PcAudioSettings* s);
/* What a new install has. */
void pc_settings_audio_defaults(PcAudioSettings* out);
/* Whether this build has spatial audio at all (MELEE_SPATIAL_AUDIO). */
bool pc_settings_spatial_built(void);

#ifdef __cplusplus
}
#endif

#endif
