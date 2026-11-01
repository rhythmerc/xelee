#ifndef _IFPRIZE_H_
#define _IFPRIZE_H_

#include <Runtime/platform.h>

#include <sysdolphin/baselib/forward.h>

#ifdef TARGET_PC
/* A special message of our own, past the game's unlock ids (0..0x41): the
 * one-time "XeleeNet unlocked" notice (gm_PrizeQueueXnNotice). */
#define IF_PRIZE_XELEENET 0x100
#endif

/* 2FE3F8 */ void un_802FE3F8(int a, int b, u16* c, u16* d);
/* 2FE470 */ void fn_802FE470(HSD_GObj* gobj);
/* 2FE6A8 */ void un_802FE6A8(void);
/* 2FE8CC */ void un_802FE8CC(void);
/* 2FE918 */ void un_802FE918(int a, int b, int c);
/* 2FEBE0 */ void ifPrize_Scene_OnEnter(void* arg0);
/* 2FED10 */ void ifPrize_Scene_OnExit(void*);

#endif
