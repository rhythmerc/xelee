#ifndef MELEE_IT_KINDS_ITCTFFLAG_H
#define MELEE_IT_KINDS_ITCTFFLAG_H

/* Capture the Flag's flag (It_Kind_Ctf_Flag): a light throwable that never
 * opens, never despawns and lands back to idle. It borrows the Poke Ball's
 * article (model, attributes, hurtbox) until it has its own look.
 * docs/xeleenet-modes-plan.md §3.1. */

#include <Runtime/platform.h>

#include <melee/it/forward.h>

#include <melee/it/kinds/types.h>

extern struct ItemLogicTable itCtfFlag_LogicTable;

/* The article the flag spawns with (the Poke Ball's, from ItCo.dat). */
Article* itCtfFlag_Article(void);

#endif
