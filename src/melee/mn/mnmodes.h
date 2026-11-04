#ifndef MELEE_MN_MNMODES_H
#define MELEE_MN_MNMODES_H

#include "forward.h"

#include <sysdolphin/baselib/forward.h>

/* PC only: VS Mode > Xelee Melee, offline (docs/xeleenet-modes-plan.md
 * §2.3). VS Mode's row under Special Melee opens a menu of the game modes (Capture the
 * Flag so far), Special Melee's panel with its sub-title swapped; each row
 * opens the mode's rules page (mnpage.c, MENU_KIND_MODE_PAGE), whose START
 * goes on to the mode's CSS (gmctfmode.c). Labels and descriptions are lang
 * keys, drawn as the PC rows are (mnmain.c's mn_UpdatePcLabels). */

/* VS Mode's Xelee Melee row and the Xelee Melee menu's rows: the label and
 * the bottom box's text, or NULL for a stock row. */
const char* mnModes_Label(MenuKind kind, int selection);
const char* mnModes_Description(MenuKind kind, int selection);

/* The Xelee Melee menu's think (mn_803EB6B0[MENU_KIND_XELEE_MELEE]). */
void mnModes_Think(HSD_GObj* gp);

/* Capture the Flag's rules page's START: on to GM_CTF. */
void mnModes_StartCtf(void);

#endif
