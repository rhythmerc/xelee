#ifndef MELEE_MN_MNONLINE_H
#define MELEE_MN_MNONLINE_H

#include "forward.h"

#include <sysdolphin/baselib/forward.h>

/* PC only: the VS Mode > Online submenu (MENU_KIND_ONLINE). SdMenu has no
 * label textures or SIS strings for it, so mnmain.c hides the matanim label
 * of every slot that has an mnOnline_Label and draws that text instead. */

/* 22D594-style think proc, see mn_803EB6B0[MENU_KIND_ONLINE]. */
void mnOnline_Think(HSD_GObj*);
/* The Slippi submenu (MENU_KIND_ONLINE_SLIPPI). */
void mnOnline_SlippiThink(HSD_GObj*);
/* The XeleeNet submenu (MENU_KIND_ONLINE_XN). */
void mnOnline_XnThink(HSD_GObj*);

/* Literal label / bottom-bar text for PC-only entries; NULL = stock. */
const char* mnOnline_Label(MenuKind, int selection);
/* Rows on the Slippi menu: 1 (Login) signed out, 3 signed in. */
int mnOnline_SlippiCount(void);
/* Called as the menu is built, so the think can tell when the rows went stale. */
void mnOnline_SlippiBuilt(void);
const char* mnOnline_Description(MenuKind, int selection);

/* One-shot description override set by the think proc (A on a stub). */
const char* mnOnline_TakeNotice(void);
/* Options > Place Stage (XR): the placement scene, then back to Options. */
void mnOnline_PlaceFromOptions(void);
/* Options > Graphics > Calibrate room light (XR): the placement scene's
 * room light calibration alone, then back to Options. */
void mnOnline_CalibrateFromOptions(void);

#endif
