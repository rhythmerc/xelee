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

/* At a match's start (gmCtf_MatchInit). */
void gmCtfAi_MatchInit(void);
/* Once per frame, after the rules (gmctf.c's tick): plans and event lines. */
void gmCtfAi_Tick(void);
/* The plans, folded into the CTF checksum. */
u32 gmCtfAi_Checksum(void);

#endif
