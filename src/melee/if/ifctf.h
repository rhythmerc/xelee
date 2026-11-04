#ifndef MELEE_IF_IFCTF_H
#define MELEE_IF_IFCTF_H

/* Capture the Flag's HUD (docs/xeleenet-modes-plan.md §3.5): the score strip
 * under the timer and the sides card at READY, GO. Drawn by the HUD camera,
 * so it shows on the headset's HUD plane too. */

void ifCtf_Create(void);
void ifCtf_Free(void);

#endif
