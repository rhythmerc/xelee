#include "ifctf.h"

#ifdef TARGET_PC
#include "forward.h"
#include "ifall.h"
#include "ifstatus.h"
#include "ifstock.h"
#include "types.h"

#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <melee/gm/gm_1601.h>
#include <melee/gm/gmctf.h>
#include <melee/gm/gmvs.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lb_00F9.h>
#include <melee/lb/lbspdisplay.h>
#include <melee/mn/types.h>
#include <melee/pl/player.h>
#include <sysdolphin/baselib/aobj.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjobject.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/state.h>
#include <sysdolphin/baselib/tev.h>
#include <sysdolphin/baselib/tobj.h>

#include "pc/ctf_art.h"

#include <stdlib.h>
#include <string.h>

/* The approved design (docs/xeleenet-modes-plan.md §6e, the hi-fi mockup):
 * each team's score is Melee's own damage digit with a team-coloured flag
 * emblem behind it, and beside it the flag's state as an icon: on its plate
 * (home), with the carrier's stock icon (carried), or tipped inside a gold
 * ring that drains to the return (free; white and faded, blinking, in the
 * last 3 s). With a timer the teams sit either side of it on its row, red
 * mirrored left; without one they meet at the top centre.
 *
 * Units are the HUD camera's: 1 unit = 10 px of the 480-line frame, y up,
 * origin at the screen's centre. */

#define DIGIT_SCALE 1.0f
#define EMBLEM_SIZE 7.0f
#define ICON_SIZE 4.5f    /* the art square; the flag fills 84 % of it */
#define STOCK_SCALE 1.2f
#define TIMER_GAP 5.7f    /* score centre from the timer's edge */
#define STATE_STEP 6.3f   /* state icon centre from the score centre */
#define CENTRE_SCORE 3.0f /* no timer: score centre from the middle */
#define RING_R 2.0f
#define RING_W 0.55f
#define BLINK_FRAMES 180

static const GXColor kEmblem[2] = { { 0xD6, 0x2C, 0x2C, 0xAF }, { 0x30, 0x56, 0xD6, 0xAF } };
static const GXColor kWhite = { 0xFF, 0xFF, 0xFF, 0xFF };
static const GXColor kGold = { 0xF0, 0xC2, 0x26, 0xFF };
static const GXColor kTrack = { 0x28, 0x28, 0x2E, 0xC8 };
static const GXColor kRim = { 0x0A, 0x0A, 0x0C, 0xFF };

typedef struct TeamHud {
    HSD_GObj* digits; /* Melee's damage-number model, the ones digit alone */
    HSD_JObj* ones;
    int shown_digit;
    HSD_GObj* stock; /* the carrier's stock icon */
    HSD_JObj* stock_icon;
    int stock_slot;         /* whose icon it shows, -1 hidden */
    float score_x, state_x; /* centres, HUD units */
} TeamHud;

static struct {
    HSD_GObj* art; /* emblems, flag icons and rings */
    TeamHud team[2];
    float y; /* the row's centre */
    GmCtfHud h;
    bool have_h;
} ifCtf;

/* ---- Melee's digit and stock models ------------------------------------- */

static HSD_JObj* nth_child(HSD_JObj* j, int n)
{
    HSD_JObj* c = j != NULL ? HSD_JObjGetChild(j) : NULL;
    while (c != NULL && n-- > 0) {
        c = HSD_JObjGetNext(c);
    }
    return c;
}

/* The ones digit's texture animation (ifstatus.c get_digit_texanim). */
static HSD_TexAnim* digit_texanim(HudIndex* hud)
{
    HSD_MatAnimJoint* mj = DP(HSD_MatAnimJoint, ((DiscU32*) hud->damage_num_matanims)[0].v);
    HSD_MatAnimJoint* child = DP(HSD_MatAnimJoint, mj->child);
    HSD_MatAnim* matanim = DP(HSD_MatAnim, child->matanim);
    return DP(HSD_TexAnim, matanim->texanim);
}

static void set_digit(TeamHud* t, int d)
{
    HSD_TObj* tobj;
    if (t->ones == NULL || d == t->shown_digit) {
        return;
    }
    t->shown_digit = d;
    tobj = t->ones->u.dobj->mobj->tobj;
    HSD_TObjReqAnimAll(tobj, 2.0f * (float) (d % 10));
    HSD_AObjSetRate(tobj->aobj, 0.0f);
    HSD_TObjAnim(tobj);
}

static void ifCtf_JObjDraw(HSD_GObj* gobj, intptr_t pass)
{
    if (!ifAll_IsHUDHidden()) {
        ifAll_TopJObjCallback(gobj, pass); /* the HUD's top row (XR: above the damage row) */
    }
}

static void make_digits(TeamHud* t, float x, float y)
{
    HudIndex* hud = ifStatus_GetHUDInfo();
    HSD_JObj* j;
    int i;

    t->digits = GObj_Create(0xE, 0xF, 0);
    j = HSD_JObjLoadJoint(hud->damage_num_joint);
    HSD_GObjObject_80390A70(t->digits, HSD_GObj_JObjKind, j);
    GObj_SetupGXLink(t->digits, ifCtf_JObjDraw, 0xB, 0);
    lb_8000C07C(j, 0, (DiscU32*) hud->damage_num_anims, (DiscU32*) hud->damage_num_matanims,
                (DiscU32*) hud->damage_num_shapeanims);
    HSD_JObjReqAnimAll(j, 0.0f);
    HSD_JObjAnimAll(j);
    /* Hundreds, tens and % hidden: the ones digit is the score. */
    for (i = 0; i < HUD_PLACE_MAX; i++) {
        HSD_JObj* c = nth_child(j, i);
        if (c != NULL && i != Ones) {
            HSD_JObjSetFlagsAll(c, JOBJ_HIDDEN);
        }
    }
    t->ones = nth_child(j, Ones);
    if (t->ones != NULL) {
        HSD_TObjAddAnimAll(t->ones->u.dobj->mobj->tobj, digit_texanim(hud));
    }
    {
        Vec3 s = { DIGIT_SCALE, DIGIT_SCALE, DIGIT_SCALE };
        HSD_JObjSetScale(j, &s);
    }
    /* Put the ones digit's own origin on (x, y). */
    HSD_JObjSetTranslateX(j, x - (t->ones != NULL ? HSD_JObjGetTranslationX(t->ones) * DIGIT_SCALE : 0.0f));
    HSD_JObjSetTranslateY(j, y - (t->ones != NULL ? HSD_JObjGetTranslationY(t->ones) * DIGIT_SCALE : 0.0f));
    t->shown_digit = -1;
    set_digit(t, 0);
}

/* The carrier's stock icon, built as the HUD's own heads are in a time match
 * (ifstock.c ifStock_802F98E8 mode 1): the stock model with its animations,
 * every node but the first icon hidden one by one (not the root), and each
 * frame the icon's joint animation at 0 and its texture on the character. */
#define STOCK_NODES 17

static void make_stock(TeamHud* t)
{
    DynamicModelDesc* model = ifStock_PcIconModel();
    HSD_JObj* nodes[STOCK_NODES];
    HSD_JObj* j;
    int i;

    t->stock_slot = -1;
    if (model == NULL) {
        return;
    }
    memset(nodes, 0, sizeof nodes);
    t->stock = GObj_Create(0xE, 0xF, 0);
    j = HSD_JObjLoadJoint(DP(HSD_Joint, model->joint));
    HSD_GObjObject_80390A70(t->stock, HSD_GObj_JObjKind, j);
    GObj_SetupGXLink(t->stock, ifCtf_JObjDraw, 0xB, 0);
    gm_8016895C(j, model, 0);
    HSD_JObjReqAnimAll(j, 0.0f);
    {
        Vec3 s = { STOCK_SCALE, STOCK_SCALE, STOCK_SCALE };
        HSD_JObjSetScale(j, &s);
    }
    lb_80011E24(j, nodes, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, -1);
    t->stock_icon = nodes[1];
    for (i = 2; i < STOCK_NODES; i++) {
        if (nodes[i] != NULL) {
            HSD_JObjSetFlagsAll(nodes[i], JOBJ_HIDDEN);
        }
    }
    if (t->stock_icon != NULL) {
        HSD_JObjSetFlagsAll(t->stock_icon, JOBJ_HIDDEN);
    }
}

static void place_stock(TeamHud* t)
{
    HSD_JObj* j = HSD_GObjGetHSDObj(t->stock);
    /* Bottom-right of the flag, overlapping it, as in the mockup: put the
     * icon node (not the root) there. */
    HSD_JObjSetTranslateX(j, t->state_x + ICON_SIZE * 0.2f - HSD_JObjGetTranslationX(t->stock_icon) * STOCK_SCALE);
    HSD_JObjSetTranslateY(j, ifCtf.y - ICON_SIZE * 0.22f - HSD_JObjGetTranslationY(t->stock_icon) * STOCK_SCALE);
}

static void set_stock(TeamHud* t, int slot)
{
    HSD_TObj* tobj;
    if (t->stock == NULL || t->stock_icon == NULL) {
        return;
    }
    if (slot < 0) {
        if (t->stock_slot >= 0) {
            HSD_JObjSetFlagsAll(t->stock_icon, JOBJ_HIDDEN);
        }
        t->stock_slot = -1;
        return;
    }
    t->stock_slot = slot;
    HSD_JObjClearFlagsAll(t->stock_icon, JOBJ_HIDDEN);
    HSD_JObjReqAnimAll(t->stock_icon, 0.0f);
    tobj = t->stock_icon->u.dobj->mobj->tobj;
    HSD_TObjReqAnimAll(tobj, gm_80168BF8(slot));
    HSD_AObjSetRate(tobj->aobj, 0.0f);
    HSD_JObjAnimAll(HSD_GObjGetHSDObj(t->stock));
    place_stock(t);
}

/* ---- the art ---------------------------------------------------------------- */

static void draw_state(int team, const GmCtfHud* h, float x, float y)
{
    const u16 left = h->return_left[team];
    const PcCtfArt home = team ? PC_CTF_ART_FLAG_BLUE_HOME : PC_CTF_ART_FLAG_RED_HOME;
    const PcCtfArt bare = team ? PC_CTF_ART_FLAG_BLUE : PC_CTF_ART_FLAG_RED;

    if (!h->present[team] || h->home[team]) {
        pc_ctf_art_quad(home, x, y, 0.0f, ICON_SIZE, 0.0f, kWhite);
    } else if (h->holder[team] >= 0) {
        pc_ctf_art_quad(bare, x - ICON_SIZE * 0.12f, y + ICON_SIZE * 0.04f, 0.0f, ICON_SIZE * 0.92f, 0.0f, kWhite);
    } else {
        const int total = h->return_frames;
        const float frac = total > 0 && left > 0 ? (float) left / (float) total : 0.0f;
        const bool blink = left != 0 && left <= BLINK_FRAMES && (left / 6) % 2 == 0;
        GXColor flag = kWhite;
        pc_ctf_arc(x, y, 0.0f, RING_R, RING_W + 0.22f, 0.06f, 1.0f, kRim);
        pc_ctf_arc(x, y, 0.0f, RING_R, RING_W, 0.05f, 1.0f, kTrack);
        pc_ctf_arc(x, y, 0.0f, RING_R, RING_W, 0.05f, frac, blink ? kWhite : kGold);
        if (blink) {
            flag.a = 110;
        }
        pc_ctf_art_quad(bare, x + ICON_SIZE * 0.06f, y, 0.0f, ICON_SIZE * 0.8f, 38.0f, flag);
    }
}

static void ifCtf_ArtDraw(HSD_GObj* gobj, intptr_t pass)
{
    Mtx view;
    int team;
    bool moved;
    (void) gobj;
    (void) pass;
    if (ifAll_IsHUDHidden() || !ifCtf.have_h) {
        return;
    }
    moved = ifAll_TopBegin();
    HSD_StateInvalidate(-1);
    HSD_CObjGetViewingMtx(HSD_CObjGetCurrent(), view);
    GXLoadPosMtxImm(view, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);
    for (team = 0; team < 2; team++) {
        const TeamHud* t = &ifCtf.team[team];
        const float out = team ? 1.0f : -1.0f;
        pc_ctf_art_quad(PC_CTF_ART_EMBLEM, t->score_x + out * 0.58f, ifCtf.y + 0.13f, 0.0f, EMBLEM_SIZE, 0.0f,
                        kEmblem[team]);
        draw_state(team, &ifCtf.h, t->state_x, ifCtf.y);
    }
    HSD_StateInvalidate(-1);
    HSD_StateInitTev();
    HSD_ClearVtxDesc();
    ifAll_TopEnd(moved);
}

/* ---- the process --------------------------------------------------------------- */

static void ifCtf_Think(HSD_GObj* gobj)
{
    int team;
    (void) gobj;
    ifCtf.have_h = gmCtf_GetHud(&ifCtf.h);
    if (!ifCtf.have_h) {
        return;
    }
    for (team = 0; team < 2; team++) {
        TeamHud* t = &ifCtf.team[team];
        const bool carried = ifCtf.h.present[team] && !ifCtf.h.home[team] && ifCtf.h.holder[team] >= 0;
        set_digit(t, ifCtf.h.captures[team]);
        set_stock(t, carried ? ifCtf.h.holder[team] : -1);
    }
}

void ifCtf_Create(void)
{
    StartMeleeRules* rules = gm_GetStartMeleeRules();
    const bool timer = rules != NULL && rules->timer_enabled;
    Vec3* tp = ifAll_GetTimerPosition();
    int team;

    memset(&ifCtf, 0, sizeof ifCtf); /* the last match's were freed with its HUD (ifAll) */
    if (ifAll_GetHUDGObj() == NULL) {
        return;
    }
    /* The row: the timer's. With a timer the teams flank it (its box runs
     * from about 6.9 units left of its anchor to 8.7 right, centiseconds
     * included). */
    ifCtf.y = tp->y;
    for (team = 0; team < 2; team++) {
        TeamHud* t = &ifCtf.team[team];
        const float side = team ? 1.0f : -1.0f;
        if (timer) {
            const float edge = team ? tp->x + 8.7f : tp->x - 6.9f;
            t->score_x = edge + side * TIMER_GAP;
        } else {
            t->score_x = side * CENTRE_SCORE;
        }
        t->state_x = t->score_x + side * STATE_STEP;
    }
    /* Draw order: the art (emblems behind the digits), then the digits,
     * then the stock icons (over the carried flag). */
    ifCtf.art = GObj_Create(0xE, 0xF, 0);
    GObj_SetupGXLink(ifCtf.art, ifCtf_ArtDraw, 0xB, 0);
    HSD_GObj_SetupProc(ifCtf.art, ifCtf_Think, 0x11);
    for (team = 0; team < 2; team++) {
        make_digits(&ifCtf.team[team], ifCtf.team[team].score_x, ifCtf.y);
    }
    for (team = 0; team < 2; team++) {
        make_stock(&ifCtf.team[team]);
    }
    OSReport("ctf-hud: timer %d at %.2f %.2f; red %.2f/%.2f blue %.2f/%.2f\n", timer, tp->x, tp->y,
             ifCtf.team[0].score_x, ifCtf.team[0].state_x, ifCtf.team[1].score_x, ifCtf.team[1].state_x);
    ifCtf_Think(NULL);
}

void ifCtf_Free(void)
{
    int team;
    if (ifCtf.art != NULL) {
        HSD_GObjFree(ifCtf.art);
    }
    for (team = 0; team < 2; team++) {
        if (ifCtf.team[team].digits != NULL) {
            HSD_GObjFree(ifCtf.team[team].digits);
        }
        if (ifCtf.team[team].stock != NULL) {
            HSD_GObjFree(ifCtf.team[team].stock);
        }
    }
    memset(&ifCtf, 0, sizeof ifCtf);
}

#else
void ifCtf_Create(void) {}
void ifCtf_Free(void) {}
#endif
