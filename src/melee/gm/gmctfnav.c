/* Waypoint navigation for the CTF CPU players (docs/ctf-ai-plan.md §3c, C3).
 *
 * Melee's CPU mover is greedy (ftCo_800AB224): it walks along an island,
 * jumps at a destination more than 35 degrees up, drops at one more than 45
 * down, and otherwise runs at it. On Hyrule Temple that runs CPUs off the
 * cave floor's right end into the pit, and it can't find the way out of the
 * cave without Melee's own five Temple hints. So the AI feeds it one waypoint
 * at a time from a hand-made graph, placed so that every hop is one of those
 * simple moves.
 *
 * The graph is const data per CTF map. At match start it is turned into an
 * all-pairs next-hop table per movement class (Floyd-Warshall over integer
 * costs, ties broken by node order), a pure function of the data: the same on
 * every peer. */

#include "gmctfnav.h"

#include "gmctf.h"

#include <Runtime/platform.h>

#include <melee/ft/fighter.h>
#include <melee/ft/types.h>
#include <melee/gm/gmvs.h>
#include <melee/gr/forward.h>

#ifdef TARGET_PC

#include <string.h>

#define NAV_MAX_NODES 32
#define NAV_NONE 0xFF
#define NAV_FAR 30000 /* no path (tenths of a unit) */

enum {
    LINK_WALK, /* along one floor */
    LINK_DROP, /* down through a pass-through platform: the mover holds down */
    LINK_JUMP, /* up: the mover jumps at a destination over 35 degrees up */
    LINK_HOP,  /* off a solid ledge: Melee's mover teeters there and turns back
                  (ftCo_800AACD0), so the bot hops off toward the next node */
};

typedef struct NavNode {
    float x, y; /* a point on the floor */
    s8 area;
} NavNode;

typedef struct NavLink {
    u8 from, to;
    u8 kind;
    u8 reach; /* a jump: the height it needs, in units (0: any class) */
} NavLink;

typedef struct NavGraph {
    u16 stkind;
    const NavNode* node;
    u8 nodes;
    const NavLink* link;
    u8 links;
    const s8* line_area; /* by collision line id: a floor line's area, or -1 */
    u8 lines;
    u8 stand[2], guard[2]; /* by base, before a side swap */
    s8 alt_via[2][2];      /* [from base][to base]: an alternate route's node, or -1 */
} NavGraph;

/* ---- Hyrule Temple ------------------------------------------------------------
 * From the M0 survey (MELEE_DEBUG_CTF_SURVEY; temple-ctf-survey.png). Areas
 * group the floor lines a fighter can walk between. */
enum {
    T_TOP,       /* the top-left mass: lines 0-13 and 41, the shaft's pass-through top */
    T_CAVE,      /* red's cave floor, 14-20 */
    T_SHAFTPLAT, /* the ledge in the left shaft, 21 */
    T_HIGHHI,    /* the high platform's top, 22 */
    T_HIGHLO,    /* its lower deck over blue's base, 23 */
    T_SLOPEPLAT, /* the sloped platform over the main floor, 24 */
    T_TOPPLAT,   /* the top middle platform, 25 */
    T_LEFTPLAT,  /* the small platform at the top left, 26-28 */
    T_RAMP,      /* the tunnel slope out of the cave, 29-31 */
    T_PILLAR,    /* the pillar between the slope and the main floor, 32 */
    T_MAIN,      /* the main floor to blue's right edge, 33-37 */
    T_BOTTOM,    /* the island under the cave's mouth, 38-40 */
};

static const s8 temple_line_area[42] = {
    T_TOP,       T_TOP,      T_TOP,     T_TOP,    T_TOP,    T_TOP,    T_TOP,   /* 0-6 */
    T_TOP,       T_TOP,      T_TOP,     T_TOP,    T_TOP,    T_TOP,    T_TOP,   /* 7-13 */
    T_CAVE,      T_CAVE,     T_CAVE,    T_CAVE,   T_CAVE,   T_CAVE,   T_CAVE,  /* 14-20 */
    T_SHAFTPLAT, T_HIGHHI,   T_HIGHLO,  T_SLOPEPLAT,                           /* 21-24 */
    T_TOPPLAT,   T_LEFTPLAT, T_LEFTPLAT, T_LEFTPLAT,                           /* 25-28 */
    T_RAMP,      T_RAMP,     T_RAMP,    T_PILLAR,                              /* 29-32 */
    T_MAIN,      T_MAIN,     T_MAIN,    T_MAIN,   T_MAIN,                      /* 33-37 */
    T_BOTTOM,    T_BOTTOM,   T_BOTTOM,  T_TOP,                                 /* 38-41 */
};

enum {
    N_RED_STAND,
    N_RED_GUARD,
    N_SHAFT_FOOT,
    N_TIP_FOOT,
    N_CAVE_END,
    N_RAMP_TIP,
    N_RAMP_MID,
    N_RAMP_TOP,
    N_PILLAR,
    N_MAIN_LEFT,
    N_MAIN_MID,
    N_BLUE_GUARD,
    N_BLUE_STAND,
    N_TOP_END,
    N_TOP_MID,
    N_SHAFT_TOP,
    N_TOP_LEFT,
    N_SHAFT_PLAT,
    N_TOPPLAT,
    N_SLOPEPLAT,
    N_HIGH_LO,
    N_HIGH_HI,
    N_LEFTPLAT,
    N_BOTTOM,
    N_TEMPLE_NODES,
};

static const NavNode temple_nodes[N_TEMPLE_NODES] = {
    [N_RED_STAND] = { -105.0f, -88.6f, T_CAVE },
    [N_RED_GUARD] = { -85.0f, -88.6f, T_CAVE },
    [N_SHAFT_FOOT] = { -124.0f, -88.6f, T_CAVE },
    /* On the cave's slope under the tunnel's tip: from here the tip is 62
     * degrees up, so the mover jumps (Melee's own hint aims there too). */
    [N_TIP_FOOT] = { -38.0f, -91.7f, T_CAVE },
    [N_CAVE_END] = { 2.0f, -110.2f, T_CAVE },
    [N_RAMP_TIP] = { -14.0f, -45.6f, T_RAMP },
    [N_RAMP_MID] = { 8.0f, -33.9f, T_RAMP },
    [N_RAMP_TOP] = { 38.0f, -11.9f, T_RAMP },
    [N_PILLAR] = { 57.0f, 23.2f, T_PILLAR },
    [N_MAIN_LEFT] = { 74.0f, -6.2f, T_MAIN },
    [N_MAIN_MID] = { 125.0f, -7.7f, T_MAIN },
    [N_BLUE_GUARD] = { 180.0f, -6.1f, T_MAIN },
    [N_BLUE_STAND] = { 200.0f, -5.5f, T_MAIN },
    [N_TOP_END] = { -8.0f, 24.3f, T_TOP },
    [N_TOP_MID] = { -50.0f, 22.5f, T_TOP },
    /* Over the shaft, right of its ledge: dropping through here lands by
     * red's stand. */
    [N_SHAFT_TOP] = { -117.0f, 8.1f, T_TOP },
    [N_TOP_LEFT] = { -190.0f, 10.0f, T_TOP },
    [N_SHAFT_PLAT] = { -131.0f, -26.8f, T_SHAFTPLAT },
    [N_TOPPLAT] = { -42.0f, 68.5f, T_TOPPLAT },
    [N_SLOPEPLAT] = { 128.0f, 31.6f, T_SLOPEPLAT },
    [N_HIGH_LO] = { 183.0f, 47.0f, T_HIGHLO },
    [N_HIGH_HI] = { 183.0f, 92.2f, T_HIGHHI },
    [N_LEFTPLAT] = { -190.0f, 48.9f, T_LEFTPLAT },
    [N_BOTTOM] = { 44.0f, -143.7f, T_BOTTOM },
};

/* The links between areas; walking within an area links each node to its
 * neighbours by x (nav_build). Reaches are the jumps' rise in units. */
static const NavLink temple_links[] = {
    { N_TIP_FOOT, N_RAMP_TIP, LINK_JUMP, 46 },
    { N_RAMP_TIP, N_TIP_FOOT, LINK_HOP, 0 },
    { N_RAMP_TOP, N_PILLAR, LINK_JUMP, 35 },
    { N_PILLAR, N_RAMP_TOP, LINK_HOP, 0 },
    { N_PILLAR, N_MAIN_LEFT, LINK_HOP, 0 },
    { N_MAIN_LEFT, N_PILLAR, LINK_JUMP, 30 },
    { N_RAMP_MID, N_TOP_END, LINK_JUMP, 58 },
    { N_TOP_END, N_RAMP_MID, LINK_HOP, 0 },
    { N_SHAFT_TOP, N_RED_STAND, LINK_DROP, 0 },
    { N_SHAFT_FOOT, N_SHAFT_PLAT, LINK_JUMP, 62 },
    { N_SHAFT_PLAT, N_SHAFT_FOOT, LINK_DROP, 0 },
    { N_SHAFT_PLAT, N_SHAFT_TOP, LINK_JUMP, 35 },
    { N_TOPPLAT, N_TOP_MID, LINK_DROP, 0 },
    { N_SLOPEPLAT, N_MAIN_MID, LINK_DROP, 0 },
    { N_HIGH_HI, N_HIGH_LO, LINK_DROP, 0 },
    { N_HIGH_LO, N_BLUE_GUARD, LINK_DROP, 0 },
    { N_LEFTPLAT, N_TOP_LEFT, LINK_DROP, 0 },
    { N_BOTTOM, N_CAVE_END, LINK_JUMP, 34 },
};

static const NavGraph nav_graphs[] = {
    { St_Kind_Shrine, temple_nodes, N_TEMPLE_NODES, temple_links, sizeof temple_links / sizeof temple_links[0],
      temple_line_area, sizeof temple_line_area, { N_RED_STAND, N_BLUE_STAND }, { N_RED_GUARD, N_BLUE_GUARD },
      /* red to blue: up the shaft and over the top; blue to red: over the
       * top and down the shaft (both only for classes that reach). */
      { { -1, N_TOP_MID }, { N_TOP_MID, -1 } } },
};

/* ---- the class reaches ------------------------------------------------------- */

/* A class's reach, in units: the highest jump link it may take. Everyone
 * gets the tunnel's tip (46), or red couldn't leave its cave at all. */
static const u8 nav_class_reach[CTFNAV_CLASSES] = { 46, 58, 62 };
/* The fighter's ideal reach (first jump plus air jump, v^2 / 2g each) that
 * puts it in a class: a link needs about 8 more than its rise, for the air
 * jump's timing and landing on top. Measured on the whole cast ("mover"
 * lines, MELEE_DEBUG_CTF_AI_LOG): Falco 93, Yoshi 90, Luigi 76, Sheik and
 * Fox 72 (class 2); Ice Climbers 68, Falcon and DK 67 (class 1); Mewtwo 65,
 * Pikachu 61, Samus, Bowser, Marth, Ness 60, Mario 56, Link 50, Ganondorf 50,
 * Peach 45 (class 0). Kirby and Jigglypuff, with five air jumps, are 2. */
static const u8 nav_class_min[CTFNAV_CLASSES] = { 0, 66, 70 };

/* ---- state: the tables, rebuilt at match start ------------------------------- */

static struct {
    const NavGraph* g;
    s16 cost[CTFNAV_CLASSES][NAV_MAX_NODES][NAV_MAX_NODES];
    u8 next[CTFNAV_CLASSES][NAV_MAX_NODES][NAV_MAX_NODES];
} nav;

static int nav_dist10(float ax, float ay, float bx, float by)
{
    const float dx = ax - bx, dy = ay - by;
    return (int) (sqrtf(dx * dx + dy * dy) * 10.0f);
}

static int nav_link_cost(const NavGraph* g, int a, int b, int kind)
{
    /* Tenths of a unit, plus a price for leaving the ground (a jump can be
     * knocked out of; a drop gives up height). */
    static const s16 extra[] = { 0, 100, 250, 100 };
    return nav_dist10(g->node[a].x, g->node[a].y, g->node[b].x, g->node[b].y) + extra[kind];
}

static void nav_edge(int c, int a, int b, int w)
{
    if (w < nav.cost[c][a][b]) {
        nav.cost[c][a][b] = (s16) w;
        nav.next[c][a][b] = (u8) b;
    }
}

static void nav_build(const NavGraph* g)
{
    int c, i, j, k;
    for (c = 0; c < CTFNAV_CLASSES; c++) {
        for (i = 0; i < NAV_MAX_NODES; i++) {
            for (j = 0; j < NAV_MAX_NODES; j++) {
                nav.cost[c][i][j] = i == j ? 0 : NAV_FAR;
                nav.next[c][i][j] = i == j ? (u8) i : NAV_NONE;
            }
        }
        /* Walks: each node to its nearest neighbours left and right in its
         * area. */
        for (i = 0; i < g->nodes; i++) {
            int left = -1, right = -1;
            for (j = 0; j < g->nodes; j++) {
                if (j == i || g->node[j].area != g->node[i].area) {
                    continue;
                }
                if (g->node[j].x < g->node[i].x && (left < 0 || g->node[j].x > g->node[left].x)) {
                    left = j;
                }
                if (g->node[j].x > g->node[i].x && (right < 0 || g->node[j].x < g->node[right].x)) {
                    right = j;
                }
            }
            if (left >= 0) {
                nav_edge(c, i, left, nav_link_cost(g, i, left, LINK_WALK));
            }
            if (right >= 0) {
                nav_edge(c, i, right, nav_link_cost(g, i, right, LINK_WALK));
            }
        }
        for (i = 0; i < g->links; i++) {
            const NavLink* l = &g->link[i];
            if (l->kind == LINK_JUMP && l->reach > nav_class_reach[c]) {
                continue;
            }
            nav_edge(c, l->from, l->to, nav_link_cost(g, l->from, l->to, l->kind));
        }
        /* Floyd-Warshall in node order: ties keep the earlier path. */
        for (k = 0; k < g->nodes; k++) {
            for (i = 0; i < g->nodes; i++) {
                if (nav.cost[c][i][k] >= NAV_FAR) {
                    continue;
                }
                for (j = 0; j < g->nodes; j++) {
                    const int w = nav.cost[c][i][k] + nav.cost[c][k][j];
                    if (w < nav.cost[c][i][j]) {
                        nav.cost[c][i][j] = (s16) (w < NAV_FAR ? w : NAV_FAR);
                        nav.next[c][i][j] = nav.next[c][i][k];
                    }
                }
            }
        }
    }
}

static float nav_d2(const NavNode* n, float x, float y)
{
    const float dx = n->x - x, dy = n->y - y;
    return dx * dx + dy * dy;
}

#endif /* TARGET_PC */

/* ---- the interface ---------------------------------------------------------------- */

bool gmCtfNav_Init(void)
{
#ifdef TARGET_PC
    int i;
    memset(&nav, 0, sizeof nav);
    if (!gmCtf_Active() || !gmCtf_RealLayout()) {
        return false;
    }
    for (i = 0; i < (int) (sizeof nav_graphs / sizeof nav_graphs[0]); i++) {
        if (nav_graphs[i].stkind == gm_GetStKind()) {
            nav.g = &nav_graphs[i];
            nav_build(nav.g);
            return true;
        }
    }
#endif
    return false;
}

bool gmCtfNav_Active(void)
{
#ifdef TARGET_PC
    return nav.g != NULL;
#else
    return false;
#endif
}

int gmCtfNav_Class(Fighter* fp)
{
#ifdef TARGET_PC
    /* The first jump's rise plus each air jump's (cpu.x558, v^2 / 2g). */
    const float g = fp->co_attrs.gravity;
    float reach;
    int c;
    if (g <= 0.0f || fp->co_attrs.max_jumps > 2) {
        return CTFNAV_CLASSES - 1;
    }
    reach = fp->co_attrs.jump_v_initial_velocity * fp->co_attrs.jump_v_initial_velocity / (2.0f * g) +
            fp->cpu.x558;
    for (c = CTFNAV_CLASSES - 1; c > 0; c--) {
        if (reach >= nav_class_min[c]) {
            return c;
        }
    }
    return 0;
#else
    return 0;
#endif
}

int gmCtfNav_AreaOfLine(int line)
{
#ifdef TARGET_PC
    if (nav.g == NULL || line < 0 || line >= nav.g->lines) {
        return -1;
    }
    return nav.g->line_area[line];
#else
    return -1;
#endif
}

int gmCtfNav_NodeArea(int node)
{
#ifdef TARGET_PC
    return nav.g != NULL && node >= 0 && node < nav.g->nodes ? nav.g->node[node].area : -1;
#else
    return -1;
#endif
}

Vec2 gmCtfNav_NodePos(int node)
{
    Vec2 p = { 0.0f, 0.0f };
#ifdef TARGET_PC
    if (nav.g != NULL && node >= 0 && node < nav.g->nodes) {
        p.x = nav.g->node[node].x;
        p.y = nav.g->node[node].y;
    }
#endif
    return p;
}

int gmCtfNav_NodeIn(int area, float x, float y)
{
#ifdef TARGET_PC
    int i, best = -1;
    float bd = 1e30f;
    if (nav.g == NULL) {
        return -1;
    }
    for (i = 0; i < nav.g->nodes; i++) {
        float d;
        if (area >= 0 && nav.g->node[i].area != area) {
            continue;
        }
        d = nav_d2(&nav.g->node[i], x, y);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
#else
    return -1;
#endif
}

int gmCtfNav_StandNode(int base)
{
#ifdef TARGET_PC
    return nav.g != NULL ? nav.g->stand[base & 1] : -1;
#else
    return -1;
#endif
}

int gmCtfNav_GuardNode(int base)
{
#ifdef TARGET_PC
    return nav.g != NULL ? nav.g->guard[base & 1] : -1;
#else
    return -1;
#endif
}

int gmCtfNav_AltVia(int from, int to)
{
#ifdef TARGET_PC
    return nav.g != NULL ? nav.g->alt_via[from & 1][to & 1] : -1;
#else
    return -1;
#endif
}

#ifdef TARGET_PC
/* The entry node: the node of `area` that makes the cheapest path from
 * (x, y) to `goal`, skipping `avoid`. */
static int nav_entry(int cls, int area, float x, float y, int goal, int avoid, int* total)
{
    int i, best = -1, bc = NAV_FAR;
    for (i = 0; i < nav.g->nodes; i++) {
        int c;
        if (nav.g->node[i].area != area || i == avoid || nav.cost[cls][i][goal] >= NAV_FAR) {
            continue;
        }
        c = nav_dist10(x, y, nav.g->node[i].x, nav.g->node[i].y) + nav.cost[cls][i][goal];
        if (c < bc) {
            bc = c;
            best = i;
        }
    }
    *total = bc;
    return best;
}
#endif

#ifdef TARGET_PC
static int nav_link_kind(int a, int b)
{
    int i;
    for (i = 0; i < nav.g->links; i++) {
        if (nav.g->link[i].from == a && nav.g->link[i].to == b) {
            return nav.g->link[i].kind;
        }
    }
    return LINK_WALK;
}
#endif

bool gmCtfNav_Step(int cls, int area, float x, float y, int goal, int via, int avoid, CtfNavStep* out)
{
#ifdef TARGET_PC
    /* A node this close (along the floor) is reached: aim at the next one.
     * A hop may start this far from its ledge's node. */
    const float arrive = 8.0f;
    const float hop_reach = 30.0f;
    int entry, total, next;
    out->cost = NAV_FAR;
    out->hop = 0;
    if (nav.g == NULL || area < 0 || goal < 0) {
        return false;
    }
    if (via >= 0) {
        if (nav.g->node[via].area == area) {
            via = -1;
        } else {
            goal = via;
        }
    }
    if (nav.g->node[goal].area == area) {
        out->cost = nav_dist10(x, y, nav.g->node[goal].x, nav.g->node[goal].y);
        return false;
    }
    entry = nav_entry(cls, area, x, y, goal, avoid, &total);
    if (entry < 0) {
        return false;
    }
    out->cost = total;
    next = nav.next[cls][entry][goal];
    if (next == NAV_NONE) {
        return false;
    }
    {
        const float dx = x - nav.g->node[entry].x;
        const float adx = dx < 0.0f ? -dx : dx;
        if (nav.g->node[next].area != area && nav_link_kind(entry, next) == LINK_HOP) {
            /* Off this floor's ledge: from anywhere near the ledge's node,
             * hop toward the next one (aiming back at the node first is what
             * left bots pacing on the pillar, C3's first runs). */
            if (adx >= hop_reach) {
                next = entry;
            } else {
                /* Aim past the landing, along the floor below: aimed at the
                 * landing node itself the bot overshot and drifted back (40
                 * frames off the pillar). */
                const int after = nav.next[cls][next][goal];
                out->hop = nav.g->node[next].x > x ? 1 : -1;
                if (after != NAV_NONE && nav.g->node[after].area == nav.g->node[next].area) {
                    next = after;
                }
            }
        } else if (adx >= arrive) {
            next = entry;
        }
    }
    out->point.x = nav.g->node[next].x;
    out->point.y = nav.g->node[next].y;
    out->node = next;
    return true;
#else
    return false;
#endif
}

int gmCtfNav_Cost(int cls, int area, float x, float y, int goal)
{
#ifdef TARGET_PC
    int total;
    if (nav.g == NULL || area < 0 || goal < 0) {
        return NAV_FAR;
    }
    if (nav.g->node[goal].area == area) {
        return nav_dist10(x, y, nav.g->node[goal].x, nav.g->node[goal].y);
    }
    if (nav_entry(cls, area, x, y, goal, -1, &total) < 0) {
        return NAV_FAR;
    }
    return total;
#else
    return 30000;
#endif
}
