/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/room_chime.h"

#ifdef AURORA_ENABLE_OPENXR

#include "pc/pc.h"
#include "pc/spatial_audio.h"
#include "pc/xr_place.h"

#include <SDL3/SDL.h>

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHIME_RATE 32000
#define CHIME_FRAME 160 /* one AX frame */
#define CHIME_STEPS 4   /* the lights' chimes; then Done */
#define CHIME_VOICES 3  /* pc_spatial_render_room's slots: a quick third light still rings */
/* Under the game's own sounds, which mix near full scale. */
#define CHIME_GAIN 0.5f

typedef struct Sound {
    float* pcm; /* mono, CHIME_RATE */
    int len;
} Sound;
static Sound s_sounds[CHIME_STEPS + 1];
static bool s_loaded;

typedef struct Voice {
    bool on, placed;
    int sound, at;
    float pos[3];
    uint64_t started;
} Voice;
static Voice s_voices[CHIME_VOICES];
static uint64_t s_starts;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;

/* A 16-bit mono WAV, at CHIME_RATE (another rate is resampled, linearly). */
static void load_one(Sound* out, const char* name) {
    size_t size = 0;
    void* data = pc_xr_load_file(name, &size);
    if (data == NULL) {
        return;
    }
    SDL_AudioSpec spec;
    Uint8* buf = NULL;
    Uint32 len = 0;
    SDL_IOStream* io = SDL_IOFromConstMem(data, size);
    if (io == NULL || !SDL_LoadWAV_IO(io, true, &spec, &buf, &len) || spec.format != SDL_AUDIO_S16 ||
        spec.channels != 1 || spec.freq <= 0)
    {
        pc_log_line("xr: calibration sound %s unreadable", name);
        SDL_free(buf);
        SDL_free(data);
        return;
    }
    const int16_t* src = (const int16_t*)buf;
    const int n = (int)(len / 2);
    const int m = (int)((int64_t)n * CHIME_RATE / spec.freq);
    float* pcm = malloc(sizeof(float) * (size_t)(m > 0 ? m : 1));
    for (int i = 0; i < m; i++) {
        const double t = (double)i * spec.freq / CHIME_RATE;
        const int k = (int)t;
        const float f = (float)(t - k);
        const float a = src[k] / 32768.f, b = k + 1 < n ? src[k + 1] / 32768.f : 0.f;
        pcm[i] = a + (b - a) * f;
    }
    out->pcm = pcm;
    out->len = m;
    SDL_free(buf);
    SDL_free(data);
}

void pc_room_chime_load(void) {
    if (s_loaded) {
        return;
    }
    s_loaded = true;
    for (int i = 0; i < CHIME_STEPS; i++) {
        char name[32];
        snprintf(name, sizeof name, "calibrate-chime-%d.wav", i + 1);
        load_one(&s_sounds[i], name);
    }
    load_one(&s_sounds[CHIME_STEPS], "calibrate-done.wav");
}

/* Into a free voice, else the oldest one. */
static void start(int sound, const float* pos) {
    if (s_sounds[sound].pcm == NULL) {
        return;
    }
    pthread_mutex_lock(&s_mutex);
    Voice* v = &s_voices[0];
    for (int i = 0; i < CHIME_VOICES; i++) {
        if (!s_voices[i].on) {
            v = &s_voices[i];
            break;
        }
        if (s_voices[i].started < v->started) {
            v = &s_voices[i];
        }
    }
    v->on = true;
    v->sound = sound;
    v->at = 0;
    v->placed = pos != NULL;
    if (pos != NULL) {
        memcpy(v->pos, pos, sizeof(v->pos));
    }
    v->started = ++s_starts;
    pthread_mutex_unlock(&s_mutex);
}

void pc_room_chime_light(int number, const float pos[3]) {
    start((number < 1 ? 1 : number > CHIME_STEPS ? CHIME_STEPS : number) - 1, pos);
}

void pc_room_chime_done(void) {
    start(CHIME_STEPS, NULL);
}

void pc_room_chime_mix(float* out, int frames) {
    if (frames != CHIME_FRAME) {
        return;
    }
    pthread_mutex_lock(&s_mutex);
    for (int i = 0; i < CHIME_VOICES; i++) {
        Voice* v = &s_voices[i];
        if (!v->on) {
            continue;
        }
        const Sound* snd = &s_sounds[v->sound];
        float mono[CHIME_FRAME];
        for (int k = 0; k < CHIME_FRAME; k++) {
            const int at = v->at + k;
            mono[k] = at < snd->len ? snd->pcm[at] * CHIME_GAIN : 0.f;
        }
        v->at += CHIME_FRAME;
        if (v->at >= snd->len) {
            v->on = false;
        }
        /* At the light; plain from the middle without spatial audio. */
        if (v->placed && pc_spatial_render_room(i, v->pos, mono)) {
            continue;
        }
        for (int k = 0; k < CHIME_FRAME; k++) {
            out[k * 2] += mono[k];
            out[k * 2 + 1] += mono[k];
        }
    }
    pthread_mutex_unlock(&s_mutex);
}

#endif
