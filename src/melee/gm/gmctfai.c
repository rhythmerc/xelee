/* CPU players for Capture the Flag (docs/ctf-ai-plan.md).
 *
 * Three layers (plan §3c): a team planner hands out roles after Quake III's
 * CTF orders (attack, defend, hunt the enemy carrier, return our flag, carry,
 * escort; §4.1); each role turns into an order for Melee's own CPU AI, which
 * still fights, shields, recovers and walks (ftCo_CtfThink in ftCo_0A01.c).
 *
 * The rules for this file (plan §5.2): its state is statics here, which the
 * rollback snapshot covers (src/pc/melee_state.ld); no item pointer is kept
 * across ticks (a flag is destroyed and respawned on every send-home); it
 * reads only the sim; game math only; integer path costs; its own random
 * stream; and nothing runs outside a CTF match, so stock VS and Slippi
 * replays are untouched.
 *
 * Debug: MELEE_DEBUG_CTF_AI_LOG=1 writes "ctfev:" event lines (KOs, stuck
 * CPUs, role changes) and a "ctfpos:" line of everyone's position every 30
 * frames, which tools/ctf_ai_test.py turns into metrics. Logging writes no
 * state. */

#include "gmctfai.h"

#include "gmctf.h"
#include "gmctfnav.h"

#include <Runtime/platform.h>

#include <dolphin/os.h>
#include <melee/ft/fighter.h>
#include <melee/ft/ftlib.h>
#include <melee/ft/types.h>
#include <melee/gm/gmvs.h>
#include <melee/it/inlines.h>
#include <melee/it/types.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/random.h>

#ifdef TARGET_PC

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A KO with no damage taken in this long before it is a self-destruct. */
#define CTFAI_SD_FRAMES 180
/* A CPU that hasn't moved in this long while short of its destination is
 * stuck (cpu.x84 counts frames without movement; stock AI never reads it),
 * if it had that goal the whole time: a bot holding in its zone or at its
 * guard spot that is handed a far goal hasn't been stuck. */
#define CTFAI_STUCK_FRAMES 180
#define CTFAI_POS_EVERY 30

/* Bots fight what is in their way, not across the map: an enemy this close
 * turns the attack masks on (plan §4.2). */
#define CTFAI_FIGHT_RADIUS 40.0f
/* A defender engages enemies this close to its stand, and never chases one
 * farther than the leash (UT's "defenders never chase", plan §4.5); an enemy
 * carrier within the leash plus CTFAI_CARRIER_REACH is hunted anyway. */
#define CTFAI_GUARD_RADIUS 60.0f
#define CTFAI_CARRIER_REACH 60.0f
/* A role is kept unless another bot is this much closer to it (hysteresis,
 * so two bots don't swap roles every plan as they move). */
#define CTFAI_ROLE_STICK 40.0f

enum {
    ROLE_NONE,
    ROLE_ATTACK,
    ROLE_DEFEND,
    ROLE_HUNT,
    ROLE_RETURN,
    ROLE_CARRY,
    ROLE_ESCORT,
};
static const char* const ai_role_name[] = { "none", "attack", "defend", "hunt", "return", "carry", "escort" };

/* The flag situation, from one team's side (plan §4.1's rows). Their flag
 * free is played as their flag home: attackers fetch it either way. */
enum {
    SIT_BOTH_HOME,
    SIT_WE_CARRY,     /* we carry theirs, ours is home */
    SIT_OURS_FREE,    /* ours lies away from home, theirs is not carried */
    SIT_OURS_CARRIED, /* they carry ours, we don't carry theirs */
    SIT_BOTH_CARRIED,
    SIT_OURS_FREE_WE_CARRY,
};

/* Per base (the layout's first and second, before a side swap): where a
 * defender waits, between the stand and the enemy's way in. Hyrule Temple's
 * red cave is entered from the right, blue's right edge from the left. */
static const Vec2 ai_guard_temple[2] = { { -85.0f, -88.6f }, { 180.0f, -6.1f } };

/* ---- state: all of it rolls back ----------------------------------------- */

static struct {
    u8 role[4];
    u8 sit[2];          /* the situation the team last planned for */
    bool avail[4];      /* last plan's idea of who could play */
    int plan_at[2];     /* the team plans again on this frame */
    u32 rng[2];         /* each team's own random stream (plan §5.2 rule 4) */
    u8 captures[2];     /* the team's captures as last seen */
    int scored_at[2];   /* the frame of its last capture (0: the match start) */
    /* Where each bot's order sends it (the point, the item, the fighter it
     * follows), for the stuck check: once a bot arrives, Melee's idle
     * behaviour sets cpu.x54 to wander points of its own (state 0xA). */
    Vec2 goal[4];
    float goal_tol[4];
    bool has_goal[4];
    int goal_since[4]; /* the frame the goal last moved more than 30 units */
    int hold_since[4]; /* a carrier holding in its zone since, or 0 */
    /* Navigation (gmctfnav.c): each fighter's area when last on the ground;
     * each bot's goal node, its way point while a path is on (kept through
     * a jump: re-aiming mid-air at the node it jumped from would undo the
     * jump), an alternate route's node, a node to route around after a
     * failure, and its progress (path cost) for the stuck check. */
    s8 area[4];
    s8 nav_goal[4];
    s8 nav_via[4];
    s8 nav_avoid[4];
    bool nav_on[4];
    Vec2 nav_pt[4];
    s8 nav_node[4];
    int avoid_until[4];
    int best_cost[4];
    int best_at[4];
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

/* A plain distance for costs, from the game's sqrtf (MSL's on every target,
 * src/pc/compat.h). */
static float ai_dist(const Vec3* a, float x, float y)
{
    return sqrtf(ai_dist2(a, x, y));
}

static int ai_rand(int team, int n)
{
    ai.rng[team] = ai.rng[team] * 1103515245u + 12345u;
    return (int) ((ai.rng[team] >> 16) % (u32) n);
}

/* The team's level: its strongest CPU's, 1-9. */
static int ai_team_level(int team)
{
    int slot, level = 1;
    for (slot = 0; slot < 4; slot++) {
        Fighter* fp = ai_fighter(slot);
        if (fp != NULL && ai_is_cpu(slot) && gmCtf_TeamOfSlot(slot) == team && fp->cpu.level > level) {
            level = fp->cpu.level;
        }
    }
    return level > 9 ? 9 : level;
}

/* MELEE_DEBUG_CTF_AI_PACIFIST=1: bots never attack (tools/ctf_ai_test.py's
 * trip test, where the idle opponent stands at the trip's far end). Read
 * once; the same on every peer. */
static bool ai_pacifist(void)
{
    static int v = -1;
    if (v < 0) {
        v = getenv("MELEE_DEBUG_CTF_AI_PACIFIST") != NULL;
    }
    return v != 0;
}

/* Another CPU of the team is in play (alive, not the given port). */
static bool ai_team_has_other(int team, int slot)
{
    int s;
    for (s = 0; s < 4; s++) {
        if (s != slot && ai_is_cpu(s) && gmCtf_TeamOfSlot(s) == team && ai_alive(ai_fighter(s))) {
            return true;
        }
    }
    return false;
}

/* How long a lone carrier holds in its zone before parking the flag there:
 * strong bots see the stalemate sooner. */
static int ai_park_delay(int level)
{
    return level >= 9 ? 60 : 60 + (9 - level) * 30;
}

/* One flag as the AI sees it this tick. */
typedef struct CtfAiFlag {
    bool present; /* the item exists (not on the frame after a loss) */
    bool home;
    int holder;   /* the carrier's port, or -1 */
    Vec3 pos;
    Vec2 stand;
    float zl, zr, zb, zt; /* its team's capture zone */
} CtfAiFlag;

static void ai_flag(int team, CtfAiFlag* f)
{
    f->present = gmCtf_FlagWhere(team, &f->pos, &f->holder, &f->home);
    if (!f->present) {
        f->holder = -1;
        f->home = true;
    }
    gmCtf_Base(team, &f->stand, &f->zl, &f->zr, &f->zb, &f->zt);
}

/* (x, y) is in the capture zone of the team whose flag `f` is (with the
 * rules' slack under a zone's bottom, gmctf.c CTF_ZONE_FLOOR_SLACK). */
static bool ai_in_zone(const CtfAiFlag* f, float x, float y)
{
    return x >= f->zl && x <= f->zr && y >= f->zb - 4.0f && y <= f->zt;
}

/* Their flag lies free in our zone: it scores the moment ours is home (the
 * rules count a free flag in the zone, gmctf.c ctf_check_captures), so
 * nobody of ours should pick it up again. */
static bool ai_free(const CtfAiFlag* f)
{
    return f->present && !f->home && f->holder < 0;
}

static bool ai_parked(const CtfAiFlag* own, const CtfAiFlag* theirs)
{
    return ai_free(theirs) && ai_in_zone(own, theirs->pos.x, theirs->pos.y);
}

static Fighter* ai_holder(const CtfAiFlag* f)
{
    return f->holder >= 0 ? ai_fighter(f->holder) : NULL;
}

/* The nearest living enemy of team `team` to (x, y), and its squared
 * distance. Ties go to the lower port, scanned first. */
static Fighter* ai_nearest_enemy_to(int team, float x, float y, float* d2)
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
        d = ai_dist2(&e->cur_pos, x, y);
        if (d < *d2) {
            *d2 = d;
            best = e;
        }
    }
    return best;
}

/* An enemy is close enough to be fighting (the stuck checks leave a bot in
 * a fight alone: standing still while shielding isn't being lost). */
static bool ai_fighting(Fighter* fp, int team)
{
    float d2;
    return ai_nearest_enemy_to(team, fp->cur_pos.x, fp->cur_pos.y, &d2) != NULL &&
           d2 < CTFAI_FIGHT_RADIUS * CTFAI_FIGHT_RADIUS;
}

static Item* ai_flag_item(int team)
{
    HSD_GObj* g = gmCtf_FlagItem(team);
    return g != NULL ? GET_ITEM(g) : NULL;
}

static Vec2 ai_guard(int team, const CtfAiFlag* own)
{
    if (gmCtf_RealLayout()) {
        return ai_guard_temple[gmCtf_BaseOf(team)];
    }
    return own->stand;
}

/* ---- the team planner (plan §4.1) ------------------------------------------ */

/* Plans run this often, and this long after a flag event (plan §4.8),
 * by level 1-9. */
static const u8 ai_plan_period[10] = { 60, 60, 52, 44, 36, 24, 20, 16, 12, 8 };
static const u8 ai_notice_delay[10] = { 60, 60, 50, 40, 30, 20, 14, 8, 4, 2 };

static int ai_situation(int team, const CtfAiFlag* own, const CtfAiFlag* theirs)
{
    const bool we_carry = theirs->holder >= 0 && gmCtf_TeamOfSlot(theirs->holder) == team;
    const bool ours_carried = own->holder >= 0;
    const bool ours_free = ai_free(own);
    if (we_carry) {
        return ours_carried ? SIT_BOTH_CARRIED : ours_free ? SIT_OURS_FREE_WE_CARRY : SIT_WE_CARRY;
    }
    return ours_carried ? SIT_OURS_CARRIED : ours_free ? SIT_OURS_FREE : SIT_BOTH_HOME;
}

/* Plan §4.1's table: the roles of a team of n (1-3) players in a situation,
 * the carrier's first when there is one. */
static int ai_table(int sit, int n, int level, bool aggressive, u8* out)
{
    static const u8 rows[6][3][3] = {
        /* SIT_BOTH_HOME */
        { { ROLE_ATTACK }, { ROLE_DEFEND, ROLE_ATTACK }, { ROLE_DEFEND, ROLE_ATTACK, ROLE_ATTACK } },
        /* SIT_WE_CARRY */
        { { ROLE_CARRY }, { ROLE_CARRY, ROLE_DEFEND }, { ROLE_CARRY, ROLE_DEFEND, ROLE_ESCORT } },
        /* SIT_OURS_FREE */
        { { ROLE_RETURN }, { ROLE_RETURN, ROLE_ATTACK }, { ROLE_RETURN, ROLE_DEFEND, ROLE_ATTACK } },
        /* SIT_OURS_CARRIED */
        { { ROLE_HUNT }, { ROLE_HUNT, ROLE_ATTACK }, { ROLE_HUNT, ROLE_HUNT, ROLE_ATTACK } },
        /* SIT_BOTH_CARRIED */
        { { ROLE_CARRY }, { ROLE_CARRY, ROLE_HUNT }, { ROLE_CARRY, ROLE_HUNT, ROLE_HUNT } },
        /* SIT_OURS_FREE_WE_CARRY */
        { { ROLE_CARRY }, { ROLE_CARRY, ROLE_RETURN }, { ROLE_CARRY, ROLE_RETURN, ROLE_HUNT } },
    };
    int i;
    n = n < 1 ? 1 : n > 3 ? 3 : n;
    for (i = 0; i < n; i++) {
        out[i] = rows[sit][n - 1][i];
    }
    /* Weak teams of three play safe: two defenders (plan §4.1). */
    if (sit == SIT_BOTH_HOME && n == 3 && level <= 4) {
        out[1] = ROLE_DEFEND;
    }
    /* Q3's aggressive flip (plan §4.8): a pair that hasn't scored in a
     * while sends both bots. */
    if (sit == SIT_BOTH_HOME && n == 2 && aggressive) {
        out[0] = ROLE_ATTACK;
    }
    return n;
}

/* How long a pair goes without scoring before both attack, by level (7-9;
 * weaker teams never flip). Q3 flips after minutes; on Temple a lone
 * attacker against a defender mostly drops the flag on the way out (C2's
 * 2v2 runs: 16 drops to 3 captures). Measured over 6 seeds of 2v2: no flip
 * 1.39 captures a minute, always both attacking 2.37, flipping after 20 s
 * 1.92, after 45 s 1.89 (which keeps a defender for most of the time). */
static int ai_flip_frames(int level)
{
    return level >= 9 ? 45 * 60 : level == 8 ? 60 * 60 : 90 * 60;
}

/* How far a bot is from a point: the path's length on the nav graph
 * (gmctfnav.c, in units), or a straight line on a map without one. */
static float ai_travel(Fighter* fp, float x, float y)
{
    int area, goal, cost;
    if (!gmCtfNav_Active()) {
        return ai_dist(&fp->cur_pos, x, y);
    }
    area = ai.area[fp->player_idx] >= 0 ? ai.area[fp->player_idx]
                                         : gmCtfNav_NodeArea(gmCtfNav_NodeIn(-1, fp->cur_pos.x, fp->cur_pos.y));
    goal = gmCtfNav_NodeIn(-1, x, y);
    cost = gmCtfNav_Cost(gmCtfNav_Class(fp), area, fp->cur_pos.x, fp->cur_pos.y, goal);
    return (float) cost / 10.0f;
}

/* How well a bot fits a role: the way to its object (lower is better). */
static float ai_role_cost(Fighter* fp, int role, const CtfAiFlag* own, const CtfAiFlag* theirs)
{
    Fighter* h;
    switch (role) {
    case ROLE_RETURN:
        return ai_travel(fp, own->pos.x, own->pos.y);
    case ROLE_DEFEND:
        return ai_travel(fp, own->stand.x, own->stand.y);
    case ROLE_HUNT:
        h = ai_holder(own);
        return h != NULL ? ai_travel(fp, h->cur_pos.x, h->cur_pos.y) : 0.0f;
    case ROLE_ESCORT:
        h = ai_holder(theirs);
        return h != NULL ? ai_travel(fp, h->cur_pos.x, h->cur_pos.y) : 0.0f;
    case ROLE_ATTACK:
        return ai_travel(fp, theirs->pos.x, theirs->pos.y);
    default:
        return 0.0f;
    }
}

static void ai_set_role(int slot, int role, int frame)
{
    if (ai.role[slot] != role) {
        if (gmCtf_EventLog()) {
            OSReport("ctfev: f%d role p%d %s\n", frame, slot, ai_role_name[role]);
        }
        ai.role[slot] = (u8) role;
    }
}

static void ai_plan(int team, int frame)
{
    /* Who fills a role first: the most urgent. */
    static const u8 order[] = { ROLE_RETURN, ROLE_DEFEND, ROLE_HUNT, ROLE_ESCORT, ROLE_ATTACK };
    CtfAiFlag own, theirs;
    u8 roles[3];
    int bots[4], nbots = 0, carrier = -1;
    bool taken[4] = { false, false, false, false };
    int n, i, k, slot, sit;
    const int level = ai_team_level(team);

    ai_flag(team, &own);
    ai_flag(team ^ 1, &theirs);
    sit = ai_situation(team, &own, &theirs);
    ai.sit[team] = (u8) sit;
    for (slot = 0; slot < 4; slot++) {
        Fighter* fp = ai_fighter(slot);
        if (fp == NULL || !ai_is_cpu(slot) || gmCtf_TeamOfSlot(slot) != team) {
            continue;
        }
        ai.avail[slot] = ai_alive(fp);
        if (!ai.avail[slot]) {
            ai_set_role(slot, ROLE_NONE, frame);
        } else if (slot == theirs.holder) {
            carrier = slot;
        } else {
            bots[nbots++] = slot;
        }
    }
    if (carrier >= 0) {
        ai_set_role(carrier, ROLE_CARRY, frame);
    }
    if (nbots == 0) {
        return;
    }
    n = ai_table(sit, nbots + (carrier >= 0 ? 1 : 0), level,
                 level >= 7 && frame - ai.scored_at[team] >= ai_flip_frames(level), roles);
    /* The carrier's row entry is the carrier's; a carrier who isn't a bot of
     * ours (a human, later) takes it the same way. */
    k = 0;
    if (theirs.holder >= 0 && gmCtf_TeamOfSlot(theirs.holder) == team) {
        k = 1;
    }
    /* A lone bot returns our flag only if it beats every enemy to it, and
     * hunts their carrier only if it is near (plan §4.1's 1-bot column). */
    if (n == 1 && nbots == 1) {
        Fighter* fp = ai_fighter(bots[0]);
        float d2;
        if (roles[0] == ROLE_RETURN) {
            ai_nearest_enemy_to(team, own.pos.x, own.pos.y, &d2);
            if (ai_dist2(&fp->cur_pos, own.pos.x, own.pos.y) > d2) {
                roles[0] = ROLE_ATTACK;
            }
        }
    }
    for (i = 0; i < (int) sizeof order; i++) {
        int j;
        for (j = k; j < n; j++) {
            float best = 1e30f;
            int pick = -1, b;
            if (roles[j] != order[i]) {
                continue;
            }
            for (b = 0; b < nbots; b++) {
                Fighter* fp;
                float c;
                if (taken[b]) {
                    continue;
                }
                fp = ai_fighter(bots[b]);
                c = ai_role_cost(fp, roles[j], &own, &theirs);
                if (ai.role[bots[b]] == roles[j]) {
                    c -= CTFAI_ROLE_STICK;
                }
                if (c < best) {
                    best = c;
                    pick = b;
                }
            }
            if (pick >= 0) {
                taken[pick] = true;
                ai_set_role(bots[pick], roles[j], frame);
            }
        }
    }
    /* More bots than the table has rows for: the rest attack. */
    for (i = 0; i < nbots; i++) {
        if (!taken[i]) {
            ai_set_role(bots[i], ROLE_ATTACK, frame);
        }
    }
}

/* Once per frame: plan again on a flag event (after the level's notice
 * delay), when a bot is KO'd or comes back, and every period. */
static void ai_planner(int frame)
{
    int team;
    for (team = 0; team < 2; team++) {
        CtfAiFlag own, theirs;
        const int level = ai_team_level(team);
        bool avail_changed = false;
        int slot, sit;
        ai_flag(team, &own);
        ai_flag(team ^ 1, &theirs);
        sit = ai_situation(team, &own, &theirs);
        if (gmCtf_TeamScore(team) != ai.captures[team]) {
            ai.captures[team] = (u8) gmCtf_TeamScore(team);
            ai.scored_at[team] = frame;
        }
        for (slot = 0; slot < 4; slot++) {
            Fighter* fp = ai_fighter(slot);
            if (fp != NULL && ai_is_cpu(slot) && gmCtf_TeamOfSlot(slot) == team && ai_alive(fp) != ai.avail[slot]) {
                avail_changed = true;
            }
        }
        if (sit != ai.sit[team] && ai.plan_at[team] > frame + ai_notice_delay[level]) {
            ai.plan_at[team] = frame + (level >= 9 ? ai_rand(team, 5) : ai_notice_delay[level]);
        }
        if (avail_changed || frame >= ai.plan_at[team]) {
            ai_plan(team, frame);
            ai.plan_at[team] = frame + ai_plan_period[level];
        }
    }
}

/* ---- the orders: a role becomes this tick's order -------------------------- */

static void ai_order_base(Fighter* fp, int team, GmCtfAiOrder* o)
{
    float d2;
    memset(o, 0, sizeof *o);
    o->defend = true;
    /* Dash to far goals: a walking CPU took 3x a scripted human's time from
     * red's stand to blue's (C3's first trips, Mario: 889 frames to 290). */
    o->run = true;
    o->target = ai_nearest_enemy_to(team, fp->cur_pos.x, fp->cur_pos.y, &d2);
    o->fight = o->target != NULL && d2 < CTFAI_FIGHT_RADIUS * CTFAI_FIGHT_RADIUS && !ai_pacifist();
}

static void ai_go_point(GmCtfAiOrder* o, Vec2 p, float tol)
{
    o->go = CTFAI_GO_POINT;
    o->point = p;
    o->tol = tol;
}

static bool ai_go_flag(GmCtfAiOrder* o, int team)
{
    o->go = CTFAI_GO_ITEM;
    o->item = ai_flag_item(team);
    o->pickup = true;
    return o->item != NULL;
}

static void ai_go_fighter(GmCtfAiOrder* o, Fighter* who, bool fight)
{
    o->go = CTFAI_GO_FIGHTER;
    o->follow = who;
    if (fight) {
        o->target = who;
        o->fight = true;
    }
}

static bool ai_hunt(Fighter* fp, int team, const CtfAiFlag* own, const CtfAiFlag* theirs, GmCtfAiOrder* o);

static bool ai_attack(Fighter* fp, int team, const CtfAiFlag* own, const CtfAiFlag* theirs, GmCtfAiOrder* o)
{
    if (ai_parked(own, theirs)) {
        /* Already where it scores: get ours home instead. */
        if (ai_free(own)) {
            return ai_go_flag(o, team);
        }
        if (own->holder >= 0) {
            return ai_hunt(fp, team, own, theirs, o);
        }
        return false;
    }
    if (theirs->present && theirs->holder < 0) {
        return ai_go_flag(o, team ^ 1);
    }
    return false;
}

static bool ai_defend(Fighter* fp, int team, const CtfAiFlag* own, const CtfAiFlag* theirs, GmCtfAiOrder* o)
{
    const int level = fp->cpu.level;
    const float leash = level <= 2 ? 120.0f : 90.0f;
    Fighter* carrier = ai_holder(own);
    Fighter* intruder;
    float d2;

    /* An enemy carrier in reach is hunted (plan §4.5's exception). */
    if (carrier != NULL && level >= 5 &&
        ai_dist2(&carrier->cur_pos, own->stand.x, own->stand.y) < (leash + CTFAI_CARRIER_REACH) * (leash + CTFAI_CARRIER_REACH))
    {
        ai_go_fighter(o, carrier, true);
        return true;
    }
    intruder = ai_nearest_enemy_to(team, own->stand.x, own->stand.y, &d2);
    /* Only an intruder on the stand's own floor: one up in the shaft over
     * red's stand is out of reach, and chasing it left the stand bare (C3's
     * 2v2 runs: 37 defenders stood under one). */
    if (intruder != NULL && d2 < CTFAI_GUARD_RADIUS * CTFAI_GUARD_RADIUS &&
        ai_dist2(&fp->cur_pos, own->stand.x, own->stand.y) < leash * leash &&
        (!gmCtfNav_Active() ||
         ai.area[intruder->player_idx] == gmCtfNav_NodeArea(gmCtfNav_NodeIn(-1, own->stand.x, own->stand.y))))
    {
        ai_go_fighter(o, intruder, true);
        return true;
    }
    ai_go_point(o, ai_guard(team, own), 5.0f);
    return true;
}

static bool ai_hunt(Fighter* fp, int team, const CtfAiFlag* own, const CtfAiFlag* theirs, GmCtfAiOrder* o)
{
    Fighter* carrier = ai_holder(own);
    if (carrier == NULL) {
        return ai_attack(fp, team, own, theirs, o);
    }
    ai_go_fighter(o, carrier, ai_dist2(&fp->cur_pos, carrier->cur_pos.x, carrier->cur_pos.y) <
                                  CTFAI_FIGHT_RADIUS * CTFAI_FIGHT_RADIUS);
    return true;
}

static bool ai_escort(Fighter* fp, int team, const CtfAiFlag* own, const CtfAiFlag* theirs, GmCtfAiOrder* o)
{
    Fighter* carrier = ai_holder(theirs);
    float d2;
    if (carrier == NULL || carrier == fp) {
        return ai_attack(fp, team, own, theirs, o);
    }
    ai_go_fighter(o, carrier, false);
    /* Fight whoever comes at the carrier. */
    o->target = ai_nearest_enemy_to(team, carrier->cur_pos.x, carrier->cur_pos.y, &d2);
    o->fight = o->target != NULL && d2 < CTFAI_FIGHT_RADIUS * CTFAI_FIGHT_RADIUS;
    return true;
}

/* ---- carrier skill (plan §4.3, C4) ---------------------------------------------- */

/* Per base (before a side swap): a stretch of floor on the way home from
 * which a smash throw toward home lands in the zone, and its direction.
 * Hyrule Temple: red's cave floor right of its zone (thrown left), blue's
 * main floor left of its zone (thrown right); ctf_test.py's throwred and
 * throwblue score from 39 and 46 units out with the weakened throw
 * (CTF_FLAG_THROW_SPEED). C4's first runs: blue's 6 throws from x 135-143
 * all scored; red's stretch was too narrow to catch a carrier landing from
 * the tunnel's tip, so it reaches up the slope. */
typedef struct CtfAiThrowSpot {
    float x0, x1, y0, y1;
    s8 dir;
} CtfAiThrowSpot;
static const CtfAiThrowSpot ai_throw_temple[2] = {
    { -62.0f, -25.0f, -100.0f, -84.0f, -1 },
    { 112.0f, 150.0f, -10.0f, 0.0f, 1 },
};

/* A flag carrier's smash throw leaves at about 3 units a frame and lands
 * 45-55 units out on flat ground (C4's runs); a pass aims inside that. */
#define CTFAI_THROW_REACH 50.0f

static void ai_throw(GmCtfAiOrder* o, int dir)
{
    o->throw_now = true;
    o->throw_x = (s8) (dir > 0 ? 127 : -127);
    o->throw_y = 0;
}

/* The way home from a point, in units (the nav graph's path, or a line). */
static float ai_home_cost(Fighter* fp, const CtfAiFlag* own, float x, float y)
{
    int goal, area;
    if (!gmCtfNav_Active()) {
        const float dx = x - own->stand.x, dy = y - own->stand.y;
        return sqrtf(dx * dx + dy * dy);
    }
    area = gmCtfNav_NodeArea(gmCtfNav_NodeIn(-1, x, y));
    goal = gmCtfNav_NodeIn(-1, own->stand.x, own->stand.y);
    return (float) gmCtfNav_Cost(gmCtfNav_Class(fp), area, x, y, goal) / 10.0f;
}

/* The carrier's throws: home from a throw spot (level 5+; under pressure
 * too at 8+), and at 7+ a pass to a teammate nearer home on the same flat
 * floor. The user allowed a cornered throw-away only at high levels and only
 * toward home or a teammate, never blind; measured, every other throw lost
 * the flag (C4's first runs: 8 of 8 cornered or out-of-jumps throws went
 * down the cave's slope into the pit, off the stage, or to an enemy who
 * returned it), so a cornered carrier throws only from a throw spot or as
 * a pass. */
static void ai_carrier_throws(Fighter* fp, int team, const CtfAiFlag* own, GmCtfAiOrder* o)
{
    const int level = fp->cpu.level;
    const float x = fp->cur_pos.x, y = fp->cur_pos.y;
    const bool grounded = fp->ground_or_air == GA_Ground;
    const bool scores = own->home || !gmCtf_HomeRequired();
    float d2;
    Fighter* e = ai_nearest_enemy_to(team, x, y, &d2);
    const bool pressed = e != NULL && d2 < 30.0f * 30.0f;
    int slot;

    if (!grounded) {
        return;
    }
    /* Home from a throw spot: it scores as it lands if ours is home. */
    if (level >= 5 && scores && gmCtf_RealLayout()) {
        const CtfAiThrowSpot* s = &ai_throw_temple[gmCtf_BaseOf(team)];
        if (x >= s->x0 && x <= s->x1 && y >= s->y0 && y <= s->y1 &&
            (level >= 8 || e == NULL || d2 > 40.0f * 40.0f))
        {
            ai_throw(o, s->dir);
            return;
        }
    }
    if (level < 7 || !pressed) {
        return;
    }
    /* A pass: a teammate on this floor level, in reach of a throw, nearer
     * home by 40 units and with no enemy on it. */
    {
        const float mine = ai_home_cost(fp, own, x, y);
        for (slot = 0; slot < 4; slot++) {
            Fighter* t = ai_fighter(slot);
            float td2, adx, dy;
            if (t == NULL || t == fp || gmCtf_TeamOfSlot(slot) != team || !ai_alive(t) ||
                t->ground_or_air != GA_Ground)
            {
                continue;
            }
            adx = t->cur_pos.x > x ? t->cur_pos.x - x : x - t->cur_pos.x;
            dy = t->cur_pos.y - y;
            if (adx < 25.0f || adx > CTFAI_THROW_REACH || dy > 10.0f || dy < -10.0f ||
                ai.area[slot] != ai.area[fp->player_idx])
            {
                continue;
            }
            if (ai_home_cost(fp, own, t->cur_pos.x, t->cur_pos.y) > mine - 40.0f) {
                continue;
            }
            ai_nearest_enemy_to(team, t->cur_pos.x, t->cur_pos.y, &td2);
            if (td2 < 35.0f * 35.0f) {
                continue;
            }
            ai_throw(o, t->cur_pos.x > x ? 1 : -1);
            return;
        }
    }
}

/* At level 7+ a carrier jumps over an enemy standing in its way on the
 * floor (plan §4.3), after the nav layer picked the way point. */
static void ai_carrier_evade(Fighter* fp, int slot, GmCtfAiOrder* o)
{
    int team, dir, s;
    CtfAiFlag theirs;
    if (fp->cpu.level < 7 || o->throw_now || o->hop != 0 || o->go != CTFAI_GO_POINT ||
        fp->ground_or_air != GA_Ground)
    {
        return;
    }
    team = gmCtf_TeamOfSlot(slot);
    ai_flag(team ^ 1, &theirs);
    if (theirs.holder != slot) {
        return;
    }
    dir = o->point.x > fp->cur_pos.x ? 1 : -1;
    for (s = 0; s < 4; s++) {
        Fighter* e = ai_fighter(s);
        float ahead, dy;
        if (e == NULL || gmCtf_TeamOfSlot(s) == team || !ai_alive(e) || e->ground_or_air != GA_Ground) {
            continue;
        }
        ahead = (e->cur_pos.x - fp->cur_pos.x) * (float) dir;
        dy = e->cur_pos.y - fp->cur_pos.y;
        if (ahead > 4.0f && ahead < 22.0f && dy < 12.0f && dy > -12.0f) {
            o->hop = (s8) dir;
            return;
        }
    }
}

/* ---- navigation (plan §3c, C3) ------------------------------------------------- */

/* No progress along a path in this long is a stuck bot: it routes around the
 * node it was making for, for CTFAI_AVOID_FRAMES. */
#define CTFAI_NAV_STUCK_FRAMES 300
#define CTFAI_AVOID_FRAMES 600
/* Share of trips (per 100) on the alternate route, from level 5 up (plan
 * §4.2: UT's alternate paths, a seeded pick per trip). Off on Temple: its
 * alternate, over the top-left mass and down the shaft, cost more stuck
 * bots than it was worth (C3's 2v2 runs: 31 of the stuck events were on
 * the top-left mass's steps); the map data keeps it for a better one. */
#define CTFAI_ALT_ROUTE_PCT 0

static int ai_area_of_point(float x, float y)
{
    return gmCtfNav_NodeArea(gmCtfNav_NodeIn(-1, x, y));
}

/* The goal of an order as a position and the area it is in. */
static int ai_order_goal(const GmCtfAiOrder* o, Vec2* at)
{
    int line;
    switch (o->go) {
    case CTFAI_GO_ITEM:
        at->x = o->item->pos.x;
        at->y = o->item->pos.y;
        line = o->item->ground_or_air == GA_Ground ? o->item->x378_itemColl.floor.index : -1;
        if (line >= 0 && gmCtfNav_AreaOfLine(line) >= 0) {
            return gmCtfNav_AreaOfLine(line);
        }
        return ai_area_of_point(at->x, at->y);
    case CTFAI_GO_FIGHTER:
        at->x = o->follow->cur_pos.x;
        at->y = o->follow->cur_pos.y;
        if (ai.area[o->follow->player_idx] >= 0) {
            return ai.area[o->follow->player_idx];
        }
        return ai_area_of_point(at->x, at->y);
    case CTFAI_GO_POINT:
        *at = o->point;
        return ai_area_of_point(at->x, at->y);
    default:
        return -1;
    }
}

/* Turn an order whose goal is in another area into the next way point of
 * the path there (gmCtfNav_Step), fed to Melee's mover as a raw destination.
 * Fighting, shielding and the pickup stay as the order says. */
static void ai_nav(Fighter* fp, int slot, GmCtfAiOrder* o)
{
    const int frame = gmCtf_Frame();
    const int team = gmCtf_TeamOfSlot(slot);
    Vec2 goal_at, pt;
    CtfNavStep step;
    int goal_area, goal, node, cost;
    int cls;

    if (!gmCtfNav_Active()) {
        return;
    }
    /* Mid-air (a jump or a fall the path asked for): keep aiming where it
     * was going. */
    if (fp->ground_or_air != GA_Ground) {
        if (ai.nav_on[slot] && ai.nav_node[slot] >= 0) {
            /* Over the way point (not under it, still rising to it): aim on
             * from there. A blue Mario that jumped the main floor's step
             * floated 70 frames over the floor's left node before jumping
             * for the pillar (C3's trips). */
            const Vec2 np = gmCtfNav_NodePos(ai.nav_node[slot]);
            const float dx = fp->cur_pos.x - np.x;
            if (dx < 8.0f && dx > -8.0f && fp->cur_pos.y >= np.y - 2.0f &&
                gmCtfNav_Step(gmCtfNav_Class(fp), gmCtfNav_NodeArea(ai.nav_node[slot]), fp->cur_pos.x,
                              fp->cur_pos.y, ai.nav_goal[slot], ai.nav_via[slot], ai.nav_avoid[slot], &step) &&
                step.hop == 0)
            {
                ai.nav_pt[slot] = step.point;
                ai.nav_node[slot] = (s8) step.node;
            }
        }
        if (ai.nav_on[slot]) {
            o->go = CTFAI_GO_POINT;
            o->point = ai.nav_pt[slot];
            o->tol = 6.0f;
            o->nav = true;
        }
        return;
    }
    ai.area[slot] = (s8) gmCtfNav_AreaOfLine(fp->coll_data.floor.index);
    if (ai.area[slot] < 0) {
        ai.area[slot] = (s8) ai_area_of_point(fp->cur_pos.x, fp->cur_pos.y);
    }
    goal_area = ai_order_goal(o, &goal_at);
    goal = gmCtfNav_NodeIn(goal_area, goal_at.x, goal_at.y);
    if (goal != ai.nav_goal[slot]) {
        /* A new trip: maybe the long way round, and a fresh progress mark. */
        const int other = gmCtf_BaseOf(team ^ 1), mine = gmCtf_BaseOf(team);
        int via = -1;
        if (goal == gmCtfNav_StandNode(other)) {
            via = gmCtfNav_AltVia(mine, other);
        } else if (goal == gmCtfNav_StandNode(mine)) {
            via = gmCtfNav_AltVia(other, mine);
        }
        if (via >= 0 && (fp->cpu.level < 5 || ai_rand(team, 100) >= CTFAI_ALT_ROUTE_PCT)) {
            via = -1;
        }
        ai.nav_goal[slot] = (s8) goal;
        ai.nav_via[slot] = (s8) via;
        ai.best_cost[slot] = 0x7FFFFFFF;
        ai.best_at[slot] = frame;
    }
    if (ai.nav_avoid[slot] >= 0 && frame >= ai.avoid_until[slot]) {
        ai.nav_avoid[slot] = -1;
    }
    cls = gmCtfNav_Class(fp);
    if (ai.nav_via[slot] >= 0 && gmCtfNav_Cost(cls, ai.area[slot], fp->cur_pos.x, fp->cur_pos.y,
                                               ai.nav_via[slot]) >= 30000)
    {
        ai.nav_via[slot] = -1; /* the long way is beyond this fighter */
    }
    if (ai.nav_via[slot] >= 0 && ai.area[slot] == gmCtfNav_NodeArea(ai.nav_via[slot])) {
        ai.nav_via[slot] = -1; /* through it */
    }
    if (!gmCtfNav_Step(cls, ai.area[slot], fp->cur_pos.x, fp->cur_pos.y, goal, ai.nav_via[slot],
                       ai.nav_avoid[slot], &step))
    {
        ai.nav_on[slot] = false;
        return;
    }
    pt = step.point;
    node = step.node;
    cost = step.cost;
    o->hop = step.hop;
    /* Progress: the path's cost must keep falling. A bot that can't make a
     * link (a jump it keeps missing) routes around that node for a while. */
    if (cost < ai.best_cost[slot] - 50 || ai_fighting(fp, team)) {
        /* A fight holds a bot up; it isn't lost. */
        ai.best_cost[slot] = cost;
        ai.best_at[slot] = frame;
    } else if (frame - ai.best_at[slot] > CTFAI_NAV_STUCK_FRAMES) {
        if (gmCtf_EventLog()) {
            OSReport("ctfev: f%d stuck p%d at %.0f %.0f dest %.0f %.0f role %s nav %d\n", frame, slot,
                     fp->cur_pos.x, fp->cur_pos.y, pt.x, pt.y, ai_role_name[ai.role[slot]], node);
        }
        ai.nav_avoid[slot] = (s8) node;
        ai.avoid_until[slot] = frame + CTFAI_AVOID_FRAMES;
        ai.best_cost[slot] = 0x7FFFFFFF;
        ai.best_at[slot] = frame;
    }
    o->go = CTFAI_GO_POINT;
    o->point = pt;
    o->tol = 6.0f;
    o->nav = true;
    ai.nav_on[slot] = true;
    ai.nav_pt[slot] = pt;
    ai.nav_node[slot] = (s8) node;
}

#endif /* TARGET_PC */

/* ---- the interface ------------------------------------------------------------ */

#ifdef TARGET_PC
static bool ai_think(Fighter* fp, GmCtfAiOrder* o);
#endif

bool gmCtfAi_Think(Fighter* fp, GmCtfAiOrder* o)
{
#ifdef TARGET_PC
    const int slot = fp->player_idx;
    bool ok;
    /* Nana follows Popo, as in VS. */
    if (!gmCtf_Active() || fp->is_sub_fighter) {
        return false;
    }
    ok = ai_think(fp, o);
    ai.has_goal[slot] = ok && o->go != CTFAI_GO_STOCK;
    if (ai.has_goal[slot]) {
        const Vec2 before = ai.goal[slot];
        ai.goal_tol[slot] = 15.0f;
        switch (o->go) {
        case CTFAI_GO_POINT:
            ai.goal[slot] = o->point;
            ai.goal_tol[slot] = o->tol + 10.0f;
            break;
        case CTFAI_GO_ITEM:
            ai.goal[slot].x = o->item->pos.x;
            ai.goal[slot].y = o->item->pos.y;
            ai.goal_tol[slot] = 20.0f; /* the pickup reach is about 18 */
            break;
        case CTFAI_GO_FIGHTER:
            ai.goal[slot].x = o->follow->cur_pos.x;
            ai.goal[slot].y = o->follow->cur_pos.y;
            /* Melee's approach stops at its attack range and waits there
             * (x38 = x56C + the target's x564); a defender under an enemy on
             * a platform holds its ground (C2's runs: 35-45 units). */
            ai.goal_tol[slot] = 45.0f;
            break;
        }
        if ((ai.goal[slot].x - before.x) * (ai.goal[slot].x - before.x) +
                (ai.goal[slot].y - before.y) * (ai.goal[slot].y - before.y) >
            30.0f * 30.0f)
        {
            ai.goal_since[slot] = gmCtf_Frame();
        }
        ai_nav(fp, slot, o);
        ai_carrier_evade(fp, slot, o);
    } else {
        ai.nav_on[slot] = false;
    }
    return ok;
#else
    return false;
#endif
}

#ifdef TARGET_PC
static bool ai_think(Fighter* fp, GmCtfAiOrder* o)
{
    CtfAiFlag own, theirs;
    int team, enemy, slot;

    slot = fp->player_idx;
    team = gmCtf_TeamOfSlot(slot);
    enemy = team ^ 1;
    ai_flag(team, &own);
    ai_flag(enemy, &theirs);
    ai_order_base(fp, team, o);

    /* The carrier, whatever the plan says: home, attacks off (any attack
     * while holding is a throw), shield and jumps on. In the zone with our
     * flag away (flag must be home) it holds there; the capture fires on
     * the tick our flag comes back (plan §4.4). */
    if (theirs.holder == slot) {
        ai_go_point(o, own.stand, 5.0f);
        o->fight = false;
        if (ai_in_zone(&own, fp->cur_pos.x, fp->cur_pos.y) && !own.home && gmCtf_HomeRequired()) {
            if (ai.hold_since[slot] == 0) {
                ai.hold_since[slot] = gmCtf_Frame();
            }
            /* Holding with nobody of ours left to fetch our flag is a
             * deadlock (1v1: both carriers held at home 87% of the time,
             * C1). Park the flag in the zone instead, where it scores the
             * moment ours is home, and go get ours. */
            if (fp->cpu.level >= 3 && !ai_team_has_other(team, slot) &&
                gmCtf_Frame() - ai.hold_since[slot] >= ai_park_delay(fp->cpu.level))
            {
                o->throw_now = true;
                o->throw_x = 0;
                o->throw_y = -127;
            }
        } else {
            ai.hold_since[slot] = 0;
        }
        if (!o->throw_now) {
            ai_carrier_throws(fp, team, &own, o);
        }
        return true;
    }
    ai.hold_since[slot] = 0;
    switch (ai.role[slot]) {
    case ROLE_RETURN:
        if (ai_free(&own)) {
            return ai_go_flag(o, team);
        }
        return ai_defend(fp, team, &own, &theirs, o);
    case ROLE_DEFEND:
        return ai_defend(fp, team, &own, &theirs, o);
    case ROLE_HUNT:
        return ai_hunt(fp, team, &own, &theirs, o);
    case ROLE_ESCORT:
        return ai_escort(fp, team, &own, &theirs, o);
    case ROLE_ATTACK:
    case ROLE_CARRY: /* stale: the plan hasn't seen the flag go yet */
    default:
        return ai_attack(fp, team, &own, &theirs, o);
    }
}
#endif

/* ---- the event lines ---------------------------------------------------------- */

#ifdef TARGET_PC
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
            /* Short of the AI's goal; stock CPUs (no goal) of their own. */
            const float gx = ai.has_goal[slot] ? ai.goal[slot].x : fp->cpu.x54.x;
            const float gy = ai.has_goal[slot] ? ai.goal[slot].y : fp->cpu.x54.y;
            const float tol = ai.has_goal[slot] ? ai.goal_tol[slot] : fp->cpu.x38 + 10.0f;
            const float dx = gx - fp->cur_pos.x, dy = gy - fp->cur_pos.y;
            const bool short_of = dx * dx + dy * dy > tol * tol;
            const bool stuck = !dead && !ftLib_IsRebirth(fp->gobj) && short_of &&
                               fp->cpu.x84 >= CTFAI_STUCK_FRAMES && !ai_fighting(fp, gmCtf_TeamOfSlot(slot)) &&
                               (!ai.has_goal[slot] || frame - ai.goal_since[slot] >= CTFAI_STUCK_FRAMES);
            if (stuck && !ai.stuck[slot]) {
                OSReport("ctfev: f%d stuck p%d at %.0f %.0f dest %.0f %.0f role %s%s\n", frame, slot,
                         fp->cur_pos.x, fp->cur_pos.y, gx, gy, ai_role_name[ai.role[slot]],
                         ai.has_goal[slot] ? "" : " (stock)");
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
#endif

void gmCtfAi_MatchInit(void)
{
#ifdef TARGET_PC
    int slot;
    memset(&ai, 0, sizeof ai);
    for (slot = 0; slot < 4; slot++) {
        ai.hurt_frame[slot] = -CTFAI_SD_FRAMES;
        ai.area[slot] = ai.nav_goal[slot] = ai.nav_via[slot] = ai.nav_avoid[slot] = ai.nav_node[slot] = -1;
    }
    gmCtfNav_Init();
    if (gmCtf_Active()) {
        /* Each team's stream starts from the match's seeded RNG, once. */
        ai.rng[0] = (u32) HSD_Randi(0x7FFFFFFF);
        ai.rng[1] = (u32) HSD_Randi(0x7FFFFFFF);
    }
#endif
}

void gmCtfAi_Tick(void)
{
#ifdef TARGET_PC
    const int frame = gmCtf_Frame();
    int slot;
    if (gmCtf_EventLog()) {
        ai_log_events(frame);
        if (frame == 2) {
            for (slot = 0; slot < 4; slot++) {
                Fighter* fp = ai_fighter(slot);
                const float g = fp != NULL ? fp->co_attrs.gravity : 0.0f;
                if (fp == NULL || g <= 0.0f) {
                    continue;
                }
                /* A jump's height is v^2 / 2g (cpu.x558 for the air jump). */
                OSReport("ctfev: f2 mover p%d kind %d h1 %.1f h2 %.1f jumps %d walk %.2f dash %.2f drift %.2f\n",
                         slot, fp->kind, fp->co_attrs.jump_v_initial_velocity * fp->co_attrs.jump_v_initial_velocity / (2.0f * g),
                         fp->cpu.x558, fp->co_attrs.max_jumps, fp->co_attrs.walk_max_vel,
                         fp->co_attrs.dash_max_velocity, fp->co_attrs.air_drift_max);
            }
        }
    }
    /* Where everyone stands, for the paths of those who follow them. */
    for (slot = 0; slot < 4; slot++) {
        Fighter* fp = ai_fighter(slot);
        if (fp != NULL && fp->ground_or_air == GA_Ground && gmCtfNav_AreaOfLine(fp->coll_data.floor.index) >= 0) {
            ai.area[slot] = (s8) gmCtfNav_AreaOfLine(fp->coll_data.floor.index);
        }
    }
    ai_planner(frame);
    /* A goal counts for the tick whose think set it: on a tick when stock
     * think takes over (a hazard, a Starman), the stuck check reads x54. */
    for (slot = 0; slot < 4; slot++) {
        ai.has_goal[slot] = false;
    }
#endif
}

u32 gmCtfAi_Checksum(void)
{
#ifdef TARGET_PC
    u32 h = 0;
    int i;
    for (i = 0; i < 4; i++) {
        h = h * 31u + ai.role[i];
        h = h * 31u + (u8) ai.nav_goal[i] + ((u32) (u8) ai.nav_via[i] << 8);
    }
    return h * 31u + ai.sit[0] * 7u + ai.sit[1];
#else
    return 0;
#endif
}
