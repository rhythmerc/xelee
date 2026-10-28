#ifndef MELEE_MN_MNXELEENET_H
#define MELEE_MN_MNXELEENET_H

#include <stdbool.h>

/* PC only: Online Mode > XeleeNet > Host, Join and Name (src/pc/xeleenet_menu.h),
 * in the online lobby scene. A page in VS. Mode's red frame, as the Slippi
 * pages are (MENU_KIND_SLIPPI_PAGE: Name Entry's panel animation, its
 * sub-title swapped for "XeleeNet Host" or "XeleeNet Join"), with Melee's
 * keyboard opened in its place for the name. */

/* From the scene's enter: PC_XN_MENU_HOST, _JOIN or _NAME. */
void mnXeleeNet_Begin(int what);
/* Each scene frame while the keyboard isn't up. */
void mnXeleeNet_Frame(void);
/* From the scene's exit. */
void mnXeleeNet_End(void);
bool mnXeleeNet_Active(void);

/* The tab's sub-title while the page is up (mnpage.c draws it). */
const char* mnXeleeNet_Title(void);

#endif
