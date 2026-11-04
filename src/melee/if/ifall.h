#ifndef MELEE_IF_IFALL_H
#define MELEE_IF_IFALL_H

#include <Runtime/platform.h>

#include <sysdolphin/baselib/forward.h>

#include <dolphin/mtx.h>

/* 2F3394 */ void ifAll_802F3394(void);
/* 2F33CC */ void ifAll_802F33CC(void);
/* 2F3404 */ HSD_GObj* ifAll_GetHUDGObj(void);
/* 2F3414 */ Vec3* ifAll_GetTimerPosition(void);
/* 2F3424 */ Vec3* ifAll_GetPlayerHUDPosition(int idx);
/* 2F343C */ void ifAll_802F343C(int);
/* 2F3690 */ HSD_Archive** ifAll_GetArchive(void);
/* 2F3698 */ void ifAll_HideHUD(void);
/* 2F36A4 */ void ifAll_ShowHUD(void);
/* 2F36B0 */ bool ifAll_IsHUDHidden(void);
/* 2F390C */ void ifAll_802F390C(void);
/* 2F3A64 */ void ifAll_802F3A64(void);

#ifdef TARGET_PC
/* The HUD's top row (the timer, CTF's score, Home-Run's record): in XR's 3D
 * view it is drawn just above the damage row instead of at the screen's top
 * edge. Its draws go between ifAll_TopBegin and ifAll_TopEnd, or through
 * ifAll_TopJObjCallback for a joint tree's GObj. Nothing changes elsewhere. */
bool ifAll_TopBegin(void);
void ifAll_TopEnd(bool began);
void ifAll_TopJObjCallback(HSD_GObj* gobj, intptr_t pass);
#endif

#endif
