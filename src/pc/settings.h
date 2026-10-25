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

typedef struct PcGraphicsSettings {
    /* A screen (desktop, phone). */
    bool fullscreen, vsync;
    int render_scale;    /* 0 auto (the window), else 1..10x */
    int widescreen;      /* 0 original 73:60, 1 16:9, 2 the window's */
    int hud_mode;        /* 0 classic 4:3, 1 wide */
    int msaa;            /* 1 off, 4 (at launch) */
    int filter_mode;     /* 0..3, pc_settings_filter_name */
    bool fps;
    int backend;         /* pc_settings_backend_name; at launch */
    /* Both. */
    bool custom_textures;
    /* The Quest; as it runs but for SpaceWarp and foveation (at launch). */
    int xr_resolution;   /* 0 dynamic, else 80..130 (%) */
    bool xr_spacewarp;
    int xr_foveation;    /* 0..3 */
    bool xr_shadow;
    int xr_lighting;     /* 0 vanilla, 1 Roomtone (needs the headset cameras) */
} PcGraphicsSettings;

void pc_settings_graphics(PcGraphicsSettings* out);
/* Applies what changed (what can change as it runs) and saves. */
void pc_settings_set_graphics(const PcGraphicsSettings* s);
void pc_settings_graphics_defaults(PcGraphicsSettings* out);
/* The Quest's rows rather than a screen's (the launcher's platform). */
bool pc_settings_quest(void);
/* The settings as the game started with them, for what applies at launch. */
void pc_settings_graphics_at_launch(PcGraphicsSettings* out);
const char* pc_settings_filter_name(int mode);
const char* pc_settings_backend_name(int backend);
/* The next backend this platform offers, wrapping to Auto. */
int pc_settings_backend_next(int backend);
typedef struct PcGameplaySettings {
    bool unlock_all;     /* every character, stage and All-Star */
    bool frozen_stadium; /* Pokemon Stadium with no transformations */
    bool free_camera;    /* the pause camera's rotation and zoom (not the Quest) */
    bool ucf, slippi_fixes, cstick_1p;
    bool place_hold;     /* XR: each fight's first frame held to place the stage */
} PcGameplaySettings;

void pc_settings_gameplay(PcGameplaySettings* out);
/* Saves; each is read as the game runs. */
void pc_settings_set_gameplay(const PcGameplaySettings* s);
void pc_settings_gameplay_defaults(PcGameplaySettings* out);

/* The headset cameras, for Roomtone: allowed; ask (the system's dialog);
 * the answer, -1 while none (0 refused, 1 allowed, 2 won't ask again). Off
 * Android, allowed. */
bool pc_settings_camera_granted(void);
void pc_settings_camera_request(void);
int pc_settings_camera_result(void);

#ifdef __cplusplus
}
#endif

#endif
