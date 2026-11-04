#ifndef MELEE_GM_GMCTFAI_H
#define MELEE_GM_GMCTFAI_H

/* CPU players for Capture the Flag (docs/ctf-ai-plan.md). Melee's own CPU AI
 * fights, recovers and moves; this layer only tells it where to go and what to
 * fetch. Everything it keeps is statics of the game library, so the rollback
 * snapshot carries it, and it reads nothing but the sim's state. */

#include <Runtime/platform.h>

#include <dolphin/mtx.h>
#include <melee/ft/forward.h>
#include <melee/it/forward.h>

/* What a CPU player should do this tick: the inputs of Melee's behaviour
 * selector (ftCo_800ADE48), set by ftCo_CtfThink in ftCo_0A01.c. */
typedef enum GmCtfAiGo {
    CTFAI_GO_STOCK,   /* stock VS think: fight the nearest enemy */
    CTFAI_GO_ITEM,    /* fetch `item` (ftCo_800A866C) */
    CTFAI_GO_FIGHTER, /* go where `follow` stands (ftCo_800A75DC) */
    CTFAI_GO_POINT,   /* go to `point`, a floor point */
} GmCtfAiGo;

typedef struct GmCtfAiOrder {
    u8 go;           /* GmCtfAiGo */
    bool fight;      /* the attack masks (cpu.xF9_b2, b3, b5) */
    bool defend;     /* shield, dodge, roll (cpu.xF9_b6) */
    bool pickup;     /* pick up `item` in reach (cpu.xF9_b7) */
    bool nav;        /* `point` is the nav layer's: written as is, no stage hints */
    bool run;        /* dash when far from the destination (cpu.xF8_b0, which
                        the no-target init clears: VS CPUs walk to a far goal) */
    Fighter* target; /* cpu.x44, whom to fight, or NULL */
    Fighter* follow; /* CTFAI_GO_FIGHTER */
    Item* item;      /* cpu.x4C: this tick's flag, never kept */
    Vec2 point;      /* CTFAI_GO_POINT */
    float tol;       /* arrival distance */
    /* Throw the held flag now, the left stick at (throw_x, throw_y) with A
     * on the same frame (a smash throw; stick down: dropped at the feet). */
    bool throw_now;
    s8 throw_x, throw_y;
    /* Hop off a ledge now, -1 left or 1 right (the nav layer's; Melee's mover
     * won't walk off a solid ledge). */
    s8 hop;
} GmCtfAiOrder;

/* A CPU's order for this tick; false outside CTF, or to leave it to stock
 * AI. Called from the think step of Melee's CPU AI (ftCo_800B24B8). */
bool gmCtfAi_Think(Fighter* fp, GmCtfAiOrder* out);

/* At a match's start (gmCtf_MatchInit). */
void gmCtfAi_MatchInit(void);
/* Once per frame, after the rules (gmctf.c's tick): plans and event lines. */
void gmCtfAi_Tick(void);
/* The plans, folded into the CTF checksum. */
u32 gmCtfAi_Checksum(void);

#endif
