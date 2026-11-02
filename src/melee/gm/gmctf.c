/* Capture the Flag, in the sim (docs/xeleenet-modes-plan.md §3).
 *
 * M0 spike: MELEE_DEBUG_CTF_FLAG spawns one flag at match start, so the
 * item can be picked up, thrown and landed by hand or by a frame script.
 * "1" drops it beside player 1's start; "x,y" puts it there. */

#include "gmctf.h"

#include <Runtime/platform.h>

#include <dolphin/os.h>
#include <melee/ft/fighter.h>
#include <melee/ft/ftchangeparam.h>
#include <melee/ft/inlines.h>
#include <melee/ft/types.h>
#include <melee/gr/ground.h>
#include <melee/gr/stage.h>
#include <melee/it/forward.h>
#include <melee/it/inlines.h>
#include <melee/it/item.h>
#include <melee/it/kinds/itctfflag.h>
#include <melee/it/types.h>
#include <melee/mp/mplib.h>
#include <melee/mp/types.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjproc.h>

#ifdef TARGET_PC
#include <stdlib.h>
#include <string.h>
#endif

#ifdef TARGET_PC
/* The match's flag (M0: the one debug flag). Statics of the game library,
 * so the rollback snapshot carries them. */
static HSD_GObj* ctf_flag;
static int ctf_frame;
static bool ctf_spawned;
/* Per port and fighter (Nana is index 1): whether its attributes were last
 * built carrying a flag (the carry slowdown, ftCo_800D105C). */
static bool ctf_carry[4][2];

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
            if (holding != ctf_carry[slot][idx]) {
                ctf_carry[slot][idx] = holding;
                ftCo_800D105C(g);
            }
        }
    }
}

/* MELEE_DEBUG_CTF_FLAG: one flag beside port 1 ("1"), or at "x,y". */
static void ctf_debug_spawn(void)
{
    const char* env = getenv("MELEE_DEBUG_CTF_FLAG");
    HSD_GObj* p1 = Player_GetEntity(0);
    SpawnItem spawn;
    Vec3 pos;
    char* end;
    double x;

    if (ctf_spawned || env == NULL || *env == '\0' || p1 == NULL) {
        return;
    }
    ctf_spawned = true;
    pos = GET_FIGHTER(p1)->cur_pos;
    pos.x += 15.0f;
    pos.y += 10.0f;
    x = strtod(env, &end);
    if (end != env && *end == ',') {
        pos.x = (float) x;
        pos.y = (float) strtod(end + 1, NULL);
    }
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
    ctf_flag = Item_80268B18(&spawn);
    if (ctf_flag != NULL && getenv("MELEE_DEBUG_CTF_TEAM") != NULL) {
        GET_ITEM(ctf_flag)->xDD4_itemVar.ctfflag.team = (u8) (atoi(getenv("MELEE_DEBUG_CTF_TEAM")) & 1);
    }
    OSReport("ctf: flag spawn at %.1f %.1f -> %s\n", pos.x, pos.y, ctf_flag != NULL ? "ok" : "refused");
}

/* MELEE_DEBUG_CTF_SURVEY=1: the stage's collision lines, bounds and named
 * points, once, as "ctf-survey:" lines (tools draw them to scale). */
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

/* Once per frame in a match. MELEE_DEBUG_CTF_LOG=N: every N frames, port 1's
 * position and the flag's (spike and survey diagnostics). */
static void ctf_tick(HSD_GObj* gobj)
{
    static int every = -1;
    if (every < 0) {
        const char* e = getenv("MELEE_DEBUG_CTF_LOG");
        every = e != NULL ? atoi(e) : 0;
    }
    ctf_frame++;
    if (ctf_frame == 2) {
        ctf_survey();
    }
    ctf_debug_spawn();
    ctf_update_carriers();
    if (every <= 0 || ctf_frame % every != 0) {
        return;
    }
    {
        HSD_GObj* p1 = Player_GetEntity(0);
        if (p1 != NULL) {
            Fighter* fp = GET_FIGHTER(p1);
            OSReport("ctf: f%d p1 %.1f %.1f holding=%d motion=%d walk=%.3f dash=%.3f\n", ctf_frame,
                     fp->cur_pos.x, fp->cur_pos.y, fp->item_gobj != NULL, fp->motion_id,
                     fp->co_attrs.walk_max_vel, fp->co_attrs.dash_max_velocity);
        }
        if (ctf_flag != NULL) {
            Item* ip = ctf_flag->user_data;
            OSReport("ctf: f%d flag %.1f %.1f state=%d\n", ctf_frame, ip->pos.x, ip->pos.y, ip->msid);
        }
    }
}
#endif

void gmCtf_MatchInit(void)
{
#ifdef TARGET_PC
    ctf_flag = NULL;
    ctf_frame = 0;
    ctf_spawned = false;
    memset(ctf_carry, 0, sizeof ctf_carry);
    if (getenv("MELEE_DEBUG_CTF_LOG") != NULL || getenv("MELEE_DEBUG_CTF_FLAG") != NULL ||
        getenv("MELEE_DEBUG_CTF_SURVEY") != NULL)
    {
        HSD_GObj* g = GObj_Create(4, 7, 0);
        HSD_GObj_SetupProc(g, ctf_tick, 0x14);
    }
#endif
}
