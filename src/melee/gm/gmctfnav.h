#ifndef MELEE_GM_GMCTFNAV_H
#define MELEE_GM_GMCTFNAV_H

/* Waypoint navigation for the CTF CPU players (docs/ctf-ai-plan.md §3c,
 * C3): a hand-made graph per CTF map, whose next hop is fed to Melee's own
 * mover as the CPU's destination. The graph is const data; the next-hop
 * tables are built at match start from it alone, so they are the same on
 * every peer. */

#include <Runtime/platform.h>

#include <dolphin/mtx.h>
#include <melee/ft/forward.h>

/* Movement classes: what a fighter's jumps can reach (gmCtfNav_Class). */
#define CTFNAV_CLASSES 3

/* At a match's start: the stage's graph and its next-hop tables. False if
 * the stage has none (the AI then leaves long moves to Melee's mover). */
bool gmCtfNav_Init(void);
bool gmCtfNav_Active(void);

/* A fighter's movement class, from its jump heights. */
int gmCtfNav_Class(Fighter* fp);

/* The nav area a collision line belongs to, or -1 (a wall, or no graph). */
int gmCtfNav_AreaOfLine(int line);
/* The area of a node, and its position. */
int gmCtfNav_NodeArea(int node);
Vec2 gmCtfNav_NodePos(int node);
/* The node nearest (x, y) within an area (any area if area < 0), or -1. */
int gmCtfNav_NodeIn(int area, float x, float y);
/* Named nodes of the layout: a base's stand and guard spot (base 0 or 1,
 * before any side swap). */
int gmCtfNav_StandNode(int base);
int gmCtfNav_GuardNode(int base);

/* The next move of a path (gmCtfNav_Step). */
typedef struct CtfNavStep {
    Vec2 point; /* the way point: the destination for Melee's mover */
    int node;   /* the node it is */
    int cost;   /* the path's cost from here, in tenths of a unit */
    s8 hop;     /* hop off a ledge now, -1 left or 1 right (Melee's mover
                   teeters at a ledge and turns back: ftCo_800AACD0); 0 no */
} CtfNavStep;

/* Where a fighter of class `cls` standing at (x, y) in area `area` should
 * head for node `goal`. With `via` >= 0 the path goes through that node
 * first (an alternate route); `avoid` (-1 for none) is a node to route
 * around (a link that failed). False when (x, y) is already in the goal's
 * area: walk straight there. */
bool gmCtfNav_Step(int cls, int area, float x, float y, int goal, int via, int avoid, CtfNavStep* out);

/* A path's cost (tenths of a unit), for role assignment; a large number if
 * there is none. */
int gmCtfNav_Cost(int cls, int area, float x, float y, int goal);

/* A base's alternate route for attackers and carriers: a node to go through
 * (or -1), from base `from` to base `to`. */
int gmCtfNav_AltVia(int from, int to);

#endif
