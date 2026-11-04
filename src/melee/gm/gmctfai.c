/* CPU players for Capture the Flag (docs/ctf-ai-plan.md).
 *
 * The rules for this file (plan §5.2): its state is statics here, which the
 * rollback snapshot covers (src/pc/melee_state.ld); no item pointer is kept
 * across ticks (a flag is destroyed and respawned on every send-home); it
 * reads only the sim; game math only; integer path costs; and nothing runs
 * outside a CTF match, so stock VS and Slippi replays are untouched.
 *
 * Debug: MELEE_DEBUG_CTF_AI_LOG=1 writes "ctfev:" event lines (KOs, stuck
 * CPUs) and a "ctfpos:" line of everyone's position every 30 frames, which
 * tools/ctf_ai_test.py turns into metrics. Logging writes no state. */

#include "gmctfai.h"

#include "gmctf.h"

#include <Runtime/platform.h>

#include <dolphin/os.h>
#include <melee/ft/fighter.h>
#include <melee/ft/ftlib.h>
#include <melee/ft/types.h>
#include <melee/gm/gmvs.h>
#include <melee/it/inlines.h>
#include <melee/it/types.h>
#include <melee/pl/player.h>

#ifdef TARGET_PC

#include <stdio.h>
#include <string.h>

/* A KO with no damage taken in this long before it is a self-destruct. */
#define CTFAI_SD_FRAMES 180
/* A CPU that hasn't moved in this long while short of its destination is
 * stuck (cpu.x84 counts frames without movement; stock AI never reads it). */
#define CTFAI_STUCK_FRAMES 180
#define CTFAI_POS_EVERY 30

/* ---- state: all of it rolls back ----------------------------------------- */

static struct {
    /* The event lines' bookkeeping. */
    bool dead[4];
    bool stuck[4];
    float percent[4];
    int hurt_frame[4]; /* the last frame this port took damage */
} ai;

/* ---- helpers ---------------------------------------------------------------- */

static Fighter* ai_fighter(int slot)
{
    HSD_GObj* g;
    if (Player_GetPlayerSlotType(slot) == Gm_PKind_NA) {
        return NULL;
    }
    g = Player_GetEntity(slot);
    return g != NULL ? GET_FIGHTER(g) : NULL;
}

static bool ai_is_cpu(int slot)
{
    return Player_GetPlayerSlotType(slot) == Gm_PKind_Cpu;
}

static bool ai_alive(Fighter* fp)
{
    return fp != NULL && !ftLib_IsDead(fp->gobj) && !ftLib_IsRebirth(fp->gobj);
}

static float ai_dist2(const Vec3* a, float x, float y)
{
    const float dx = a->x - x, dy = a->y - y;
    return dx * dx + dy * dy;
}

/* One flag as the AI sees it this tick. */
typedef struct CtfAiFlag {
    bool present; /* the item exists (not on the frame after a loss) */
    bool home;
    int holder;   /* the carrier's port, or -1 */
    Vec3 pos;
    Vec2 stand;
} CtfAiFlag;

static void ai_flag(int team, CtfAiFlag* f)
{
    float l, r, b, t;
    f->present = gmCtf_FlagWhere(team, &f->pos, &f->holder, &f->home);
    if (!f->present) {
        f->holder = -1;
        f->home = true;
    }
    gmCtf_Base(team, &f->stand, &l, &r, &b, &t);
}

/* The nearest living enemy of `fp`, and its squared distance. */
static Fighter* ai_nearest_enemy(Fighter* fp, int team, float* d2)
{
    Fighter* best = NULL;
    int slot;
    *d2 = 1e12f;
    for (slot = 0; slot < 4; slot++) {
        Fighter* e = ai_fighter(slot);
        float d;
        if (e == NULL || gmCtf_TeamOfSlot(slot) == team || !ai_alive(e)) {
            continue;
        }
        d = ai_dist2(&fp->cur_pos, e->cur_pos.x, e->cur_pos.y);
        if (d < *d2) { /* ties: the lower port, scanned first */
            *d2 = d;
            best = e;
        }
    }
    return best;
}

/* Is `fp` the CPU of its team nearest to (x, y)? Ties go to the lower port. */
static bool ai_nearest_of_team(Fighter* fp, int team, float x, float y)
{
    const float mine = ai_dist2(&fp->cur_pos, x, y);
    int slot;
    for (slot = 0; slot < 4; slot++) {
        Fighter* o = ai_fighter(slot);
        float d;
        if (o == NULL || o == fp || !ai_is_cpu(slot) || gmCtf_TeamOfSlot(slot) != team || !ai_alive(o)) {
            continue;
        }
        d = ai_dist2(&o->cur_pos, x, y);
        if (d < mine || (d == mine && slot < fp->player_idx)) {
            return false;
        }
    }
    return true;
}

static Item* ai_flag_item(int team)
{
    HSD_GObj* g = gmCtf_FlagItem(team);
    return g != NULL ? GET_ITEM(g) : NULL;
}

/* ---- the event lines ---------------------------------------------------------- */

static void ai_log_events(int frame)
{
    char line[160];
    int n = 0;
    int slot;

    for (slot = 0; slot < 4; slot++) {
        Fighter* fp = ai_fighter(slot);
        bool dead;
        if (fp == NULL) {
            continue;
        }
        dead = ftLib_IsDead(fp->gobj);
        if (fp->dmg.x1830_percent > ai.percent[slot]) {
            ai.hurt_frame[slot] = frame;
        }
        ai.percent[slot] = fp->dmg.x1830_percent;
        if (dead && !ai.dead[slot]) {
            OSReport("ctfev: f%d ko p%d team%d sd%d at %.0f %.0f\n", frame, slot, gmCtf_TeamOfSlot(slot),
                     frame - ai.hurt_frame[slot] > CTFAI_SD_FRAMES, fp->cur_pos.x, fp->cur_pos.y);
        }
        ai.dead[slot] = dead;
        if (ai_is_cpu(slot)) {
            const float dx = fp->cpu.x54.x - fp->cur_pos.x, dy = fp->cpu.x54.y - fp->cur_pos.y;
            const bool short_of = dx * dx + dy * dy > (fp->cpu.x38 + 10.0f) * (fp->cpu.x38 + 10.0f);
            const bool stuck = !dead && !ftLib_IsRebirth(fp->gobj) && short_of &&
                               fp->cpu.x84 >= CTFAI_STUCK_FRAMES;
            if (stuck && !ai.stuck[slot]) {
                OSReport("ctfev: f%d stuck p%d at %.0f %.0f dest %.0f %.0f\n", frame, slot, fp->cur_pos.x,
                         fp->cur_pos.y, fp->cpu.x54.x, fp->cpu.x54.y);
            }
            ai.stuck[slot] = stuck;
        }
        if (frame % CTFAI_POS_EVERY == 0 && n < (int) sizeof line - 40) {
            n += snprintf(&line[n], sizeof line - n, " p%d %.0f %.0f %d", slot, fp->cur_pos.x, fp->cur_pos.y,
                          dead);
        }
    }
    if (n > 0) {
        /* t: the match's own frame count (gmvs.c), whose zero is GO. */
        OSReport("ctfpos: f%d t%u%s\n", frame, gm_GetFrameCount(), line);
    }
}

/* ---- the orders ------------------------------------------------------------------ */

/* Bots fight what is in their way, not across the map: an enemy this close
 * turns the attack masks on (plan §4.2). */
#define CTFAI_FIGHT_RADIUS 40.0f

static void ai_order_base(Fighter* fp, int team, GmCtfAiOrder* o)
{
    float d2;
    memset(o, 0, sizeof *o);
    o->defend = true;
    o->target = ai_nearest_enemy(fp, team, &d2);
    o->fight = o->target != NULL && d2 < CTFAI_FIGHT_RADIUS * CTFAI_FIGHT_RADIUS;
}

#endif /* TARGET_PC */

/* ---- the interface ------------------------------------------------------------ */

bool gmCtfAi_Think(Fighter* fp, GmCtfAiOrder* o)
{
#ifdef TARGET_PC
    CtfAiFlag own, theirs;
    int team, enemy;

    /* Nana follows Popo, as in VS. */
    if (!gmCtf_Active() || fp->is_sub_fighter) {
        return false;
    }
    team = gmCtf_TeamOfSlot(fp->player_idx);
    enemy = team ^ 1;
    ai_flag(team, &own);
    ai_flag(enemy, &theirs);
    ai_order_base(fp, team, o);

    /* The carrier: home, attacks off (any attack while holding is a
     * throw), shield and jumps on. */
    if (theirs.holder == fp->player_idx) {
        o->go = CTFAI_GO_POINT;
        o->point = own.stand;
        o->tol = 5.0f;
        o->fight = false;
        return true;
    }
    /* Our flag lying away from home: the nearest of us touches it. */
    if (own.present && !own.home && own.holder < 0 && ai_nearest_of_team(fp, team, own.pos.x, own.pos.y)) {
        o->go = CTFAI_GO_ITEM;
        o->item = ai_flag_item(team);
        o->pickup = true;
        return o->item != NULL;
    }
    /* Everyone else attacks: fetch their flag, unless one of us has it. */
    if (theirs.present && (theirs.holder < 0 || gmCtf_TeamOfSlot(theirs.holder) != team)) {
        o->go = CTFAI_GO_ITEM;
        o->item = ai_flag_item(enemy);
        o->pickup = true;
        return o->item != NULL;
    }
    return false;
#else
    return false;
#endif
}

void gmCtfAi_MatchInit(void)
{
#ifdef TARGET_PC
    int slot;
    memset(&ai, 0, sizeof ai);
    for (slot = 0; slot < 4; slot++) {
        ai.hurt_frame[slot] = -CTFAI_SD_FRAMES;
    }
#endif
}

void gmCtfAi_Tick(void)
{
#ifdef TARGET_PC
    if (gmCtf_EventLog()) {
        ai_log_events(gmCtf_Frame());
    }
#endif
}

u32 gmCtfAi_Checksum(void)
{
    return 0;
}
