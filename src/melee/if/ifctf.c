#include "ifctf.h"

#ifdef TARGET_PC
#include "forward.h"
#include "ifall.h"

#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <melee/gm/gmctf.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/sislib.h>
#include <sysdolphin/baselib/state.h>
#include <sysdolphin/baselib/tev.h>

#include <stdio.h>
#include <string.h>

/* The text recipe is ifnet.c's: a SIS canvas on font 2 parented to the HUD
 * camera. Entry coordinates are HUD units, origin at the screen's centre,
 * y down, 1 unit = 10 px of the 4:3 frame; an entry's glyph cell bottom sits
 * at y + 32. Entries are centred on their x (alignment 1). */
#define STRIP_BOTTOM (-12.4f)  /* the strip's text baseline: under the timer */
#define STATE_X 11.0f           /* red's state left of the score, blue's right */
#define SCORE_X 2.2f
#define STATE_SCALE 0.05f
#define SCORE_SCALE 0.09f
#define CARD_BOTTOM (9.0f)
#define CARD_SCALE 0.075f
#define CARD_SIDE_SCALE 0.052f
#define CARD_FRAMES 120 /* READY: gone before the GO! graphic, which covers it */
#define BLINK_FRAMES 180

enum {
    E_SCORE_R,
    E_SCORE_B,
    E_STATE_R,
    E_STATE_B,
    E_CARD_TITLE,
    E_CARD_R,
    E_CARD_B,
    E_COUNT,
};

static const GXColor red = { 0xFF, 0x6A, 0x6A, 0xFF };
static const GXColor blue = { 0x7F, 0xA6, 0xFF, 0xFF };
static const GXColor cream = { 0xFF, 0xF4, 0xD8, 0xFF };

static struct {
    HSD_GObj* gobj;  /* the think process and the panel's draw */
    HSD_Text* text;
    int entry[E_COUNT];
    char shown[E_COUNT][48];
    bool card_up;
} ifCtf;

static void set(int e, const char* s)
{
    if (strcmp(ifCtf.shown[e], s) != 0) {
        HSD_SisLib_803A70A0(ifCtf.text, ifCtf.entry[e], "%s", s);
        snprintf(ifCtf.shown[e], sizeof ifCtf.shown[e], "%s", s);
    }
}

static void state_line(char* out, size_t n, const GmCtfHud* h, int team)
{
    const u16 left = h->return_left[team];
    if (!h->present[team] || h->home[team]) {
        snprintf(out, n, "HOME");
    } else if (h->holder[team] >= 0) {
        snprintf(out, n, "P%d HAS IT", h->holder[team] + 1);
    } else if (left != 0 && left <= BLINK_FRAMES && (left / 6) % 2 == 0) {
        snprintf(out, n, " "); /* the return warning blinks */
    } else if (left != 0) {
        snprintf(out, n, "FREE %d", (left + 59) / 60);
    } else {
        snprintf(out, n, "FREE");
    }
}

static void ifCtf_Think(HSD_GObj* gobj)
{
    GmCtfHud h;
    char line[48];
    (void) gobj;
    if (ifCtf.text == NULL || !gmCtf_GetHud(&h)) {
        return;
    }
    snprintf(line, sizeof line, "%d", h.captures[0]);
    set(E_SCORE_R, line);
    snprintf(line, sizeof line, "%d", h.captures[1]);
    set(E_SCORE_B, line);
    state_line(line, sizeof line, &h, 0);
    set(E_STATE_R, line);
    state_line(line, sizeof line, &h, 1);
    set(E_STATE_B, line);

    ifCtf.card_up = h.frame < CARD_FRAMES;
    if (ifCtf.card_up) {
        set(E_CARD_TITLE, h.swap ? "SIDES SWAPPED" : "CAPTURE THE FLAG");
        snprintf(line, sizeof line, "RED  %s", h.base[0]);
        set(E_CARD_R, line);
        snprintf(line, sizeof line, "BLUE  %s", h.base[1]);
        set(E_CARD_B, line);
    } else {
        set(E_CARD_TITLE, " ");
        set(E_CARD_R, " ");
        set(E_CARD_B, " ");
    }
}

/* A dark panel, in HUD world units (y up). */
static void panel(float l, float r, float b, float t, u8 alpha)
{
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(l, t, 0.0f);
    GXColor4u8(0x0B, 0x0B, 0x10, alpha);
    GXPosition3f32(r, t, 0.0f);
    GXColor4u8(0x0B, 0x0B, 0x10, alpha);
    GXPosition3f32(r, b, 0.0f);
    GXColor4u8(0x0B, 0x0B, 0x10, alpha);
    GXPosition3f32(l, b, 0.0f);
    GXColor4u8(0x0B, 0x0B, 0x10, alpha);
    GXEnd();
}

/* The panels behind the strip and the card; the text draws after them. */
static void ifCtf_Draw(HSD_GObj* gobj, intptr_t pass)
{
    Mtx view;
    const float strip_b = -STRIP_BOTTOM - 0.5f;
    (void) gobj;
    (void) pass;
    if (ifAll_IsHUDHidden() || ifCtf.text == NULL) {
        return;
    }
    HSD_StateInvalidate(-1);
    GXClearVtxDesc();
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZMode(GX_DISABLE, GX_ALWAYS, GX_DISABLE);
    GXSetCullMode(GX_CULL_NONE);
    HSD_CObjGetViewingMtx(HSD_CObjGetCurrent(), view);
    GXLoadPosMtxImm(view, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);

    panel(-STATE_X - 7.5f, STATE_X + 7.5f, strip_b - 0.3f, strip_b + 3.6f, 0xC0);
    if (ifCtf.card_up) {
        panel(-22.0f, 22.0f, -CARD_BOTTOM - 1.2f, -CARD_BOTTOM + 6.2f, 0xD0);
    }
    HSD_StateInvalidate(-1);
    HSD_StateInitTev();
    HSD_ClearVtxDesc();
}

static void entry(int e, float x, float bottom, float scale, const GXColor* c)
{
    GXColor col = *c;
    ifCtf.entry[e] = HSD_SisLib_803A6B98(ifCtf.text, x, bottom - 32.0f, " ");
    HSD_SisLib_803A7548(ifCtf.text, ifCtf.entry[e], scale, scale);
    HSD_SisLib_803A74F0(ifCtf.text, ifCtf.entry[e], &col);
    strcpy(ifCtf.shown[e], " ");
}

void ifCtf_Create(void)
{
    int canvas;
    memset(&ifCtf, 0, sizeof ifCtf); /* the last match's were freed with its HUD (ifAll) */
    if (ifAll_GetHUDGObj() == NULL) {
        return;
    }
    /* The panel first, on the HUD camera's link: it draws before the text's
     * canvas (priority 19 there). */
    ifCtf.gobj = GObj_Create(HSD_GOBJ_CLASS_UI, 15, 0);
    GObj_SetupGXLink(ifCtf.gobj, ifCtf_Draw, 11, 0);
    canvas = HSD_SisLib_803A611C(2, ifAll_GetHUDGObj(), HSD_GOBJ_CLASS_UI, 15, 0, 11, 0, 19);
    ifCtf.text = HSD_SisLib_803A6754(2, canvas);
    ifCtf.text->default_kerning = 1;
    ifCtf.text->default_alignment = 1; /* centred on each entry's x */
    entry(E_SCORE_R, -SCORE_X, STRIP_BOTTOM + 0.2f, SCORE_SCALE, &red);
    entry(E_SCORE_B, SCORE_X, STRIP_BOTTOM + 0.2f, SCORE_SCALE, &blue);
    entry(E_STATE_R, -STATE_X, STRIP_BOTTOM - 0.4f, STATE_SCALE, &red);
    entry(E_STATE_B, STATE_X, STRIP_BOTTOM - 0.4f, STATE_SCALE, &blue);
    entry(E_CARD_TITLE, 0.0f, CARD_BOTTOM - 2.8f, CARD_SCALE, &cream);
    entry(E_CARD_R, -11.0f, CARD_BOTTOM + 0.9f, CARD_SIDE_SCALE, &red);
    entry(E_CARD_B, 11.0f, CARD_BOTTOM + 0.9f, CARD_SIDE_SCALE, &blue);
    HSD_GObj_SetupProc(ifCtf.gobj, ifCtf_Think, 17);
    ifCtf_Think(NULL);
}

void ifCtf_Free(void)
{
    if (ifCtf.gobj != NULL) {
        HSD_GObjFree(ifCtf.gobj);
        ifCtf.gobj = NULL;
    }
    if (ifCtf.text != NULL) {
        HSD_SisLib_803A5CC4(ifCtf.text);
        ifCtf.text = NULL;
    }
}

#else
void ifCtf_Create(void) {}
void ifCtf_Free(void) {}
#endif
