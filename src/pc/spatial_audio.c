/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Positional sound effects; see spatial_audio.h for the whole path. */
#include "compat.h"
#include "pc/spatial_audio.h"

#ifdef MELEE_SPATIAL_AUDIO

#include "pc/pc.h"
#include "pc/stage_ambience.h"
#include "pc/stage_mix.h"
#include "pc/xr_scene.h"
#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif
#include <melee/ft/forward.h>
#include <melee/ft/ftlib.h>
#include <melee/gr/ground.h>
#include <melee/gr/grbigblue.h>
#include <melee/it/forward.h>
/* it/it_26B1.h, declared here: its header pulls in every fighter type. */
void it_8026B294(HSD_GObj* gobj, Vec3* pos);
ItemKind itGetKind(HSD_GObj* gobj);
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/jobj.h>

#include <phonon.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SP_VOICES 64       /* AX_VOICES */
#define SP_PLACED 8        /* sounds made here, at points of the stage (pc_spatial_render_placed) */
#define SP_ROOM 3          /* sounds placed in the room, not the stage (pc_spatial_render_room) */
#define SP_EMITTERS 64     /* fighters and items published per frame */
#define SP_HANDLES 256     /* netplay starts remembered by handle */
#define SP_STALE_NS 250000000ull /* positions older than this: back to plain stereo */
#define SP_INSTANCES 32    /* stage sounds placed at the part that started them */
#define SP_INSTANCE_NS 8000000000ull /* followed this long (an Arwing's pass) */
#define SP_SHOT_NS 500000000ull      /* a start's shot appears within this */

/* A fighter's position is at its feet; its sounds come from about the
 * middle of its body (the same lift pc/xr_scene.c gives the follow focus). */
#define SP_FIGHTER_LIFT 10.0f
/* Desktop: the fight camera is the listener, and its game units are taken
 * at the XR arena's default size (AURORA_XR_ARENA_SCALE), so a fight seen
 * from the usual camera distance sounds about as far as the tabletop stage
 * does from a seated player. */
#define SP_DESKTOP_SCALE 0.0035f
/* Distance gain: full within one stage width (170 game units, Final
 * Destination's main platform, in meters at the arena's current scale),
 * then a gentle 1 / (1 + k (d / ref - 1)) rolloff with a floor, so a
 * fighter at the far edge of a table stage is a little quieter, never lost.
 * Scaling ref with the arena keeps the balance the same at any table size. */
#define SP_REF_UNITS 170.0f
/* The crowd (sound groups 5 and 6, sfx/crowdsfx.c): heard from seats in a
 * ring around the listener, fixed to the stage's axes, a little above ear
 * height; its bus's left channel feeds the seats on the left, its right
 * those on the right. Each seat hears the bus a little later than the last
 * (milliseconds), as a stadium's stands would, and through its own
 * decorrelator (SP_SEAT_TAPS), or the same sound from six directions would
 * fold back to the middle of the head. Delays alone made it ring: copies
 * 12 ms apart summed at each ear into a comb (peaks every 83 Hz, nulls
 * between), "metallic and harsh". */
#define SP_CROWD_SEATS 6
#define SP_CROWD_ELEVATION 20.0f /* degrees */
#define SP_CROWD_DELAY 4096      /* delay line, 48 kHz samples (85 ms); a power of two */
static const struct {
    float azimuth; /* degrees from straight at the stage; negative: left */
    int side;      /* the bus channel it plays: 0 left, 1 right */
    float delay_ms;
} s_seats[SP_CROWD_SEATS] = {
    {-30.f, 0, 5.f}, {-90.f, 0, 17.f}, {-150.f, 0, 29.f},
    {30.f, 1, 11.f}, {90.f, 1, 23.f},  {150.f, 1, 37.f},
};
/* Speakers for music, which is not part of the fight's world: as VR games
 * commonly do, most of it stays plain stereo and a share comes from a
 * stereo pair out in the room (SP_PLACED_SHARE of the power). Fight music's
 * pair stands ahead of the stage and up, fixed to its axes like the crowd's
 * seats; in the XR menus the whole mix's pair stands beside the screen,
 * SP_SPEAKER_GAP meters out from its sides. */
#define SP_PLACED_SHARE 0.3f
#define SP_MUSIC_AZIMUTH 30.0f   /* degrees either side of straight at the stage */
#define SP_MUSIC_ELEVATION 15.0f
#define SP_SPEAKER_GAP 0.10f
/* Fight music under spatial audio, in dB: placed sounds, the crowd and the
 * stage's ambience need room the plain mix never gave them. -4.4 is 60%;
 * -9 (the player's first pick by ear, 30%) cut it more than needed.
 * MELEE_SPATIAL_MUSIC_GAIN overrides. */
#define SP_FIGHT_MUSIC_DB (-4.4f)
/* Menus (no fight spatializing) under spatial audio, the whole mix in dB:
 * the fight's music sits 4.4 dB down for its placed sounds, so menus at
 * full came out louder than play (a flat desktop mix: character select
 * -18.9 dBFS, a fight's music and stage -23.6), and on the headset "much
 * louder". The same trim puts their music level with the fight's.
 * MELEE_MENU_GAIN overrides. Glides over SP_MENU_GLIDE_S. */
#define SP_MENU_DB (-4.4f)
#define SP_MENU_GLIDE_S 0.5f
/* Frames (5 ms) to cross between plain stereo and a speaker blend. */
#define SP_SPEAKER_FADE 10
#define SP_ROLLOFF 0.5f
#define SP_MIN_GAIN 0.3f
/* The built-in HRTF (RMS-normalized) comes out about 1.2 dB under the same
 * voice mixed center-panned, measured over fights with
 * MELEE_SPATIAL_AUDIO_STATS; made up so an A/B compares placement, not level. */
#define SP_MAKEUP 1.15f

static int s_forced = -1;  /* MELEE_SPATIAL_AUDIO: 1 on, 0 off, -1 the setting */
static bool s_pref = true; /* the Audio setting; read on any thread (__atomic) */
static bool s_ready;       /* audio thread: the Steam Audio objects are made */

void pc_spatial_set_enabled(bool on) {
    __atomic_store_n(&s_pref, on, __ATOMIC_RELAXED);
}

static int forced(void) {
    if (s_forced == -1) {
        const char* v = getenv("MELEE_SPATIAL_AUDIO");
        s_forced = v != NULL && v[0] != '\0' ? v[0] != '0' : -2;
    }
    return s_forced;
}

/* Whether spatial audio can be on this run: the Steam Audio objects are made
 * at the first sound (AXInit) unless MELEE_SPATIAL_AUDIO=0, so the setting
 * can turn it on and off as it plays. Emitters are tagged either way. */
static bool enabled(void) {
    return forced() != 0;
}

/* Whether it's on now: MELEE_SPATIAL_AUDIO, else the setting. Off, voices
 * mix plain and the menus as without it, as if it were never made. */
static bool on_now(void) {
    const int f = forced();
    return f >= 0 ? f != 0 : __atomic_load_n(&s_pref, __ATOMIC_RELAXED);
}

bool pc_spatial_enabled(void) {
    return __atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) && on_now();
}

static int stats_on(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("MELEE_SPATIAL_AUDIO_STATS") != NULL;
    }
    return cached;
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- game thread ------------------------------------------------------ */

static const HSD_GObj* s_cur;      /* the fighter or item whose proc runs */
static const HSD_GObj* s_cur_part; /* or the stage part */
static const HSD_GObj* s_lane_part; /* pc_spatial_stage_lane, around a start */
static int s_lane = -1;
static bool s_override_on;         /* a deferred netplay start: its noted emitter */
static const void* s_override_gobj;
static float s_override_pos[2][3];
static struct {
    int32_t handle;
    const void* gobj;
    float pos[2][3];
} s_handles[SP_HANDLES];
static unsigned s_handle_next;
static unsigned s_starts_tagged, s_starts_plain, s_starts_crowd, s_late_found, s_late_missed; /* stats */

void pc_spatial_stage_lane(const void* part, int lane) {
    s_lane_part = part;
    s_lane = part != NULL ? lane : -1;
}

void pc_spatial_proc(const void* gobj) {
    const HSD_GObj* g = gobj;
    if (!enabled()) {
        return;
    }
    s_cur = g != NULL && (g->classifier == HSD_GOBJ_CLASS_FIGHTER || g->classifier == HSD_GOBJ_CLASS_ITEM) ? g
                                                                                                          : NULL;
    s_cur_part = g != NULL && g->classifier == HSD_GOBJ_CLASS_STAGE ? g : NULL;
}

/* Where an emitter is now; read only. False for anything else. */
static bool emitter_pos(const HSD_GObj* g, float pos[3]) {
    Vec3 p;
    if (g == NULL || g->user_data == NULL) {
        return false;
    }
    if (g->classifier == HSD_GOBJ_CLASS_FIGHTER) {
        ftLib_GetPos((HSD_GObj*)g, &p);
        p.y += SP_FIGHTER_LIFT;
    } else if (g->classifier == HSD_GOBJ_CLASS_ITEM) {
        it_8026B294((HSD_GObj*)g, &p);
    } else {
        return false;
    }
    if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) {
        return false;
    }
    pos[0] = p.x;
    pos[1] = p.y;
    pos[2] = p.z;
    return true;
}

/* Stage sounds, placed. A stereo one's left and right voices each get a
 * point (x[0] and x[1]); a mono one sits between them. The points are in
 * game units, or in a stage part's joint as it was last drawn: a part of the
 * map (part), or the part whose proc started the sound (from_proc, one
 * placement per start, followed as that part moves). Read only, like the
 * rest: the lava's height is the game's own. */
typedef struct StageSound {
    int grkind;
    int sfx[4];     /* the sounds placed so (0: unused) */
    float x[2];     /* left voice, right voice */
    float z;
    bool lava_y;    /* y follows the lava's surface (Ground_801C4368) */
    float y;        /* otherwise fixed */
    float gain;     /* over the game's own level */
    int part;       /* > 0: in this map part's joint */
    bool from_proc; /* in the starting part's joint */
    int joint;      /* depth first from the part's root, as pc/xr_scene.c counts */
    bool to_shot;   /* then with the Arwing laser the start fires, once it exists */
    bool lane;      /* with the car of the lane it starts for (pc_spatial_stage_lane) */
    int mix;        /* its stage mixer channel (pc/stage_mix.h) */
} StageSound;

static const StageSound s_stage_sounds[] = {
    /* Brinstar: the acid's stereo rumble as it starts to rise (0x61A80) or
     * fall (0x61A81). Its channels a third and two thirds of the way across
     * the stretch of acid the 3D view keeps (x -120 to 125, pc/xr_scene.c),
     * at its surface (where it burns: the acid joint less 55, grzebes.c).
     * Followed below where the 3D view shows it too (a rise starts about 300
     * under the stage): from there it is a cue to where the acid is, a little
     * quieter by the distance gain alone. The rumble is quiet in the game's
     * own mix (-30 to -50 dB, about 2.5 s), and lost under the stage, so it
     * is played 7 dB up, about 8 dB under busy fighters over a fight, with
     * the acid's own simmer (pc/stage_ambience.c) between. */
    {.grkind = 0x08,
     .sfx = {0x61A80, 0x61A81},
     .x = {-120.f + 245.f / 3.f, -120.f + 245.f * 2.f / 3.f},
     .lava_y = true,
     .gain = 2.25f,
     .mix = STAGE_MIX_ACID_RUMBLE},
    /* Corneria (grcorneria.c). The Great Fox (part 3) flies nose to the
     * left. Its engine hum, a stereo loop from the stage's start whose right
     * channel runs about 9 dB over its left (as plain stereo it sat in the
     * right ear), comes from its tail: x 233-262 at rest, drawn at 0.54
     * scale as the ship drifts. */
    {.grkind = 0x0E, .sfx = {0x55737}, .x = {245.f, 245.f}, .y = 15.f, .gain = 1.f, .part = 3, .joint = 0,
     .mix = STAGE_MIX_GREAT_FOX},
    /* Its cannon under the nose (joints 3 and 4, 10 units apart): the
     * charge, one beam (0x55739) or a volley (0x5573A), and each shot. */
    {.grkind = 0x0E, .sfx = {0x55739, 0x5573A, 0x55735}, .gain = 1.f, .part = 3, .joint = 3,
     .mix = STAGE_MIX_GREAT_FOX},
    /* The Arwings (parts 1, 2 and 10; joint 3 carries the ship): the swoops
     * as one comes and goes, heard from it all the way past. */
    {.grkind = 0x0E, .sfx = {0x55730, 0x55731, 0x55732}, .gain = 1.f, .from_proc = true, .joint = 3,
     .mix = STAGE_MIX_ARWINGS},
    /* An Arwing's laser shot: from the Arwing, then with the laser it makes
     * just after starting the sound (itarwinglaser.c) as it flies. */
    {.grkind = 0x0E, .sfx = {0x55733, 0x55734}, .gain = 1.f, .from_proc = true, .joint = 3, .to_shot = true,
     .mix = STAGE_MIX_ARWINGS},
    /* Big Blue (grbigblue.c): the racers' engines, loops Melee starts as a
     * car takes one of the track's four lanes (four as the stage loads) and
     * never stops (a later one in the same sound slot replaces it). Plain,
     * they were a steady bed in both ears (-35 dBFS); each now rides its
     * lane's car, past the player and on as the lane takes new ones. Placed
     * they came to -42 in a flat desktop mix, the cars mostly away to the
     * sides. On the headset they were barely heard under the rush of air:
     * 9 dB up, the racers being the point. */
    {.grkind = 0x13,
     .sfx = {0x6DDD2, 0x6DDD3, 0x6DDD4, 0x6DDD5},
     .gain = 2.8f,
     .lane = true,
     .mix = STAGE_MIX_ENGINES},
};
#define SP_STAGE_SOUNDS ((int)(sizeof s_stage_sounds / sizeof s_stage_sounds[0]))

/* One placement of a from_proc sound: its own emitter. */
typedef struct StageInstance {
    const StageSound* r;
    const HSD_GObj* part; /* the part that started it */
    const HSD_GObj* shot; /* to_shot: its laser, once matched */
    bool waiting;         /* to_shot: no laser yet */
    bool lane_on;         /* lane: follows `lane`'s car of `part` while it lives */
    int lane;
    uint64_t stamp;       /* now_ns() at the start */
    float pos[2][3];      /* where it was last */
} StageInstance;

static StageInstance s_inst[SP_INSTANCES];
static unsigned s_inst_next;

static bool live(const HSD_GObj* g, int plink) {
    for (HSD_GObj* h = HSD_GObjPLinkHead[plink]; h != NULL && g != NULL; h = h->next) {
        if (h == g) {
            return true;
        }
    }
    return false;
}

/* A part's joint, counted as pc/xr_scene.c's nth_joint does. */
static HSD_JObj* nth_joint(HSD_JObj* jobj, int* n) {
    for (; jobj != NULL; jobj = jobj->next) {
        if ((*n)-- == 0) {
            return jobj;
        }
        HSD_JObj* found = nth_joint(jobj->child, n);
        if (found != NULL) {
            return found;
        }
    }
    return NULL;
}

/* The points, in the part's joint as last drawn. False: no such joint. */
static bool joint_points(const HSD_GObj* g, int joint, const float local[2][3], float pos[2][3]) {
    HSD_JObj* root = g != NULL ? g->hsd_obj : NULL;
    int n = joint - 1;
    HSD_JObj* j = root == NULL ? NULL : joint == 0 ? root : nth_joint(root->child, &n);
    if (j == NULL) {
        return false;
    }
    for (int c = 0; c < 2; c++) {
        float p[3];
        for (int k = 0; k < 3; k++) {
            p[k] = j->mtx[k][0] * local[c][0] + j->mtx[k][1] * local[c][1] + j->mtx[k][2] * local[c][2] + j->mtx[k][3];
        }
        if (!isfinite(p[0]) || !isfinite(p[1]) || !isfinite(p[2])) {
            return false;
        }
        memcpy(pos[c], p, sizeof(p));
    }
    return true;
}

static void local_points(const StageSound* r, float pos[2][3]) {
    float y = r->y;
    if (r->lava_y) {
        float speed;
        Ground_801C4368(&y, &speed);
    }
    for (int c = 0; c < 2; c++) {
        pos[c][0] = r->x[c];
        pos[c][1] = y;
        pos[c][2] = r->z;
    }
}

/* A map-wide one's points now (not a from_proc one). */
static void stage_sound_pos(const StageSound* r, float pos[2][3]) {
    float local[2][3];
    local_points(r, local);
    if (r->part <= 0 || !joint_points(Ground_GetMapGObj(r->part), r->joint, local, pos)) {
        memcpy(pos, local, sizeof(local)); /* or the part is gone: where it rests */
    }
}

/* An instance's points now; where they were last once its part or laser is gone. */
static void instance_pos(StageInstance* in) {
    float p[3];
    if (in->lane_on) {
        if (live(in->part, HSD_GOBJ_PLINK_GROUND) && grBigBlue_pc_lane_pos(in->part, in->lane, p)) {
            memcpy(in->pos[0], p, sizeof(p));
            memcpy(in->pos[1], p, sizeof(p));
        }
    } else if (in->shot != NULL && live(in->shot, HSD_GOBJ_PLINK_ITEM) && emitter_pos(in->shot, p)) {
        memcpy(in->pos[0], p, sizeof(p));
        memcpy(in->pos[1], p, sizeof(p));
    } else if (in->shot == NULL && live(in->part, HSD_GOBJ_PLINK_GROUND)) {
        float local[2][3], pos[2][3];
        local_points(in->r, local);
        if (joint_points(in->part, in->r->joint, local, pos)) {
            memcpy(in->pos, pos, sizeof(pos));
        }
    }
}

static const StageSound* stage_sound(int sound_id) {
    for (int i = 0; i < SP_STAGE_SOUNDS; i++) {
        const StageSound* r = &s_stage_sounds[i];
        if (r->grkind != (int)stage_info.grkind) {
            continue;
        }
        for (int k = 0; k < 4 && r->sfx[k] != 0; k++) {
            if (r->sfx[k] == sound_id) {
                return r;
            }
        }
    }
    return NULL;
}

/* The crowd's emitter: it has no point (pc_spatial_voice_bus). */
static const char s_crowd_tag;
#define SP_CROWD ((const void*)&s_crowd_tag)

/* A placed stage sound's entry, from its emitter; NULL for anything else. */
static const StageSound* stage_entry(const void* gobj) {
    const StageSound* r = (const StageSound*)gobj;
    if (r >= s_stage_sounds && r < s_stage_sounds + SP_STAGE_SOUNDS) {
        return r;
    }
    const StageInstance* in = (const StageInstance*)gobj;
    return in >= s_inst && in < s_inst + SP_INSTANCES ? in->r : NULL;
}

/* The emitter of a sound starting now, and its two points. */
static const void* find_emitter(int sound_id, int group, float pos[2][3]) {
    if (group == 5 || group == 6) { /* the crowd, though a fighter's proc starts it */
        memset(pos, 0, sizeof(float) * 6);
        return SP_CROWD;
    }
    if (s_cur != NULL && emitter_pos(s_cur, pos[0])) {
        memcpy(pos[1], pos[0], sizeof(pos[0]));
        return s_cur;
    }
    const StageSound* r = stage_sound(sound_id);
    if (r == NULL) {
        return NULL;
    }
    if (r->lane) {
        if (s_lane < 0 || s_lane_part == NULL) {
            return NULL;
        }
        /* One emitter a lane, whichever of its loops are playing. */
        for (int i = 0; i < SP_INSTANCES; i++) {
            StageInstance* in = &s_inst[i];
            if (in->r == r && in->lane_on && in->part == s_lane_part && in->lane == s_lane) {
                instance_pos(in);
                memcpy(pos, in->pos, sizeof(in->pos));
                return in;
            }
        }
        StageInstance* in = &s_inst[s_inst_next++ % SP_INSTANCES];
        memset(in, 0, sizeof(*in));
        in->r = r;
        in->part = s_lane_part;
        in->lane_on = true;
        in->lane = s_lane;
        in->stamp = now_ns();
        instance_pos(in);
        memcpy(pos, in->pos, sizeof(in->pos));
        return in;
    }
    if (!r->from_proc) {
        stage_sound_pos(r, pos);
        return r;
    }
    if (s_cur_part == NULL) {
        return NULL; /* not from a part's proc: plain */
    }
    StageInstance* in = &s_inst[s_inst_next++ % SP_INSTANCES];
    memset(in, 0, sizeof(*in));
    in->r = r;
    in->part = s_cur_part;
    in->waiting = r->to_shot;
    in->stamp = now_ns();
    instance_pos(in);
    memcpy(pos, in->pos, sizeof(in->pos));
    return in;
}

bool pc_spatial_take_emitter(int sound_id, int group, const void** gobj, float pos[2][3]) {
    *gobj = NULL;
    if (!enabled()) {
        return false;
    }
    if (s_override_on) {
        *gobj = s_override_gobj;
        memcpy(pos, s_override_pos, sizeof(s_override_pos));
    } else {
        *gobj = find_emitter(sound_id, group, pos);
    }
    if (*gobj == SP_CROWD) {
        s_starts_crowd++;
    }
    if (*gobj != NULL) {
        s_starts_tagged++;
    } else {
        s_starts_plain++;
    }
    return *gobj != NULL;
}

void pc_spatial_note_handle(int32_t handle, int sound_id, int group) {
    float pos[2][3];
    const void* gobj;
    if (!enabled() || (gobj = find_emitter(sound_id, group, pos)) == NULL) {
        return;
    }
    unsigned i = s_handle_next++ % SP_HANDLES;
    s_handles[i].handle = handle;
    s_handles[i].gobj = gobj;
    memcpy(s_handles[i].pos, pos, sizeof(pos));
}

void pc_spatial_handle_begin(int32_t handle) {
    if (!enabled()) {
        return;
    }
    /* Newest first: a handle is a frame and a call index, so an old entry
     * for the same value is a frame NET_SFX_FRAMES-plus ago at the least. */
    for (unsigned k = 1; k <= SP_HANDLES; k++) {
        unsigned i = (s_handle_next - k) % SP_HANDLES;
        if (s_handles[i].gobj != NULL && s_handles[i].handle == handle) {
            s_override_on = true;
            s_override_gobj = s_handles[i].gobj;
            memcpy(s_override_pos, s_handles[i].pos, sizeof(s_override_pos));
            s_late_found++;
            return;
        }
    }
    s_late_missed++; /* started with no emitter (or noted too long ago) */
}

void pc_spatial_handle_end(void) {
    s_override_on = false;
}

/* What the game thread hands the audio thread once per drawn fight frame. */
typedef struct Snapshot {
    uint64_t stamp; /* now_ns() when published; 0: never */
    bool xr3d;      /* the fight is in 3D in a headset: the head is the listener */
    bool bypass;    /* spatial audio bypassed (dev tools): every voice plain stereo */
    bool off;       /* the setting off: every voice plain, no stage sounds */
    float view[3][4];
    int n;
    struct {
        const void* gobj;
        float pos[2][3]; /* left and right voices (the same for a fighter or an item) */
    } e[SP_EMITTERS];
    int fighters, items; /* stats */
} Snapshot;

static pthread_mutex_t s_snap_mutex = PTHREAD_MUTEX_INITIALIZER;
static Snapshot s_snap;

static void collect(Snapshot* s, int plink, int* count) {
    for (HSD_GObj* g = HSD_GObjPLinkHead[plink]; g != NULL && s->n < SP_EMITTERS; g = g->next) {
        if (emitter_pos(g, s->e[s->n].pos[0])) {
            memcpy(s->e[s->n].pos[1], s->e[s->n].pos[0], sizeof(s->e[s->n].pos[0]));
            s->e[s->n].gobj = g;
            s->n++;
            (*count)++;
        }
    }
}

/* Arwing lasers new since the last drawn frame go, oldest first, to the
 * laser sounds started just before them that wait for one. */
static void match_shots(void) {
    static const HSD_GObj* seen[16];
    static int n_seen;
    const HSD_GObj* now[16];
    int n = 0;
    for (HSD_GObj* g = HSD_GObjPLinkHead[HSD_GOBJ_PLINK_ITEM]; g != NULL && n < 16; g = g->next) {
        if (g->user_data != NULL && itGetKind(g) == It_Kind_Arwing_Laser) {
            now[n++] = g;
        }
    }
    uint64_t t = now_ns();
    for (int i = 0; i < n; i++) {
        bool old = false;
        for (int k = 0; k < n_seen && !old; k++) {
            old = seen[k] == now[i];
        }
        if (old) {
            continue;
        }
        StageInstance* best = NULL;
        for (int k = 0; k < SP_INSTANCES; k++) {
            StageInstance* in = &s_inst[k];
            if (in->waiting && t - in->stamp < SP_SHOT_NS && (best == NULL || in->stamp < best->stamp)) {
                best = in;
            }
        }
        if (best != NULL) {
            best->shot = now[i];
            best->waiting = false;
        }
    }
    memcpy(seen, now, sizeof(now[0]) * (size_t)n);
    n_seen = n;
}

/* This stage's placed sounds, which move with the lava, the ship or a laser. */
static void collect_stage(Snapshot* s) {
    for (int i = 0; i < SP_STAGE_SOUNDS && s->n < SP_EMITTERS; i++) {
        const StageSound* r = &s_stage_sounds[i];
        if (r->grkind == (int)stage_info.grkind && !r->from_proc) {
            stage_sound_pos(r, s->e[s->n].pos);
            s->e[s->n].gobj = r;
            s->n++;
        }
    }
    match_shots();
    uint64_t t = now_ns();
    for (int i = 0; i < SP_INSTANCES && s->n < SP_EMITTERS; i++) {
        StageInstance* in = &s_inst[i];
        if (in->r != NULL && (in->lane_on ? live(in->part, HSD_GOBJ_PLINK_GROUND) : t - in->stamp < SP_INSTANCE_NS)) {
            instance_pos(in);
            memcpy(s->e[s->n].pos, in->pos, sizeof(in->pos));
            s->e[s->n].gobj = in;
            s->n++;
        }
    }
}

/* The dev tools panel's audio settings (aurora_xr_dev_audio): spatial audio
 * bypassed for an A/B (every voice as it would be without it), returned,
 * and the music's level, handed to the mixer. Game thread. */
static bool dev_bypass(void) {
#ifdef AURORA_ENABLE_OPENXR
    static bool published;
    if (!published) {
        const AuroraXrDevAudio d = {.spatial = true, .music = 1.f};
        aurora_xr_dev_audio_defaults(&d);
        published = true;
    }
    AuroraXrDevAudio d;
    if (!aurora_xr_dev_audio(&d)) {
        return false;
    }
    /* The Stage page open in the pause: the music out of the pause's duck
     * too, to mix against it. */
    const float duck = pc_ambience_duck();
    pc_audio_set_dev_music_gain(pc_stage_mix_listening() && duck > 0.f && duck < 1.f ? d.music / duck : d.music);
    return !d.spatial;
#else
    return false;
#endif
}

void pc_spatial_frame(const float view[3][4]) {
    static Snapshot s; /* game thread only; static to keep it off the stack */
    static uint64_t last_log;
    if (!enabled() || HSD_GObjPLinkHead == NULL) {
        return;
    }
    s.n = s.fighters = s.items = 0;
    collect(&s, HSD_GOBJ_PLINK_FIGHTER, &s.fighters);
    collect(&s, HSD_GOBJ_PLINK_ITEM, &s.items);
    collect_stage(&s);
    memcpy(s.view, view, sizeof(s.view));
    s.xr3d = pc_xr_world_3d();
    s.bypass = dev_bypass();
    s.off = !on_now();
    s.stamp = now_ns();
    pthread_mutex_lock(&s_snap_mutex);
    s_snap = s;
    pthread_mutex_unlock(&s_snap_mutex);

    if (stats_on() && s.stamp - last_log >= 2000000000ull) {
        last_log = s.stamp;
        pc_log_line("spatial: game: starts tagged=%u (crowd %u) plain=%u, rollback-late starts with emitter %u"
                    " without %u; publishing %d fighters %d items, listener %s",
                    s_starts_tagged, s_starts_crowd, s_starts_plain, s_late_found, s_late_missed, s.fighters,
                    s.items, s.xr3d ? "head (XR)" : "camera");
        s_starts_tagged = s_starts_crowd = s_starts_plain = s_late_found = s_late_missed = 0;
    }
}

/* ---- audio thread ----------------------------------------------------- */

/* Steam Audio's built-in HRTF cannot be made for AX's 32 kHz (iplHRTFCreate
 * fails there, checked against 4.8.1; 44.1 and 48 kHz work), so the
 * positional path runs at 48 kHz: each voice's mono frame is resampled 2:3
 * up to 240 samples, spatialized into a 48 kHz stereo bus, and the bus is
 * resampled 3:2 back down into the 32 kHz mix once per frame. Both are
 * fixed-ratio windowed-sinc polyphase filters (a quarter and three eighths
 * of a millisecond of delay). */
#define SP_RATE 48000
#define SP_FRAME 240               /* SP_RATE / 200: one 5 ms AX frame */
#define SP_AX_RATE 32000
#define SP_AX_FRAME 160
#define SP_UP_HALF 8               /* upsampler half-width, input samples */
#define SP_UP_TAPS (2 * SP_UP_HALF)
#define SP_DOWN_HALF 12            /* downsampler half-width, 48 kHz samples */
#define SP_DOWN_TAPS (2 * SP_DOWN_HALF)

typedef struct SpVoice {
    const void* gobj;
    float pos[3]; /* game units, the newest known */
    int channel;  /* the emitter's left (0) or right (1) point, or between them (-1) */
    float boost;  /* over the game's own level (a placed stage sound's) */
    int mix;      /* a placed stage sound's mixer channel, or -1 */
    bool room;    /* pos is in the room (metres, tracking space), not the stage */
    StageMixState mix_state;
    bool tagged;
    uint64_t last_frame; /* the AX frame it last rendered in; state resets after a gap */
    float hist[SP_UP_TAPS]; /* the upsampler's last input samples */
    IPLBinauralEffect bin;
    IPLDirectEffect dir;
} SpVoice;

/* AX's voices, then the placed sounds' slots, then the room's. */
static SpVoice s_v[SP_VOICES + SP_PLACED + SP_ROOM];
static IPLContext s_ctx;
static IPLHRTF s_hrtf;
static IPLAudioSettings s_settings;

/* The crowd: its 32 kHz stereo bus this frame, the bus at 48 kHz in delay
 * lines, and a binaural effect per seat. */
static float s_crowd_in[SP_AX_FRAME * 2];
static bool s_crowd_in_used;
static float s_crowd_up_hist[2][SP_UP_TAPS];
static float s_crowd_delay[2][SP_CROWD_DELAY];
static unsigned s_crowd_write;
static int s_crowd_tail; /* frames left to play out the delay lines */
static IPLBinauralEffect s_seat_bin[SP_CROWD_SEATS];

/* Each seat's decorrelator: velvet noise, a sparse filter of taps of random
 * sign, one at a random point in each SP_SEAT_SPAN_MS / SP_SEAT_TAPS ms,
 * decaying over SP_SEAT_DECAY_MS, of unit energy. Different noise per seat,
 * so the seats' copies of the bus don't line up into peaks at an ear. */
#define SP_SEAT_TAPS 24
#define SP_SEAT_SPAN_MS 24.0f
#define SP_SEAT_DECAY_MS 8.0f
static struct {
    unsigned delay[SP_SEAT_TAPS]; /* samples at SP_RATE */
    float gain[SP_SEAT_TAPS];
} s_seat_fir[SP_CROWD_SEATS];

static void seat_fir_init(void) {
    for (int k = 0; k < SP_CROWD_SEATS; k++) {
        uint32_t seed = 0x9E3779B9u * (uint32_t)(k + 1);
        float energy = 0.f;
        for (int t = 0; t < SP_SEAT_TAPS; t++) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            const float where = (float)(seed >> 8) / 16777216.f;
            const float ms = ((float)t + where) * (SP_SEAT_SPAN_MS / SP_SEAT_TAPS);
            s_seat_fir[k].delay[t] = (unsigned)(ms * (SP_RATE / 1000.f));
            const float g = ((seed & 1u) ? 1.f : -1.f) * expf(-ms / SP_SEAT_DECAY_MS);
            s_seat_fir[k].gain[t] = g;
            energy += g * g;
        }
        for (int t = 0; t < SP_SEAT_TAPS; t++) {
            s_seat_fir[k].gain[t] /= sqrtf(energy);
        }
    }
}

/* A stereo pair of virtual speakers: left channel from one, right from the
 * other. */
typedef struct Speakers {
    float up_hist[2][SP_UP_TAPS];
    float down_hist[2][SP_DOWN_TAPS];
    IPLBinauralEffect bin[2];
    float mix;       /* 0: plain stereo; 1: the full SP_PLACED_SHARE blend */
    float dir[2][3]; /* the speakers' last directions, head space */
} Speakers;
static Speakers s_menu_spk;  /* XR menus: beside the screen, the whole mix */
static Speakers s_music_spk; /* fights: ahead of the stage, the music */
/* Fight music this frame (32 kHz stereo), apart from the rest of the mix. */
static float s_music_bus[SP_AX_FRAME * 2];
static bool s_music_bus_on;
static bool s_bypassed; /* a fight draws, spatial audio bypassed (dev tools) */

static float s_up[3][SP_UP_TAPS];     /* upsampler phases: offsets 0, 2/3, 1/3 */
static float s_down[2][SP_DOWN_TAPS]; /* downsampler phases: offsets 0, 1/2 */
static float s_bus[2][SP_FRAME];      /* this frame's spatialized voices, 48 kHz */
static float s_bus_hist[2][SP_DOWN_TAPS];
static bool s_bus_used;               /* anything in the bus or its history */
static int s_bus_quiet;               /* frames since the bus last had a voice */

static Snapshot s_audio_snap;   /* the audio thread's copy */
static float s_listener[3][4];  /* game units -> listener space, meters */
static float s_room[3][4];      /* the room (tracking space, metres) -> listener space */
static float s_scale;           /* meters per game unit */
static bool s_active;           /* spatializing this AX frame */
static uint64_t s_frame;        /* AX frames since start */

/* Stats, MELEE_SPATIAL_AUDIO_STATS=1. */
static struct {
    unsigned tags, voice_frames, fallback_frames, frames, crowd_frames, speaker_frames;
    double energy_in, energy_out;
    uint64_t ns;
    float sample_pos[3];
    float sample_gain;
    bool sampled;
} st;

static double blackman_sinc(double x, double cutoff, double half) {
    /* cutoff: of the input's Nyquist (1: none). */
    const double pi = 3.14159265358979323846;
    if (fabs(x) >= half) {
        return 0.0;
    }
    const double t = pi * x * cutoff;
    const double s = fabs(t) < 1e-9 ? 1.0 : sin(t) / t;
    const double w = 0.42 + 0.5 * cos(pi * x / half) + 0.08 * cos(2.0 * pi * x / half);
    return s * w;
}

/* One phase: taps at distances (frac + half - 1 - t) from the output point,
 * normalized to unity gain. */
static void make_phase(float* taps, int ntaps, double frac, double cutoff) {
    const double half = ntaps / 2;
    double sum = 0.0;
    double v[64];
    for (int t = 0; t < ntaps; t++) {
        v[t] = blackman_sinc(frac + half - 1 - t, cutoff, half + 1.0);
        sum += v[t];
    }
    for (int t = 0; t < ntaps; t++) {
        taps[t] = (float)(v[t] / sum);
    }
}

static void make_filters(void) {
    /* Up: output j sits at input position j * 2/3; its fraction cycles
     * 0, 2/3, 1/3. The band is the input's own, so no cutoff below it. */
    make_phase(s_up[0], SP_UP_TAPS, 0.0, 0.95);
    make_phase(s_up[1], SP_UP_TAPS, 2.0 / 3.0, 0.95);
    make_phase(s_up[2], SP_UP_TAPS, 1.0 / 3.0, 0.95);
    /* Down: output i sits at bus position i * 3/2; fractions 0, 1/2. The
     * band is cut at 32 kHz's Nyquist, 2/3 of 48 kHz's. */
    make_phase(s_down[0], SP_DOWN_TAPS, 0.0, 2.0 / 3.0 * 0.95);
    make_phase(s_down[1], SP_DOWN_TAPS, 0.5, 2.0 / 3.0 * 0.95);
}

/* 160 samples at 32 kHz -> 240 at 48 kHz, with the voice's own history. */
static void upsample_hist(float hist[SP_UP_TAPS], const float* in, float* out) {
    float b[SP_UP_TAPS + SP_AX_FRAME];
    memcpy(b, hist, sizeof(float) * SP_UP_TAPS);
    memcpy(b + SP_UP_TAPS, in, sizeof(float) * SP_AX_FRAME);
    for (int j = 0; j < SP_FRAME; j++) {
        /* Position SP_UP_HALF + j * 2/3 in b: floor and phase. */
        const int n = SP_UP_HALF + (j * 2) / 3;
        const float* taps = s_up[j % 3];
        const float* x = b + n - SP_UP_HALF + 1;
        float acc = 0.f;
        for (int t = 0; t < SP_UP_TAPS; t++) {
            acc += x[t] * taps[t];
        }
        out[j] = acc;
    }
    memcpy(hist, b + SP_AX_FRAME, sizeof(float) * SP_UP_TAPS);
}

static void upsample(SpVoice* v, const float* in, float* out) {
    upsample_hist(v->hist, in, out);
}

/* A 48 kHz stereo bus (240 samples a side) -> 160 frames at 32 kHz, added
 * to the interleaved `out`. */
static void downsample(float hist[2][SP_DOWN_TAPS], float bus[2][SP_FRAME], float* out) {
    for (int c = 0; c < 2; c++) {
        float b[SP_DOWN_TAPS + SP_FRAME];
        memcpy(b, hist[c], sizeof(float) * SP_DOWN_TAPS);
        memcpy(b + SP_DOWN_TAPS, bus[c], sizeof(float) * SP_FRAME);
        for (int i = 0; i < SP_AX_FRAME; i++) {
            const int n = SP_DOWN_HALF + (i * 3) / 2;
            const float* taps = s_down[i & 1];
            const float* x = b + n - SP_DOWN_HALF + 1;
            float acc = 0.f;
            for (int t = 0; t < SP_DOWN_TAPS; t++) {
                acc += x[t] * taps[t];
            }
            out[i * 2 + c] += acc;
        }
        memcpy(hist[c], b + SP_FRAME, sizeof(float) * SP_DOWN_TAPS);
    }
}

static void downsample_bus(float* out) {
    downsample(s_bus_hist, s_bus, out);
}

static void log_cb(IPLLogLevel level, const char* message) {
    (void)level;
    pc_log_line("spatial: steam audio: %s", message);
}

void pc_spatial_init(int rate, int frame) {
    static bool tried;
    if (tried || !enabled()) {
        return;
    }
    tried = true;
    if (rate != SP_AX_RATE || frame != SP_AX_FRAME) {
        pc_log_line("spatial: the mixer runs at %d Hz in %d-sample frames, not %d/%d; off", rate, frame,
                    SP_AX_RATE, SP_AX_FRAME);
        return;
    }
    const uint64_t t0 = now_ns();
    IPLContextSettings cs;
    memset(&cs, 0, sizeof(cs));
    cs.version = STEAMAUDIO_VERSION;
    cs.logCallback = log_cb;
#if defined(__aarch64__) || defined(__ARM_NEON)
    cs.simdLevel = IPL_SIMDLEVEL_NEON;
#else
    cs.simdLevel = IPL_SIMDLEVEL_AVX2; /* AVX-512 can throttle the clock */
#endif
    if (iplContextCreate(&cs, &s_ctx) != IPL_STATUS_SUCCESS) {
        pc_log_line("spatial: Steam Audio context failed; off");
        return;
    }
    s_settings.samplingRate = SP_RATE;
    s_settings.frameSize = SP_FRAME;
    IPLHRTFSettings hs;
    memset(&hs, 0, sizeof(hs));
    hs.type = IPL_HRTFTYPE_DEFAULT;
    hs.volume = 1.0f;
    hs.normType = IPL_HRTFNORMTYPE_RMS;
    IPLerror err = iplHRTFCreate(s_ctx, &s_settings, &hs, &s_hrtf);
    if (err != IPL_STATUS_SUCCESS) {
        pc_log_line("spatial: Steam Audio HRTF failed (%d); off", (int)err);
        return;
    }
    for (int i = 0; i < SP_VOICES + SP_PLACED + SP_ROOM; i++) {
        IPLBinauralEffectSettings bs = {s_hrtf};
        IPLDirectEffectSettings ds = {1};
        if (iplBinauralEffectCreate(s_ctx, &s_settings, &bs, &s_v[i].bin) != IPL_STATUS_SUCCESS ||
            iplDirectEffectCreate(s_ctx, &s_settings, &ds, &s_v[i].dir) != IPL_STATUS_SUCCESS)
        {
            pc_log_line("spatial: Steam Audio effect %d failed; off", i);
            return;
        }
    }
    for (int i = 0; i < SP_CROWD_SEATS + 4; i++) {
        IPLBinauralEffectSettings bs = {s_hrtf};
        IPLBinauralEffect* e = i < SP_CROWD_SEATS     ? &s_seat_bin[i] :
                               i < SP_CROWD_SEATS + 2 ? &s_menu_spk.bin[i - SP_CROWD_SEATS] :
                                                        &s_music_spk.bin[i - SP_CROWD_SEATS - 2];
        if (iplBinauralEffectCreate(s_ctx, &s_settings, &bs, e) != IPL_STATUS_SUCCESS) {
            pc_log_line("spatial: Steam Audio crowd/speaker effect %d failed; off", i);
            return;
        }
    }
    make_filters();
    seat_fir_init();
    __atomic_store_n(&s_ready, true, __ATOMIC_RELEASE);
    pc_log_line("spatial: ready (%s), Steam Audio %d.%d.%d, HRTF binaural at %d Hz (resampled from %d) in %d-sample frames"
                " (set up in %.1f ms)",
                on_now() ? "on" : "off for now", STEAMAUDIO_VERSION_MAJOR, STEAMAUDIO_VERSION_MINOR, STEAMAUDIO_VERSION_PATCH, SP_RATE, rate,
                SP_FRAME, (now_ns() - t0) / 1e6);
}

/* A voice's point of its emitter's two. */
static void channel_pos(const float pos[2][3], int channel, float out[3]) {
    for (int k = 0; k < 3; k++) {
        out[k] = channel < 0 ? 0.5f * (pos[0][k] + pos[1][k]) : pos[channel & 1][k];
    }
}

void pc_spatial_voice_tag(int voice, const void* gobj, const float pos[2][3], int channel) {
    if (!s_ready || voice < 0 || voice >= SP_VOICES || gobj == NULL) {
        return;
    }
    SpVoice* v = &s_v[voice];
    v->gobj = gobj;
    v->channel = channel;
    const StageSound* r = stage_entry(gobj);
    v->boost = r != NULL ? r->gain : 1.f;
    v->mix = r != NULL ? r->mix : -1;
    if (r != NULL && r->lane && pc_ambience_racers()) {
        v->boost = 0.f; /* Big Blue's engines: the stage ambience's racers play instead */
        v->mix = -1;
    }
    channel_pos(pos, channel, v->pos);
    v->tagged = true;
    v->last_frame = 0; /* a new sound: state resets at its first frame */
    st.tags++;
}

void pc_spatial_voice_clear(int voice) {
    if (voice >= 0 && voice < SP_VOICES) {
        s_v[voice].tagged = false;
        s_v[voice].gobj = NULL;
    }
}

void pc_spatial_frame_begin(void) {
    s_frame++;
    s_active = false;
    s_bypassed = false;
    s_music_bus_on = false;
    if (!s_ready) {
        return;
    }
    if (s_crowd_in_used) {
        memset(s_crowd_in, 0, sizeof(s_crowd_in));
        s_crowd_in_used = false;
    }
    pthread_mutex_lock(&s_snap_mutex);
    s_audio_snap = s_snap;
    pthread_mutex_unlock(&s_snap_mutex);
    const Snapshot* s = &s_audio_snap;
    if (s->stamp == 0 || now_ns() - s->stamp > SP_STALE_NS) {
        return; /* no fight drawing (menus, scene changes) */
    }
    if (s->off) {
        return; /* turned off: every voice plain, as if it were never made */
    }
    if (s->bypass) {
        s_bypassed = true;
        return; /* every voice as it would be without spatial audio */
    }
    if (s->xr3d) {
#ifdef AURORA_ENABLE_OPENXR
        if (!aurora_xr_audio_listener(s_listener)) {
            return;
        }
        s_scale = sqrtf(s_listener[0][0] * s_listener[0][0] + s_listener[1][0] * s_listener[1][0] +
                        s_listener[2][0] * s_listener[2][0]);
#else
        return;
#endif
    } else {
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                s_listener[r][c] = s->view[r][c] * SP_DESKTOP_SCALE;
            }
        }
        s_scale = SP_DESKTOP_SCALE;
    }
    if (!(s_scale > 0.f) || !isfinite(s_scale)) {
        return;
    }
    s_active = true;
    memset(s_music_bus, 0, sizeof(s_music_bus));
    s_music_bus_on = true;
    /* Follow each tagged voice's emitter (Brinstar's lava rises and falls
     * under its sound). One no longer published (an item gone, a fighter
     * KO'd off the lists) keeps where it was last seen. */
    for (int i = 0; i < SP_VOICES; i++) {
        SpVoice* v = &s_v[i];
        if (!v->tagged || v->gobj == SP_CROWD) {
            continue;
        }
        for (int k = 0; k < s->n; k++) {
            if (s->e[k].gobj == v->gobj) {
                channel_pos(s->e[k].pos, v->channel, v->pos);
                break;
            }
        }
    }
}

float* pc_spatial_voice_bus(int voice) {
    if (voice < 0 || voice >= SP_VOICES || !s_v[voice].tagged || s_v[voice].gobj != SP_CROWD) {
        return NULL;
    }
    if (!s_active) {
        st.fallback_frames++;
        return NULL;
    }
    s_crowd_in_used = true;
    return s_crowd_in;
}

bool pc_spatial_bypassed(void) {
    return s_bypassed;
}

float* pc_spatial_ambient_bus(void) {
    if (!s_active) {
        return NULL;
    }
    s_crowd_in_used = true;
    return s_crowd_in;
}

bool pc_spatial_voice_active(int voice) {
    if (voice < 0 || voice >= SP_VOICES || !s_v[voice].tagged || s_v[voice].gobj == SP_CROWD) {
        return false;
    }
    if (!s_active) {
        st.fallback_frames++;
        return false;
    }
    return true;
}

void pc_spatial_render(int voice, const float* mono) {
    SpVoice* v = &s_v[voice];
    const uint64_t t0 = now_ns();
    float in[SP_FRAME], left[SP_FRAME], right[SP_FRAME];
    float* in_ch[1] = {in};
    float* out_ch[2] = {left, right};
    IPLAudioBuffer inb = {1, SP_FRAME, in_ch};
    IPLAudioBuffer outb = {2, SP_FRAME, out_ch};

    if (v->last_frame + 1 != s_frame) {
        iplBinauralEffectReset(v->bin);
        iplDirectEffectReset(v->dir);
        memset(v->hist, 0, sizeof(v->hist));
        memset(&v->mix_state, 0, sizeof(v->mix_state));
    }
    v->last_frame = s_frame;
    float mixed[SP_AX_FRAME];
    if (v->mix >= 0) { /* a stage sound: through its mixer channel (its aux sends are as they were) */
        memcpy(mixed, mono, sizeof(mixed));
        pc_stage_mix_apply(v->mix, &v->mix_state, mixed, SP_AX_FRAME, 1);
        mono = mixed;
    }
    upsample(v, mono, in);

    /* Listener space: x right, y up, -z ahead, meters. */
    const float* p = v->pos;
    const float(*m)[4] = v->room ? s_room : s_listener;
    float h[3];
    for (int r = 0; r < 3; r++) {
        h[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
    }
    const float d = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    IPLVector3 dir = {0.f, 0.f, -1.f};
    if (d > 1e-4f && isfinite(d)) {
        dir.x = h[0] / d;
        dir.y = h[1] / d;
        dir.z = h[2] / d;
    }
    const float ref = SP_REF_UNITS * s_scale;
    float gain = 1.f;
    if (!v->room && d > ref && isfinite(d)) { /* a room sound keeps its level */
        gain = 1.f / (1.f + SP_ROLLOFF * (d / ref - 1.f));
        if (gain < SP_MIN_GAIN) {
            gain = SP_MIN_GAIN;
        }
    }

    IPLDirectEffectParams dp;
    memset(&dp, 0, sizeof(dp));
    dp.flags = IPL_DIRECTEFFECTFLAGS_APPLYDISTANCEATTENUATION;
    dp.distanceAttenuation = gain;
    iplDirectEffectApply(v->dir, &dp, &inb, &inb);

    IPLBinauralEffectParams bp;
    memset(&bp, 0, sizeof(bp));
    bp.direction = dir;
    bp.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
    bp.spatialBlend = 1.f;
    bp.hrtf = s_hrtf;
    iplBinauralEffectApply(v->bin, &bp, &inb, &outb);

    for (int i = 0; i < SP_FRAME; i++) {
        left[i] *= SP_MAKEUP * v->boost; /* past the direct effect, whose gain tops out at 1 */
        right[i] *= SP_MAKEUP * v->boost;
        s_bus[0][i] += left[i];
        s_bus[1][i] += right[i];
    }
    s_bus_used = true;
    s_bus_quiet = 0;

    if (stats_on()) {
        for (int i = 0; i < SP_AX_FRAME; i++) {
            /* The dry signal as plain stereo would have carried it: the
             * mono level on both sides (the equal-power center). */
            st.energy_in += 2.0 * (double)mono[i] * mono[i];
        }
        for (int i = 0; i < SP_FRAME; i++) {
            /* At 48 kHz: 1.5 samples per input sample. */
            st.energy_out += ((double)left[i] * left[i] + (double)right[i] * right[i]) * (2.0 / 3.0);
        }
        st.voice_frames++;
        st.ns += now_ns() - t0;
        if (!st.sampled) {
            st.sampled = true;
            memcpy(st.sample_pos, h, sizeof(h));
            st.sample_gain = gain;
        }
    }
}

bool pc_spatial_listener_distance(const float pos[3], float* units) {
    if (!s_active || s_scale <= 0.f) {
        return false;
    }
    float h[3];
    for (int r = 0; r < 3; r++) {
        h[r] = s_listener[r][0] * pos[0] + s_listener[r][1] * pos[1] + s_listener[r][2] * pos[2] + s_listener[r][3];
    }
    *units = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]) / s_scale;
    return isfinite(*units);
}

bool pc_spatial_render_placed(int slot, const float pos[3], const float* mono) {
    if (!s_active || slot < 0 || slot >= SP_PLACED) {
        return false;
    }
    SpVoice* v = &s_v[SP_VOICES + slot];
    memcpy(v->pos, pos, sizeof(v->pos));
    v->boost = 1.f;
    v->mix = -1; /* the ambience mixes its own */
    v->room = false;
    pc_spatial_render(SP_VOICES + slot, mono);
    return true;
}

bool pc_spatial_render_room(int slot, const float pos[3], const float* mono) {
#ifdef AURORA_ENABLE_OPENXR
    if (!s_ready || !on_now() || slot < 0 || slot >= SP_ROOM || !aurora_xr_audio_room(s_room)) {
        return false;
    }
    const int voice = SP_VOICES + SP_PLACED + slot;
    SpVoice* v = &s_v[voice];
    memcpy(v->pos, pos, sizeof(v->pos));
    v->boost = 1.f;
    v->mix = -1;
    v->room = true;
    pc_spatial_render(voice, mono);
    return true;
#else
    (void)slot;
    (void)pos;
    (void)mono;
    return false;
#endif
}

/* A direction fixed to the stage's axes (x right, y up, +z toward the
 * player; azimuth from straight at the stage, negative left), in head
 * space: the listener's rotation, unscaled. */
static IPLVector3 stage_direction(float azimuth_deg, float elevation_deg) {
    const float deg = 3.14159265f / 180.f;
    const float az = azimuth_deg * deg, el = elevation_deg * deg;
    const float g[3] = {sinf(az) * cosf(el), sinf(el), -cosf(az) * cosf(el)};
    float h[3];
    for (int r = 0; r < 3; r++) {
        h[r] = (s_listener[r][0] * g[0] + s_listener[r][1] * g[1] + s_listener[r][2] * g[2]) / s_scale;
    }
    IPLVector3 dir = {h[0], h[1], h[2]};
    return dir;
}

/* The crowd's bus, from its seats, into the positional bus. */
static void crowd_render(void) {
    float up[2][SP_FRAME];
    for (int c = 0; c < 2; c++) {
        float in[SP_AX_FRAME];
        for (int i = 0; i < SP_AX_FRAME; i++) {
            in[i] = s_crowd_in[i * 2 + c];
        }
        upsample_hist(s_crowd_up_hist[c], in, up[c]);
        for (int i = 0; i < SP_FRAME; i++) {
            s_crowd_delay[c][(s_crowd_write + i) % SP_CROWD_DELAY] = up[c][i];
        }
    }
    const float seat_gain = SP_MAKEUP / sqrtf(SP_CROWD_SEATS / 2.f); /* a side's power over its seats */
    for (int k = 0; k < SP_CROWD_SEATS; k++) {
        const IPLVector3 dir = stage_direction(s_seats[k].azimuth, SP_CROWD_ELEVATION);
        const unsigned d = (unsigned)(s_seats[k].delay_ms * (SP_RATE / 1000.f));
        float in[SP_FRAME], left[SP_FRAME], right[SP_FRAME];
        const float* line = s_crowd_delay[s_seats[k].side];
        memset(in, 0, sizeof(in));
        for (int t = 0; t < SP_SEAT_TAPS; t++) {
            const unsigned at = s_crowd_write + SP_CROWD_DELAY - d - s_seat_fir[k].delay[t];
            const float g = s_seat_fir[k].gain[t];
            for (int i = 0; i < SP_FRAME; i++) {
                in[i] += g * line[(at + (unsigned)i) & (SP_CROWD_DELAY - 1)];
            }
        }
        float* in_ch[1] = {in};
        float* out_ch[2] = {left, right};
        IPLAudioBuffer inb = {1, SP_FRAME, in_ch};
        IPLAudioBuffer outb = {2, SP_FRAME, out_ch};
        IPLBinauralEffectParams bp;
        memset(&bp, 0, sizeof(bp));
        bp.direction = dir;
        bp.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
        bp.spatialBlend = 1.f;
        bp.hrtf = s_hrtf;
        iplBinauralEffectApply(s_seat_bin[k], &bp, &inb, &outb);
        for (int i = 0; i < SP_FRAME; i++) {
            s_bus[0][i] += left[i] * seat_gain;
            s_bus[1][i] += right[i] * seat_gain;
        }
    }
    s_crowd_write = (s_crowd_write + SP_FRAME) % SP_CROWD_DELAY;
    s_bus_used = true;
    s_bus_quiet = 0;
}

void pc_spatial_frame_end(float* out) {
    if (!s_ready) {
        return;
    }
    /* The crowd plays out its delay lines for a while after its last voice
     * (silence going in), then stops costing anything. */
    if (s_active && s_crowd_in_used) {
        s_crowd_tail = (int)(SP_CROWD_DELAY / SP_FRAME) + 1;
    }
    if (s_active && s_crowd_tail > 0) {
        const uint64_t t0 = now_ns();
        crowd_render();
        s_crowd_tail--;
        st.crowd_frames++;
        st.ns += now_ns() - t0;
    } else if (s_crowd_tail > 0) {
        s_crowd_tail = 0; /* the fight stopped drawing: start clean next time */
        memset(s_crowd_delay, 0, sizeof(s_crowd_delay));
        memset(s_crowd_up_hist, 0, sizeof(s_crowd_up_hist));
        for (int k = 0; k < SP_CROWD_SEATS; k++) {
            iplBinauralEffectReset(s_seat_bin[k]);
        }
    }
    /* The bus drains through the downsampler for its length after the last
     * voice, then stops costing anything. */
    if (s_bus_used) {
        const uint64_t t0 = now_ns();
        downsample_bus(out);
        memset(s_bus, 0, sizeof(s_bus));
        if (++s_bus_quiet > 2) {
            s_bus_used = false;
            memset(s_bus_hist, 0, sizeof(s_bus_hist));
        }
        st.ns += now_ns() - t0;
    }
    if (!stats_on()) {
        return;
    }
    st.frames++;
    if (st.frames % 400 != 0) { /* 2 s of 5 ms frames */
        return;
    }
    unsigned active = 0;
    for (int i = 0; i < SP_VOICES; i++) {
        active += s_v[i].tagged;
    }
    const double ratio = st.energy_in > 0.0 ? 10.0 * log10(st.energy_out / st.energy_in) : 0.0;
    pc_log_line("spatial: audio: %u voices tagged, %u voice-frames spatialized, %u fell back to stereo, %u tagged now;"
                " crowd %u frames, menu speakers %u;"
                " %.1f us per AX frame (%.2f%% of its 5 ms); out/in %+.1f dB; e.g. at %.2f,%.2f,%.2f m gain %.2f",
                st.tags, st.voice_frames, st.fallback_frames, active, st.crowd_frames, st.speaker_frames,
                st.ns / 1e3 / 400.0,
                st.ns / 1e9 / 400.0 / 0.005 * 100.0, ratio, st.sample_pos[0], st.sample_pos[1],
                st.sample_pos[2], st.sample_gain);
    st.tags = st.voice_frames = st.fallback_frames = st.crowd_frames = st.speaker_frames = 0;
    st.energy_in = st.energy_out = 0.0;
    st.ns = 0;
    st.sampled = false;
}

float* pc_spatial_music_bus(void) {
    return s_ready && s_music_bus_on ? s_music_bus : NULL;
}

/* The interleaved 32 kHz stereo `in`, plain and from the pair at `sp->dir`,
 * ramping sp->mix toward `want`: added to `out`, or replacing it when `out`
 * is `in`. */
static void speakers_blend(Speakers* sp, bool want, const float* in, float* out) {
    if (sp->mix <= 0.f) { /* coming in: start the speakers clean */
        memset(sp->up_hist, 0, sizeof(sp->up_hist));
        memset(sp->down_hist, 0, sizeof(sp->down_hist));
        iplBinauralEffectReset(sp->bin[0]);
        iplBinauralEffectReset(sp->bin[1]);
    }
    float bus[2][SP_FRAME];
    memset(bus, 0, sizeof(bus));
    for (int k = 0; k < 2; k++) {
        float in32[SP_AX_FRAME], up[SP_FRAME], left[SP_FRAME], right[SP_FRAME];
        for (int i = 0; i < SP_AX_FRAME; i++) {
            in32[i] = in[i * 2 + k];
        }
        upsample_hist(sp->up_hist[k], in32, up);
        float* in_ch[1] = {up};
        float* out_ch[2] = {left, right};
        IPLAudioBuffer inb = {1, SP_FRAME, in_ch};
        IPLAudioBuffer outb = {2, SP_FRAME, out_ch};
        IPLBinauralEffectParams bp;
        memset(&bp, 0, sizeof(bp));
        bp.direction.x = sp->dir[k][0];
        bp.direction.y = sp->dir[k][1];
        bp.direction.z = sp->dir[k][2];
        bp.interpolation = IPL_HRTFINTERPOLATION_NEAREST;
        bp.spatialBlend = 1.f;
        bp.hrtf = s_hrtf;
        iplBinauralEffectApply(sp->bin[k], &bp, &inb, &outb);
        for (int i = 0; i < SP_FRAME; i++) {
            bus[0][i] += left[i] * SP_MAKEUP;
            bus[1][i] += right[i] * SP_MAKEUP;
        }
    }
    float placed[SP_AX_FRAME * 2];
    memset(placed, 0, sizeof(placed));
    downsample(sp->down_hist, bus, placed);
    /* By power: plain keeps 1 - SP_PLACED_SHARE of it, the pair the rest;
     * sp->mix ramps the blend in and out over SP_SPEAKER_FADE frames. */
    const float dry = sqrtf(1.f - SP_PLACED_SHARE), wet = sqrtf(SP_PLACED_SHARE);
    const float from = sp->mix;
    float to = from + (want ? 1.f : -1.f) / SP_SPEAKER_FADE;
    to = to < 0.f ? 0.f : to > 1.f ? 1.f : to;
    for (int i = 0; i < SP_AX_FRAME; i++) {
        const float m = from + (to - from) * (float)(i + 1) / SP_AX_FRAME;
        const float gd = 1.f + m * (dry - 1.f), gw = m * wet;
        for (int c = 0; c < 2; c++) {
            const float y = gd * in[i * 2 + c] + gw * placed[i * 2 + c];
            if (in == out) {
                out[i * 2 + c] = y; /* the whole mix, replaced */
            } else {
                out[i * 2 + c] += y; /* a bus, added */
            }
        }
    }
    sp->mix = to;
}

void pc_spatial_output(float* out) {
    if (!s_ready) {
        return;
    }
    const uint64_t t0 = now_ns();
    bool used = false;
    /* Menus: all of it down a little, gliding in and out. */
    static float menu_trim = -1.f, menu_gain = 1.f;
    if (menu_trim < 0.f) {
        const char* v = getenv("MELEE_MENU_GAIN");
        menu_trim = powf(10.f, (v != NULL && v[0] != '\0' ? (float)atof(v) : SP_MENU_DB) / 20.f);
    }
    /* Turned off: the menus' trim and the XR speakers glide out, as it was
     * without spatial audio. */
    const bool off = !on_now();
    const float menu_want = s_active || off ? 1.f : menu_trim;
    const float menu0 = menu_gain;
    const float menu_step = (1.f - menu_trim) * (float)SP_AX_FRAME / (SP_MENU_GLIDE_S * (float)SP_AX_RATE);
    menu_gain = menu_gain < menu_want ? fminf(menu_want, menu_gain + menu_step) : fmaxf(menu_want, menu_gain - menu_step);
    if (menu0 != 1.f || menu_gain != 1.f) {
        for (int i = 0; i < SP_AX_FRAME; i++) {
            const float g = menu0 + (menu_gain - menu0) * ((float)i / (float)SP_AX_FRAME);
            out[i * 2] *= g;
            out[i * 2 + 1] *= g;
        }
    }
    /* Fights: the music, from ahead of the stage, turned down. */
    if (s_music_bus_on) {
        static float trim = -1.f;
        if (trim < 0.f) {
            const char* v = getenv("MELEE_SPATIAL_MUSIC_GAIN");
            trim = powf(10.f, (v != NULL && v[0] != '\0' ? (float)atof(v) : SP_FIGHT_MUSIC_DB) / 20.f);
        }
        for (int i = 0; i < SP_AX_FRAME * 2; i++) {
            s_music_bus[i] *= trim;
        }
        for (int k = 0; k < 2; k++) {
            const IPLVector3 d = stage_direction(k == 0 ? -SP_MUSIC_AZIMUTH : SP_MUSIC_AZIMUTH, SP_MUSIC_ELEVATION);
            s_music_spk.dir[k][0] = d.x;
            s_music_spk.dir[k][1] = d.y;
            s_music_spk.dir[k][2] = d.z;
        }
        speakers_blend(&s_music_spk, true, s_music_bus, out);
        used = true;
    } else {
        s_music_spk.mix = 0.f; /* the music is back in the plain mix */
    }
#ifdef AURORA_ENABLE_OPENXR
    /* XR menus: no fight is drawing and the flat screen is up. */
    float c[3], r[3], w;
    const bool want = !s_active && !off && aurora_xr_audio_screen(c, r, &w);
    if (want) {
        for (int k = 0; k < 2; k++) {
            const float side = (k == 0 ? -1.f : 1.f) * (0.5f * w + SP_SPEAKER_GAP);
            float p[3] = {c[0] + r[0] * side, c[1] + r[1] * side, c[2] + r[2] * side};
            const float d = sqrtf(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (d > 1e-4f && isfinite(d)) {
                for (int i = 0; i < 3; i++) {
                    s_menu_spk.dir[k][i] = p[i] / d;
                }
            }
        }
    }
    if (want || s_menu_spk.mix > 0.f) {
        speakers_blend(&s_menu_spk, want, out, out);
        st.speaker_frames++;
        used = true;
    }
#endif
    if (used) {
        st.ns += now_ns() - t0;
    }
}

#endif
