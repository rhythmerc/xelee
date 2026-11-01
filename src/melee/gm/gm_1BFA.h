#ifndef MELEE_GM_1BFA_H
#define MELEE_GM_1BFA_H

#include <melee/gm/forward.h>

/* 1BFA6C */ void gm_ModeState_Approach_OnEnter(GameModeState*);
/* 1BFABC */ void gm_ModeState_ApproachVs_OnEnter(GameModeState*);
/* 1BFCFC */ void gm_ModeState_Prize_OnEnter(GameModeState*);
#ifdef TARGET_PC
/* If the "XeleeNet unlocked" message has never shown, queues it for the next
 * special-message scene and returns true. */
bool gm_PrizeQueueXnNotice(void);
#endif

#endif
