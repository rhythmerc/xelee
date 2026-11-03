/* Capture the Flag, in the sim (docs/xeleenet-modes-plan.md §3).
 *
 * Two flags (items, itctfflag.c) on their teams' stands. A flag is "home"
 * while it sits on its stand untouched by the enemy. Sending a flag home is
 * always the same: destroy the item (a holder lets go, ftLib_ReleaseItem) and
 * spawn a new one on the stand. That is what happens on a capture, when a
 * team picks up its own flag away from home, when a free flag's return timer
 * runs out, and when a flag leaves the stage.
 *
 * Debug hooks (only when the variable is set):
 *   MELEE_DEBUG_CTF=1             a CTF match on Temple from the debug VS
 *   MELEE_DEBUG_CTF_BASES=x,y,x,y  red and blue stands elsewhere (tests)
 *   MELEE_DEBUG_CTF_WIN=N         captures to win
 *   MELEE_DEBUG_CTF_FREE=t,x,y    team t's flag starts free at x,y, not home
 *   MELEE_DEBUG_CTF_FLAG=1|x,y    one stray flag, outside a CTF match (M0)
 *   MELEE_DEBUG_CTF_TEAM=0|1      that stray flag's team
 *   MELEE_DEBUG_CTF_LOG=N         port 1's and the flags' state every N frames
 *   MELEE_DEBUG_CTF_SURVEY=1      the stage's collision, bounds and points */

#include "gmctf.h"

#include <Runtime/platform.h>

#include <dolphin/os.h>
#include <melee/ft/fighter.h>
#include <melee/ft/ftchangeparam.h>
#include <melee/ft/ftlib.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>
#include <melee/gm/gmvs.h>
#include <melee/gr/forward.h>
#include <melee/gr/ground.h>
#include <melee/gr/stage.h>
#include <melee/it/forward.h>
#include <melee/it/inlines.h>
#include <melee/it/item.h>
#include <melee/it/kinds/itctfflag.h>
#include <melee/it/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/mn/types.h>
#include <melee/if/ifctf.h>
#include <melee/cm/camera.h>
#include <melee/cm/forward.h>
#include <melee/cm/types.h>
#include <melee/mp/mplib.h>
#include <melee/mp/types.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/state.h>
#include <sysdolphin/baselib/tev.h>

#include "pc/ctf_art.h"

#ifdef TARGET_PC
#include <math.h>
#include <stdlib.h>
#include <string.h>
#endif

#ifdef TARGET_PC

/* ---- layouts (plan §3.2) ------------------------------------------------ */

typedef struct CtfBox {
    float l, r, b, t;
} CtfBox;

typedef struct CtfBase {
    const char* name; /* where it is, for the sides card */
    Vec2 stand;       /* where the flag stands */
    CtfBox zone;   /* the enemy flag in here scores */
    Vec2 spawn[2]; /* start and respawn, in port order within the team */
} CtfBase;

typedef struct CtfLayout {
    u16 stkind;
    CtfBase base[2]; /* red, blue (before any side swap) */
} CtfLayout;

static const CtfLayout ctf_layouts[] = {
    /* Hyrule Temple: red in the cave at the bottom left, blue on the right
     * edge of the ground floor (the M0 survey). */
    { St_Kind_Shrine,
      { { "BOTTOM LEFT", { -105.0f, -88.6f }, { -134.0f, -75.0f, -88.6f, -50.0f },
          { { -125.0f, -70.0f }, { -88.0f, -70.0f } } },
        { "RIGHT EDGE", { 200.0f, -5.3f }, { 170.0f, 225.6f, -5.3f, 30.0f },
          { { 182.0f, 22.0f }, { 214.0f, 22.0f } } } } },
};

/* MELEE_DEBUG_CTF_BASES: stands placed by a test, zones and spawns around them. */
static CtfLayout ctf_debug_layout;

/* READY's length: the camera frames both bases and the beams stand. */
#define CTF_READY_FRAMES 150

/* The capture zones' edge (pc_ctf_zone_ring): a third of an average
 * fighter's height (Mario and Fox stand about 14-15 units), and the footprint
 * this far in front of and behind the fighters' plane. */
#define CTF_ZONE_RING_HEIGHT 5.0f
#define CTF_ZONE_RING_DEPTH 10.0f

/* A zone that can't score (home required, its own flag away) dims and
 * breathes: its alpha swings between these every CTF_ZONE_PULSE_FRAMES, and
 * it fades between that and full over CTF_ZONE_FADE_FRAMES. */
#define CTF_ZONE_DIM_LOW 0.15f
#define CTF_ZONE_DIM_HIGH 0.45f
#define CTF_ZONE_PULSE_FRAMES 100
#define CTF_ZONE_FADE_FRAMES 12

/* ---- state: all of it rolls back ---------------------------------------- */

static bool ctf_requested;
static GmCtfRules ctf_rules;

static struct {
    HSD_GObj* flag[2]; /* the team's flag item, or NULL until respawned */
    bool home[2];      /* on its stand, untouched by the enemy */
    u8 captures[2];
    s8 winner;         /* -1 until a team reaches captures_to_win */
    bool carry[4][2];  /* attributes last built carrying a flag (slowdown) */
    int frame;
    HSD_GObj* stray;   /* MELEE_DEBUG_CTF_FLAG's flag */
    CmSubject* cam[2]; /* READY: the camera frames both bases too */
    bool stray_spawned;
} ctf;

/* ---- helpers ------------------------------------------------------------- */

static const CtfLayout* ctf_layout(void)
{
    const char* env = getenv("MELEE_DEBUG_CTF_BASES");
    int i;
    if (env != NULL) {
        float v[4];
        char* p = (char*) env;
        for (i = 0; i < 4; i++) {
            v[i] = (float) strtod(p, &p);
            if (*p == ',') {
                p++;
            }
        }
        ctf_debug_layout.stkind = gm_GetStKind();
        for (i = 0; i < 2; i++) {
            CtfBase* b = &ctf_debug_layout.base[i];
            b->name = i ? "TEST BLUE" : "TEST RED";
            b->stand.x = v[i * 2];
            b->stand.y = v[i * 2 + 1];
            b->zone.l = b->stand.x - 15.0f;
            b->zone.r = b->stand.x + 15.0f;
            b->zone.b = b->stand.y - 2.0f;
            b->zone.t = b->stand.y + 30.0f;
            b->spawn[0].x = b->stand.x - 8.0f;
            b->spawn[1].x = b->stand.x + 8.0f;
            b->spawn[0].y = b->spawn[1].y = b->stand.y + 15.0f;
        }
        return &ctf_debug_layout;
    }
    for (i = 0; i < (int) (sizeof ctf_layouts / sizeof ctf_layouts[0]); i++) {
        if (ctf_layouts[i].stkind == gm_GetStKind()) {
            return &ctf_layouts[i];
        }
    }
    return NULL;
}

/* A team's base, after this game's side swap. */
static const CtfBase* ctf_base(int team)
{
    const CtfLayout* lay = ctf_layout();
    return lay != NULL ? &lay->base[(team ^ (ctf_rules.swap_sides ? 1 : 0)) & 1] : NULL;
}

/* CTF has two teams: Melee's blue is blue, every other colour plays red. */
static int ctf_team_of_slot(int slot)
{
    StartMeleeRules* r = gm_GetStartMeleeRules();
    if (r != NULL && r->is_teams) {
        return Player_GetTeam(slot) == 1 ? 1 : 0;
    }
    return slot & 1;
}

static int ctf_team_of_fighter(HSD_GObj* fighter)
{
    return ctf_team_of_slot(GET_FIGHTER(fighter)->player_idx);
}

static bool ctf_in_box(const CtfBox* b, const Vec3* p)
{
    return p->x >= b->l && p->x <= b->r && p->y >= b->b && p->y <= b->t;
}

static HSD_GObj* ctf_spawn_flag(Vec3 pos, int team)
{
    SpawnItem spawn;
    HSD_GObj* g;

    pos.z = 0.0f;
    spawn.kind = It_Kind_Ctf_Flag;
    spawn.prev_pos = pos;
    spawn.pos = pos;
    spawn.facing_dir = 1.0f;
    spawn.x3C_damage = 0;
    spawn.vel.x = spawn.vel.y = spawn.vel.z = 0.0f;
    spawn.x0_parent_gobj = NULL;
    spawn.x4_parent_gobj2 = NULL;
    spawn.x44_flag.b0 = 1;
    spawn.x40 = 0;
    g = Item_80268B18(&spawn);
    if (g != NULL) {
        GET_ITEM(g)->xDD4_itemVar.ctfflag.team = (u8) team;
    }
    return g;
}

/* Put a team's flag back on its stand. */
static void ctf_send_home(int team, const char* why)
{
    const CtfBase* base = ctf_base(team);
    Vec3 pos;
    HSD_GObj* old = ctf.flag[team];

    ctf.flag[team] = NULL;
    if (old != NULL) {
        Item_8026A8EC(old); /* a holder lets go first (ftLib_ReleaseItem) */
    }
    pos.x = base->stand.x;
    pos.y = base->stand.y + 4.0f; /* drops onto the stand */
    pos.z = 0.0f;
    ctf.flag[team] = ctf_spawn_flag(pos, team);
    ctf.home[team] = true;
    OSReport("ctf: f%d %s flag home (%s)\n", ctf.frame, team ? "blue" : "red", why);
}

/* Where a flag is: its holder's position while held. */
static bool ctf_flag_pos(HSD_GObj* flag, Vec3* out, HSD_GObj** holder)
{
    Item* ip;
    *holder = NULL;
    if (flag == NULL) {
        return false;
    }
    ip = GET_ITEM(flag);
    if (ip->xDC8_word.flags.x13 && ip->owner != NULL && ftLib_IsFighter(ip->owner) &&
        GET_FIGHTER(ip->owner)->item_gobj == flag)
    {
        *holder = ip->owner;
        *out = GET_FIGHTER(ip->owner)->cur_pos;
    } else {
        *out = ip->pos;
    }
    return true;
}

/* ---- the rules, once per frame ------------------------------------------- */

static void ctf_update_flags(void)
{
    int team;
    for (team = 0; team < 2; team++) {
        HSD_GObj* holder;
        Vec3 pos;
        Item* ip;

        if (ctf.flag[team] == NULL) {
            /* Lost past the blast zone, or never placed. */
            ctf_send_home(team, "respawn");
            if (ctf.frame == 1 && getenv("MELEE_DEBUG_CTF_FREE") != NULL &&
                atoi(getenv("MELEE_DEBUG_CTF_FREE")) == team)
            {
                /* A test's flag away from home. */
                char* p = strchr(getenv("MELEE_DEBUG_CTF_FREE"), ',');
                Vec3 at = { 0.0f, 0.0f, 0.0f };
                if (p != NULL) {
                    at.x = (float) strtod(p + 1, &p);
                    at.y = p != NULL && *p == ',' ? (float) strtod(p + 1, NULL) : 0.0f;
                }
                Item_8026A8EC(ctf.flag[team]);
                ctf.flag[team] = ctf_spawn_flag(at, team);
                ctf.home[team] = false;
                OSReport("ctf: f%d %s flag starts free at %.1f %.1f\n", ctf.frame, team ? "blue" : "red", at.x,
                         at.y);
            }
            continue;
        }
        ctf_flag_pos(ctf.flag[team], &pos, &holder);
        ip = GET_ITEM(ctf.flag[team]);
        if (holder != NULL) {
            ip->xDD4_itemVar.ctfflag.return_left = 0;
            if (ctf_team_of_fighter(holder) == team) {
                /* Your own team picked it up away from home: it goes home. */
                ctf_send_home(team, "returned by its team");
            } else {
                ctf.home[team] = false;
            }
        } else if (ctf.home[team]) {
            /* Knocked about on its stand: it stays put, so a big shove
             * puts it back. */
            const CtfBase* base = ctf_base(team);
            const float dx = pos.x - base->stand.x, dy = pos.y - base->stand.y;
            if (dx * dx + dy * dy > 12.0f * 12.0f) {
                ctf_send_home(team, "knocked off its stand");
            }
        } else {
            /* Free: thrown, dropped, or lying where it fell. */
            u16* left = &ip->xDD4_itemVar.ctfflag.return_left;
            if (ctf_rules.return_frames != 0) {
                if (*left == 0) {
                    *left = ctf_rules.return_frames;
                } else if (--*left == 0) {
                    ctf_send_home(team, "timer");
                }
            }
        }
    }
}

static void ctf_check_captures(void)
{
    int team;
    if (ctf.winner >= 0) {
        return;
    }
    for (team = 0; team < 2; team++) {
        const int enemy = team ^ 1;
        const CtfBase* base = ctf_base(team);
        HSD_GObj* holder;
        Vec3 pos;

        if (ctf.home[enemy] || !ctf_flag_pos(ctf.flag[enemy], &pos, &holder)) {
            continue;
        }
        if (!ctf_in_box(&base->zone, &pos)) {
            continue;
        }
        if (ctf_rules.home_required && !ctf.home[team]) {
            continue;
        }
        ctf.captures[team]++;
        lbAudioAx_800237A8(CTF_CAPTURE_SFX, 0x7F, 0x40);
        OSReport("ctf: f%d capture %s (%s) red %d blue %d\n", ctf.frame, team ? "blue" : "red",
                 holder != NULL ? "carried" : "thrown", ctf.captures[0], ctf.captures[1]);
        ctf_send_home(enemy, "captured");
        if (ctf.captures[team] >= ctf_rules.captures_to_win) {
            ctf.winner = (s8) team;
            OSReport("ctf: f%d %s wins\n", ctf.frame, team ? "blue" : "red");
        }
    }
}

/* When a fighter starts or stops carrying a flag, rebuild its attributes,
 * which applies or removes CTF_FLAG_CARRY_SPEED. */
static void ctf_update_carriers(void)
{
    int slot, idx;
    for (slot = 0; slot < 4; slot++) {
        for (idx = 0; idx < 2; idx++) {
            HSD_GObj* g = Player_GetEntityAtIndex(slot, idx);
            bool holding;
            if (g == NULL) {
                continue;
            }
            holding = itCtfFlag_Is(GET_FIGHTER(g)->item_gobj);
            if (holding != ctf.carry[slot][idx]) {
                ctf.carry[slot][idx] = holding;
                ftCo_800D105C(g);
            }
        }
    }
}

/* ---- debug ---------------------------------------------------------------- */

/* MELEE_DEBUG_CTF_FLAG: one flag beside port 1 ("1"), or at "x,y". */
static void ctf_debug_spawn(void)
{
    const char* env = getenv("MELEE_DEBUG_CTF_FLAG");
    HSD_GObj* p1 = Player_GetEntity(0);
    Vec3 pos;
    char* end;
    double x;

    if (ctf.stray_spawned || env == NULL || *env == '\0' || p1 == NULL) {
        return;
    }
    ctf.stray_spawned = true;
    pos = GET_FIGHTER(p1)->cur_pos;
    pos.x += 15.0f;
    pos.y += 10.0f;
    x = strtod(env, &end);
    if (end != env && *end == ',') {
        pos.x = (float) x;
        pos.y = (float) strtod(end + 1, NULL);
    }
    ctf.stray = ctf_spawn_flag(pos, getenv("MELEE_DEBUG_CTF_TEAM") != NULL
                                        ? atoi(getenv("MELEE_DEBUG_CTF_TEAM")) & 1
                                        : 0);
    OSReport("ctf: flag spawn at %.1f %.1f -> %s\n", pos.x, pos.y, ctf.stray != NULL ? "ok" : "refused");
}

/* MELEE_DEBUG_CTF_SURVEY=1: the stage's collision lines, bounds and named
 * points, once, as "ctf-survey:" lines (tools/ctf_survey_draw.py). */
static void ctf_survey(void)
{
    MapCollData* md = mpLib_8004D164();
    CollLine* lines = mpGetGroundCollLine();
    CollVtx* vtx = mpGetGroundCollVtx();
    int i;

    if (getenv("MELEE_DEBUG_CTF_SURVEY") == NULL || md == NULL) {
        return;
    }
    OSReport("ctf-survey: light item drop roll HSD_Randi(%d)\n", (int) p_ftCommonData->x418);
    OSReport("ctf-survey: blast %.1f %.1f %.1f %.1f\n", Stage_GetBlastZoneLeftOffset(),
             Stage_GetBlastZoneRightOffset(), Stage_GetBlastZoneTopOffset(), Stage_GetBlastZoneBottomOffset());
    OSReport("ctf-survey: cam %.1f %.1f %.1f %.1f\n", Stage_GetCamBoundsLeftOffset(),
             Stage_GetCamBoundsRightOffset(), Stage_GetCamBoundsTopOffset(), Stage_GetCamBoundsBottomOffset());
    for (i = 0; i < 8; i++) {
        Vec3 p;
        if (Ground_801C2D24(i, &p)) {
            OSReport("ctf-survey: point %d %.1f %.1f\n", i, p.x, p.y);
        }
    }
    for (i = 0; i < md->line_count; i++) {
        const MapLine* l = lines[i].x0;
        const u32 f = lines[i].flags;
        const char k = (f & CollLine_Floor) ? 'F' : (f & CollLine_Ceiling) ? 'C'
                     : (f & CollLine_RightWall) ? 'R' : (f & CollLine_LeftWall) ? 'L' : '?';
        if (l == NULL) {
            continue;
        }
        OSReport("ctf-survey: line %d %c %.1f %.1f %.1f %.1f\n", i, k, vtx[l->v0_idx].pos.x,
                 vtx[l->v0_idx].pos.y, vtx[l->v1_idx].pos.x, vtx[l->v1_idx].pos.y);
    }
    OSReport("ctf-survey: end\n");
}

static void ctf_debug_log(void)
{
    static int every = -1;
    int team;
    if (every < 0) {
        const char* e = getenv("MELEE_DEBUG_CTF_LOG");
        every = e != NULL ? atoi(e) : 0;
    }
    if (every <= 0 || ctf.frame % every != 0) {
        return;
    }
    {
        HSD_GObj* p1 = Player_GetEntity(0);
        if (p1 != NULL) {
            Fighter* fp = GET_FIGHTER(p1);
            OSReport("ctf: f%d p1 %.1f %.1f holding=%d motion=%d walk=%.3f dash=%.3f\n", ctf.frame,
                     fp->cur_pos.x, fp->cur_pos.y, fp->item_gobj != NULL, fp->motion_id,
                     fp->co_attrs.walk_max_vel, fp->co_attrs.dash_max_velocity);
        }
    }
    if (ctf.stray != NULL) {
        Item* ip = ctf.stray->user_data;
        OSReport("ctf: f%d flag %.1f %.1f state=%d\n", ctf.frame, ip->pos.x, ip->pos.y, ip->msid);
    }
    for (team = 0; team < 2; team++) {
        HSD_GObj* holder;
        Vec3 pos;
        if (ctf_flag_pos(ctf.flag[team], &pos, &holder)) {
            OSReport("ctf: f%d %s %.1f %.1f home=%d held=%d ret=%d\n", ctf.frame, team ? "blue" : "red",
                     pos.x, pos.y, ctf.home[team], holder != NULL,
                     GET_ITEM(ctf.flag[team])->xDD4_itemVar.ctfflag.return_left);
        }
    }
}

/* ---- the per-frame process ------------------------------------------------ */

/* A zone ring's alpha: full while its team can score there, else a slow
 * pulse between CTF_ZONE_DIM_LOW and _HIGH (the rule in ctf_check_captures:
 * with home required, a team scores only while its own flag is home). The
 * fade between the two is the draw's own (a step per sim frame advanced, so
 * extra draws of one frame, XR eyes or held redraws, don't speed it), never
 * the sim's. */
static float ctf_zone_ring_alpha(int team)
{
    static float level[2] = { 1.0f, 1.0f }; /* 1 live, 0 dimmed */
    static int last_frame[2] = { -1, -1 };
    const bool live = !ctf_rules.home_required || ctf.home[team];
    const float target = live ? 1.0f : 0.0f;
    const float phase = (float) (ctf.frame % CTF_ZONE_PULSE_FRAMES) / CTF_ZONE_PULSE_FRAMES;
    const float pulse =
        CTF_ZONE_DIM_LOW + (CTF_ZONE_DIM_HIGH - CTF_ZONE_DIM_LOW) * (0.5f - 0.5f * cosf(phase * 6.2831853f));

    if (ctf.frame != last_frame[team]) {
        /* Frames since the last draw; a rollback or a new match counts one. */
        int steps = ctf.frame > last_frame[team] ? ctf.frame - last_frame[team] : 1;
        float step;
        if (steps > CTF_ZONE_FADE_FRAMES) {
            steps = CTF_ZONE_FADE_FRAMES;
        }
        step = (float) steps / CTF_ZONE_FADE_FRAMES;
        if (level[team] < target) {
            level[team] = level[team] + step > target ? target : level[team] + step;
        } else if (level[team] > target) {
            level[team] = level[team] - step < target ? target : level[team] - step;
        }
        last_frame[team] = ctf.frame;
    }
    return pulse + (1.0f - pulse) * level[team];
}

/* The stands, in the stage's draw: a plate under each, and a ghost of the
 * flag while it is away (itCtfFlag_DrawStand). Reads state, writes none. */
static void ctf_draw_stands(HSD_GObj* gobj, intptr_t pass)
{
    static const GXColor body[2] = { { 232, 58, 58, 255 }, { 63, 111, 240, 255 } };
    static const GXColor light[2] = { { 255, 120, 120, 255 }, { 130, 170, 255, 255 } };
    Mtx view;
    int team;
    if (!gmCtf_Active() || Camera_80031060() != 0) {
        return;
    }
    for (team = 0; team < 2; team++) {
        const CtfBase* base = ctf_base(team);
        Vec3 at;
        at.x = base->stand.x;
        at.y = base->stand.y;
        at.z = 0.0f;
        itCtfFlag_DrawStand(&at, (u8) team, !ctf.home[team] || ctf.flag[team] == NULL);
    }
    HSD_CObjGetViewingMtx(HSD_CObjGetCurrent(), view);
    GXLoadPosMtxImm(view, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    /* Each capture zone's edge, on its floor, all match long. One that
     * can't score dims and breathes (ctf_zone_ring_alpha). */
    for (team = 0; team < 2; team++) {
        const CtfBox* z = &ctf_base(team)->zone;
        pc_ctf_zone_ring(z->l, z->r, z->b, CTF_ZONE_RING_DEPTH, -CTF_ZONE_RING_DEPTH, CTF_ZONE_RING_HEIGHT,
                         body[team], light[team], ctf_zone_ring_alpha(team));
    }
    /* READY: a light beam over each base (the camera frames both then). */
    if (ctf.frame < CTF_READY_FRAMES) {
        const float in = ctf.frame < 10 ? ctf.frame / 10.0f : 1.0f;
        const float out = ctf.frame > CTF_READY_FRAMES - 30 ? (CTF_READY_FRAMES - ctf.frame) / 30.0f : 1.0f;
        for (team = 0; team < 2; team++) {
            const CtfBase* base = ctf_base(team);
            pc_ctf_beam(base->stand.x, base->stand.y, 0.0f, 200.0f, 40.0f, body[team], light[team], in * out,
                        ctf.frame);
        }
    }
    HSD_StateInvalidate(-1);
    HSD_StateInitTev();
    HSD_ClearVtxDesc();
}

/* Through READY the camera frames both bases as well as the players, so
 * everyone sees where each team defends; at GO the subjects go and the
 * camera eases back onto the players. */

static void ctf_ready_camera(void)
{
    int team;
    for (team = 0; team < 2; team++) {
        if (ctf.frame == 1) {
            const CtfBase* base = ctf_base(team);
            CmSubject* s = Camera_80029044(CmSubjectState_Active);
            if (s != NULL) {
                s->pos.x = base->stand.x;
                s->pos.y = base->stand.y;
                s->pos.z = 0.0f;
                s->target_ext.h.x = s->target_ext.h.y = 12.0f;
                s->target_ext.v.x = 26.0f; /* the flag above the stand */
                s->target_ext.v.y = 4.0f;
                s->target_ext.v.z = 0.0f;
                s->ext = s->target_ext;
            }
            ctf.cam[team] = s;
        } else if (ctf.frame == CTF_READY_FRAMES && ctf.cam[team] != NULL) {
            Camera_800290D4(ctf.cam[team]);
            ctf.cam[team] = NULL;
        }
    }
}

static void ctf_tick(HSD_GObj* gobj)
{
    ctf.frame++;
    if (gmCtf_Active()) {
        ctf_ready_camera();
    }
    if (ctf.frame == 1 && gmCtf_Active() && getenv("MELEE_DEBUG_CTF_NOHUD") == NULL) {
        ifCtf_Create(); /* MELEE_DEBUG_CTF_NOHUD: clean frames for HUD mockups */
    }
    if (ctf.frame == 2) {
        ctf_survey();
    }
    ctf_debug_spawn();
    if (gmCtf_Active()) {
        ctf_update_flags();
        ctf_check_captures();
    }
    ctf_update_carriers();
    ctf_debug_log();
}

#endif /* TARGET_PC */

/* ---- the interface ---------------------------------------------------------- */

void gmCtf_SetRules(const GmCtfRules* rules)
{
#ifdef TARGET_PC
    ctf_requested = rules != NULL;
    if (rules != NULL) {
        ctf_rules = *rules;
    }
#endif
}

bool gmCtf_Active(void)
{
#ifdef TARGET_PC
    return ctf_requested && ctf_layout() != NULL;
#else
    return false;
#endif
}

void gmCtf_MatchInit(void)
{
#ifdef TARGET_PC
    memset(&ctf, 0, sizeof ctf);
    ctf.winner = -1;
    if (gmCtf_Active() || getenv("MELEE_DEBUG_CTF_LOG") != NULL || getenv("MELEE_DEBUG_CTF_FLAG") != NULL ||
        getenv("MELEE_DEBUG_CTF_SURVEY") != NULL)
    {
        HSD_GObj* g = GObj_Create(4, 7, 0);
        HSD_GObj_SetupProc(g, ctf_tick, 0x14);
        if (gmCtf_Active()) {
            GObj_SetupGXLink(g, ctf_draw_stands, 6, 0);
        }
    }
    if (gmCtf_Active()) {
        OSReport("ctf: match on stage %d, first to %d, home required %d, return %d frames, swap %d\n",
                 gm_GetStKind(), ctf_rules.captures_to_win, ctf_rules.home_required, ctf_rules.return_frames,
                 ctf_rules.swap_sides);
    }
#endif
}

bool gmCtf_SpawnPoint(int slot, Vec3* out)
{
#ifdef TARGET_PC
    const CtfBase* base;
    int i, nth = 0;
    int team;

    if (!gmCtf_Active()) {
        return false;
    }
    team = ctf_team_of_slot(slot);
    if ((base = ctf_base(team)) == NULL) {
        return false;
    }
    for (i = 0; i < slot; i++) {
        if (Player_GetPlayerSlotType(i) != Gm_PKind_NA && ctf_team_of_slot(i) == team) {
            nth++;
        }
    }
    out->x = base->spawn[nth & 1].x;
    out->y = base->spawn[nth & 1].y;
    out->z = 0.0f;
    return true;
#else
    return false;
#endif
}

void gmCtf_Respawn(int slot)
{
#ifdef TARGET_PC
    Vec3 pos;
    const int point = Player_80032F30(slot);
    if (!gmCtf_SpawnPoint(slot, &pos)) {
        return;
    }
    if (point != -1) {
        /* Keep Melee's revival point and offset it onto the base: a descent
         * to a stage point passes through the stage, while one to a bare
         * position collides with it (ftCo_Rebirth_Coll) and would stop on
         * the roof of red's cave. */
        Vec3 rp, off;
        Stage_80224E38(&rp, point);
        off.x = pos.x - rp.x;
        off.y = pos.y - rp.y;
        off.z = 0.0f;
        Player_SetSomePos(slot, &off);
    } else {
        Player_SetSpawnPlatformPos(slot, &pos);
    }
#endif
}

bool gmCtf_PickupBlocked(HSD_GObj* fighter, HSD_GObj* item)
{
#ifdef TARGET_PC
    int team;
    if (!gmCtf_Active() || !itCtfFlag_Is(item)) {
        return false;
    }
    team = GET_ITEM(item)->xDD4_itemVar.ctfflag.team & 1;
    return ctf.flag[team] == item && ctf.home[team] && ctf_team_of_fighter(fighter) == team;
#else
    return false;
#endif
}

void gmCtf_FlagDestroyed(HSD_GObj* item)
{
#ifdef TARGET_PC
    int team;
    for (team = 0; team < 2; team++) {
        if (ctf.flag[team] == item) {
            ctf.flag[team] = NULL; /* the next tick puts it back on its stand */
        }
    }
    if (ctf.stray == item) {
        ctf.stray = NULL;
    }
#endif
}

bool gmCtf_GetHud(GmCtfHud* out)
{
#ifdef TARGET_PC
    int team;
    if (!gmCtf_Active()) {
        return false;
    }
    for (team = 0; team < 2; team++) {
        HSD_GObj* holder;
        Vec3 pos;
        out->captures[team] = ctf.captures[team];
        out->home[team] = ctf.home[team];
        out->present[team] = ctf_flag_pos(ctf.flag[team], &pos, &holder);
        out->holder[team] = holder != NULL ? (s8) GET_FIGHTER(holder)->player_idx : -1;
        out->return_left[team] =
            ctf.flag[team] != NULL ? GET_ITEM(ctf.flag[team])->xDD4_itemVar.ctfflag.return_left : 0;
        out->base[team] = ctf_base(team)->name;
    }
    out->swap = ctf_rules.swap_sides != 0;
    out->return_frames = ctf_rules.return_frames;
    out->frame = ctf.frame;
    return true;
#else
    return false;
#endif
}

bool gmCtf_CarrySlow(void)
{
#ifdef TARGET_PC
    return !gmCtf_Active() || ctf_rules.carry_slow;
#else
    return false;
#endif
}

int gmCtf_Winner(void)
{
#ifdef TARGET_PC
    return gmCtf_Active() ? ctf.winner : -1;
#else
    return -1;
#endif
}

bool gmCtf_Tied(void)
{
#ifdef TARGET_PC
    return gmCtf_Active() && ctf.captures[0] == ctf.captures[1];
#else
    return false;
#endif
}

int gmCtf_TeamScore(int melee_team)
{
#ifdef TARGET_PC
    return ctf.captures[melee_team == 1 ? 1 : 0];
#else
    return 0;
#endif
}

u32 gmCtf_Checksum(void)
{
#ifdef TARGET_PC
    u32 h = 0;
    int team;
    if (!gmCtf_Active()) {
        return 0;
    }
    for (team = 0; team < 2; team++) {
        HSD_GObj* holder;
        Vec3 pos = { 0.0f, 0.0f, 0.0f };
        ctf_flag_pos(ctf.flag[team], &pos, &holder);
        h = h * 31u + ctf.captures[team];
        h = h * 31u + (u32) ctf.home[team];
        h = h * 31u + (holder != NULL ? (u32) GET_FIGHTER(holder)->player_idx + 1 : 0u);
        h = h * 31u + (u32) (s32) (pos.x * 16.0f);
        h = h * 31u + (u32) (s32) (pos.y * 16.0f);
    }
    return h * 31u + (u32) (ctf.winner + 1);
#else
    return 0;
#endif
}
