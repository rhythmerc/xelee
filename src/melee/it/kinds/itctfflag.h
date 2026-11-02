#ifndef MELEE_IT_KINDS_ITCTFFLAG_H
#define MELEE_IT_KINDS_ITCTFFLAG_H

/* Capture the Flag's flag (It_Kind_Ctf_Flag): a light throwable that never
 * opens, never despawns and lands back to idle. It borrows the Poke Ball's
 * article (model, attributes, hurtbox) until it has its own look.
 * docs/xeleenet-modes-plan.md §3.1. */

#include <Runtime/platform.h>

#include <dolphin/mtx.h>
#include <melee/it/forward.h>

#include <melee/it/kinds/types.h>

extern struct ItemLogicTable itCtfFlag_LogicTable;

/* Is this item a CTF flag? (NULL is not.) */
bool itCtfFlag_Is(Item_GObj* gobj);

/* Melee drops a held light item when HSD_Randi(x418) < the hit's damage
 * (Fighter_8006CDA4); a flag carrier rolls with the damage times this. */
#define CTF_FLAG_DROP_BIAS 3

/* A flag carrier's walk and dash speeds, as a fraction (ftCo_800D105C). */
#define CTF_FLAG_CARRY_SPEED 0.8f

/* The flag's look (its GXLink render function): a pole and a cloth in its
 * team's colour, blinking near the end of an auto-return. */
void itCtfFlag_Draw(HSD_GObj* gobj, intptr_t pass);

/* A team's stand at `at` (the floor point): a plate, and while the flag is
 * away (`empty`) a translucent ghost of it. Drawn by gmctf.c. */
void itCtfFlag_DrawStand(const Vec3* at, u8 team, bool empty);

/* The article the flag spawns with (the Poke Ball's, from ItCo.dat). */
Article* itCtfFlag_Article(void);

#endif
