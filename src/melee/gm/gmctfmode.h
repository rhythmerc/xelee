#ifndef MELEE_GM_GMCTFMODE_H
#define MELEE_GM_GMCTFMODE_H

#include <Runtime/platform.h>

#include <melee/gm/types.h>

#include <stddef.h>

/* PC only: GM_CTF, offline Capture the Flag (docs/xeleenet-modes-plan.md
 * §2.3). VS Mode > Game Modes > Capture the Flag's rules page (mnmodes.c,
 * ctf_rules_menu.cpp) starts it: the CSS with teams forced on, red and blue
 * only, then the stage select with only the CTF maps lit, then the match
 * with its CTF rules (gmctf.c), results, and back to the CSS, as VS Mode's
 * Melee does. It is gmgiant.c's shape on VS Melee's states (gmvsmelee.c),
 * with a VsModeData of its own and a start_cb that sets the CTF rules. */

extern GameModeState gm_Mode_Ctf_States[];
void gmCtfMode_OnInit(void);
void gmCtfMode_OnLoad(void);
void gmCtfMode_OnUnload(void);

/* An offline CTF is in force (the CSS, SSS and match of GM_CTF). */
bool gmCtfMode_InForce(void);
/* The CSS's first visit of this entry to the mode: deal the doors red and
 * blue (true once). */
bool gmCtfMode_TakeDeal(void);
/* The CSS's rules banner ("Capture the Flag: first to 5!"). */
void gmCtfMode_Banner(char* out, size_t size);

#endif
