#ifndef MELEE_GM_GMCTF_H
#define MELEE_GM_GMCTF_H

/* Capture the Flag in the sim (docs/xeleenet-modes-plan.md §3). All of its
 * state is statics of the game library, so the rollback snapshot carries it,
 * and every function here is a pure function of that state and the match's. */

#include <Runtime/platform.h>

#include <dolphin/mtx.h>
#include <sysdolphin/baselib/forward.h>

/* A CTF match's own rules (plan §3.6); the time limit is Melee's own. */
typedef struct GmCtfRules {
    u8 captures_to_win; /* 1-9 */
    u8 home_required;   /* your own flag must be on its stand to score */
    u8 carry_slow;      /* carriers walk and dash slower (CTF_FLAG_CARRY_SPEED) */
    u8 swap_sides;      /* this game, red takes blue's base and blue red's */
    u16 return_frames;  /* a free flag goes home after this long; 0 = never */
} GmCtfRules;

/* The rules for the next match, or NULL for no CTF. Every peer of a netplay
 * session sets the same rules before the match starts. */
void gmCtf_SetRules(const GmCtfRules* rules);
/* CTF rules are set and the stage has a CTF layout. */
bool gmCtf_Active(void);

/* At a match's start, after the item system is up. */
void gmCtf_MatchInit(void);

/* A player's start position (true: CTF chose it). */
bool gmCtf_SpawnPoint(int slot, Vec3* out);
/* After Melee picks a respawn point for a player: CTF moves it to the base. */
void gmCtf_Respawn(int slot);
/* May this fighter pick up this item? (A team can't lift its flag from its
 * stand.) */
bool gmCtf_PickupBlocked(HSD_GObj* fighter, HSD_GObj* item);
/* The flag item is going away (itctfflag.c). */
void gmCtf_FlagDestroyed(HSD_GObj* item);
/* Carriers are slowed in this match. */
bool gmCtf_CarrySlow(void);

/* What the HUD shows (ifctf.c): read-only, from the sim's state. */
typedef struct GmCtfHud {
    u8 captures[2];
    s8 holder[2];        /* the carrier's port, or -1 */
    bool home[2];
    bool present[2];     /* the flag item exists (false for a frame after a loss) */
    u16 return_left[2];  /* frames, while free and counting */
    bool swap;
    int frame;
    const char* base[2]; /* where red and blue defend this game ("BOTTOM LEFT") */
} GmCtfHud;
bool gmCtf_GetHud(GmCtfHud* out);

/* The capture sound (Melee's own SFX id). */
#define CTF_CAPTURE_SFX 0xAA /* the trophy chime (Classic/Adventure trophy catch, itcoin.c) */

/* The match's end: the winning team (0 red, 1 blue) or -1; a tie. */
int gmCtf_Winner(void);
bool gmCtf_Tied(void);
/* A player's results score: their team's captures (Melee team id). */
int gmCtf_TeamScore(int melee_team);
/* The CTF state folded into the per-frame netplay checksum (0 when off). */
u32 gmCtf_Checksum(void);

#endif
