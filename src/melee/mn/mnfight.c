#include "mnfight.h"

#include "forward.h"
#include "inlines.h"
#include "mnmain.h"
#include "types.h"
#include "mnslippi.h"
#include "pc/audio_menu.h"
#include "pc/graphics_menu.h"
#include "pc/fight_menu.h"
#include <melee/it/forward.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbspdisplay.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/gobjuserdata.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/sislib.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

/* The panel's joint with the sub-screen's title in its tab (MenMainPanel:
 * 84 is "Options", 85 the screen's own, Erase Data's in our frames). */
#define PANEL_TITLE_JOINT 85

/* A page's RmlUi screen (src/pc/fight_menu.cpp, audio_menu.cpp) way in and
 * out, in frames: hidden while the Options rows leave (as Sound's panel
 * waits, its cursor = 0x14), a fade and a rise in, and a fade out while the
 * rows come back. */
enum {
    FIGHT_HOLD_FRAMES = 20,
    FIGHT_IN_FRAMES = 10,
    FIGHT_OUT_FRAMES = 8,
};

typedef enum FightPhase {
    FIGHT_HOLD,
    FIGHT_IN,
    FIGHT_SHOWN,
    FIGHT_OUT,
} FightPhase;

/* An Options page drawn in RmlUi: its kind, the Options row it opens from
 * and goes back to, its tab and first hint, and its screen. The screens take
 * fight_menu.h's PC_FIGHT_MENU_* input. */
typedef struct PcOptionsPage {
    MenuKind kind;
    int row;
    const char* title;
    const char* intro;
    void (*show)(bool show);
    void (*presence)(float presence);
    bool (*input)(int what);
    const char* (*hint)(void);
    bool (*leaving)(void); ///< the screen asks to go back (or NULL)
} PcOptionsPage;

static const PcOptionsPage mnFight_pages[] = {
    { MENU_KIND_SETTINGS_FIGHT, SEL_SETTINGS_FIGHT, "Controls", "Choose what each button does in a fight.",
      pc_fight_menu_show, pc_fight_menu_presence, pc_fight_menu_input, pc_fight_menu_hint, NULL },
    { MENU_KIND_SETTINGS_AUDIO, SEL_SETTINGS_DISPLAY, "Audio", "Set the volumes and how the game sounds.",
      pc_audio_menu_show, pc_audio_menu_presence, pc_audio_menu_input, pc_audio_menu_hint, NULL },
    { MENU_KIND_SETTINGS_GRAPHICS, SEL_SETTINGS_SOUND, "Graphics", "Set the resolution and picture quality.",
      pc_graphics_menu_show, pc_graphics_menu_presence, pc_graphics_menu_input, pc_graphics_menu_hint,
      pc_graphics_menu_leaving },
};

static const PcOptionsPage* mnFight_Page(MenuKind kind)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(mnFight_pages); i++) {
        if (mnFight_pages[i].kind == kind) {
            return &mnFight_pages[i];
        }
    }
    return NULL;
}

typedef struct FightMenu {
    const PcOptionsPage* page;
    HSD_Text* hint;
    char hint_text[128]; ///< what the hint box shows
    FightPhase phase;
    int frames; ///< into the phase
} FightMenu;

static HSD_GObj* mnFight_gobj;
static const PcOptionsPage* mnFight_open; ///< the page taking input

/* The hint box at the stock size holds about 40 letters on its one line;
 * smaller, two lines of about 48. */
enum { HINT_FIT = 40, HINT_FIT_TWO = 48 };
#define HINT_SIZE 0.022f
#define HINT_SIZE_TWO 0.0175f
#define HINT_CELL 32.0f /* a glyph cell, in the text's own units */
#define HINT_LINE 36.0f /* two lines' pitch, a little apart */

HSD_Text* mnFight_HintText(const char* text)
{
    HSD_Text* t = HSD_SisLib_803A6754(0, mn_804D6BB4);
    const size_t n = strlen(text);
    char first[128];
    const char* second = NULL;
    size_t longest = n;
    f32 size = HINT_SIZE;

    t->pos_x = -9.0f;
    t->pos_y = 9.3f;
    t->pos_z = 17.0f;
    t->default_kerning = 1;
    if (n > HINT_FIT) {
        /* Two lines, broken at the space nearest the middle. */
        size_t best = 0, i;
        for (i = 1; i < n && i < sizeof first; i++) {
            if (text[i] == ' ' && (best == 0 || (i > n / 2 ? i - n / 2 : n / 2 - i) <
                                                    (best > n / 2 ? best - n / 2 : n / 2 - best)))
            {
                best = i;
            }
        }
        if (best > 0) {
            memcpy(first, text, best);
            first[best] = '\0';
            second = text + best + 1;
            longest = best > n - best - 1 ? best : n - best - 1;
            size = HINT_SIZE_TWO;
            /* Centred where the one line sits. */
            t->pos_y -= ((HINT_LINE + HINT_CELL) * size - HINT_CELL * HINT_SIZE) / 2.0f;
        }
    }
    t->font_size.x = size;
    t->font_size.y = size;
    {
        /* A line still too long is condensed to fit. */
        const size_t fit = second != NULL ? HINT_FIT_TWO : HINT_FIT;
        if (longest > fit) {
            t->font_size.x = size * (f32) fit / (f32) longest;
        }
    }
    if (second != NULL) {
        HSD_SisLib_803A6B98(t, 0.0f, 0.0f, "%s", first);
        HSD_SisLib_803A6B98(t, 0.0f, HINT_LINE, "%s", second);
    } else {
        HSD_SisLib_803A6B98(t, 0.0f, 0.0f, "%s", text);
    }
    return t;
}

/* The hint box's text: literal, sized and placed as the PC descriptions in
 * mnmain.c's (mnOnline_Description). */
static void mnFight_SetHint(FightMenu* menu, const char* text)
{
    if (menu->hint != NULL) {
        HSD_SisLib_803A5CC4(menu->hint);
    }
    menu->hint = mnFight_HintText(text);
}

static void mnFight_FreeHint(FightMenu* menu)
{
    if (menu->hint != NULL) {
        HSD_SisLib_803A5CC4(menu->hint);
        menu->hint = NULL;
    }
}

/* The page's own GObj: the screen's fades and the hint box. It outlives the
 * page by the fade out (the Options rows already back), then goes. */
static void mnFight_Frame(HSD_GObj* gobj)
{
    FightMenu* menu = HSD_GObjGetUserData(gobj);
    const PcOptionsPage* page = menu->page;
    const char* hint;

    if (mn_804A04F0.cur_menu != page->kind && menu->phase != FIGHT_OUT) {
        /* Options writes its own hint as its rows come back. */
        mnFight_FreeHint(menu);
        menu->phase = FIGHT_OUT;
        menu->frames = 0;
    }
    menu->frames++;
    switch (menu->phase) {
    case FIGHT_HOLD:
        if (menu->frames >= FIGHT_HOLD_FRAMES) {
            page->show(true);
            page->presence(0.0f);
            menu->phase = FIGHT_IN;
            menu->frames = 0;
        }
        return;
    case FIGHT_IN:
        page->presence((f32) menu->frames / FIGHT_IN_FRAMES);
        if (menu->frames >= FIGHT_IN_FRAMES) {
            menu->phase = FIGHT_SHOWN;
        }
        break;
    case FIGHT_SHOWN:
        break;
    case FIGHT_OUT:
        page->presence(1.0f - (f32) menu->frames / FIGHT_OUT_FRAMES);
        if (menu->frames >= FIGHT_OUT_FRAMES) {
            page->show(false);
            mnFight_gobj = NULL;
            HSD_GObjFree(gobj);
        }
        return;
    }
    hint = page->hint();
    if (hint != NULL && strcmp(hint, menu->hint_text) != 0) {
        snprintf(menu->hint_text, sizeof menu->hint_text, "%s", hint);
        mnFight_SetHint(menu, menu->hint_text);
    }
}

static void mnFight_Think(HSD_GObj* gobj)
{
    const PcOptionsPage* page = mnFight_open;
    u64 events;

    if (mn_804D6BC8.cooldown != 0) {
        Menu_DecrementAnimTimer();
        return;
    }
    events = Menu_GetAllInputs();
    /* The screen takes the menus' input; B with nothing to back out of
     * leaves the page, as does the screen's own asking (Graphics' restart
     * question's Later). */
    if (events & MenuInput_Up) {
        page->input(PC_FIGHT_MENU_UP);
    } else if (events & MenuInput_Down) {
        page->input(PC_FIGHT_MENU_DOWN);
    } else if (events & MenuInput_Left) {
        page->input(PC_FIGHT_MENU_LEFT);
    } else if (events & MenuInput_Right) {
        page->input(PC_FIGHT_MENU_RIGHT);
    } else if (events & MenuInput_Confirm) {
        page->input(PC_FIGHT_MENU_CONFIRM);
    }
    if (((events & MenuInput_Back) && !page->input(PC_FIGHT_MENU_BACK)) ||
        (page->leaving != NULL && page->leaving()))
    {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_SETTINGS, page->row, 3);
        return;
    }
}

void mnFight_Open(MenuKind kind)
{
    const PcOptionsPage* page = mnFight_Page(kind);
    HSD_GObj* gobj;
    HSD_GObjProc* proc;
    FightMenu* menu;

    if (page == NULL) {
        return;
    }
    /* Back in before the last fade out finished: that one goes now. */
    if (mnFight_gobj != NULL) {
        FightMenu* old = HSD_GObjGetUserData(mnFight_gobj);
        old->page->show(false);
        mnFight_FreeHint(old);
        HSD_GObjFree(mnFight_gobj);
        mnFight_gobj = NULL;
    }
    mn_804D6BC8.cooldown = 5;
    mn_804A04F0.prev_menu = mn_804A04F0.cur_menu;
    mn_804A04F0.cur_menu = kind;
    mn_804A04F0.hovered_selection = 0;
    mnFight_open = page;

    gobj = GObj_Create(HSD_GOBJ_CLASS_ITEM, 7U, 0x80U);
    mnFight_gobj = gobj;
    menu = HSD_MemAlloc(sizeof(FightMenu));
    menu->page = page;
    menu->hint = NULL;
    menu->hint_text[0] = '\0';
    menu->phase = FIGHT_HOLD;
    menu->frames = 0;
    GObj_InitUserData(gobj, 0U, HSD_Free, menu);
    proc = HSD_GObj_SetupProc(gobj, mnFight_Frame, 0U);
    proc->flags_3 = HSD_GObj_804D783C;
    mnFight_SetHint(menu, page->intro);

    proc = HSD_GObj_SetupProc(GObj_Create(0, 1, 0x80), mnFight_Think, 0);
    proc->flags_3 = HSD_GObj_804D783C;
}

static HSD_JObj* mnFight_titleJoint;

/* The stock titles are slanted (italic) textures on the tab's joint; the
 * font is upright. Before the glyphs go out, the text's position matrix is
 * the joint's world rotation with an x-by-y shear, about the joint's origin
 * (as mnmain.c's mn_PcLabelLean leans the PC row labels). */
static void mnFight_TitleItalic(void* gobj_)
{
    HSD_JObj* jobj = mnFight_titleJoint;
    Mtx view, rot, out;
    float a[3], len;
    int c, r;

    (void) gobj_;
    if (jobj == NULL) {
        return;
    }
    a[0] = jobj->mtx[0][3];
    a[1] = jobj->mtx[1][3];
    a[2] = jobj->mtx[2][3];
    for (c = 0; c < 3; c++) {
        len = sqrtf(jobj->mtx[0][c] * jobj->mtx[0][c] + jobj->mtx[1][c] * jobj->mtx[1][c] +
                    jobj->mtx[2][c] * jobj->mtx[2][c]);
        if (len < 1e-4f) {
            return;
        }
        for (r = 0; r < 3; r++) {
            rot[r][c] = jobj->mtx[r][c] / len;
        }
    }
    {
        /* About the stock titles' slant: x moves right a fifth of y. */
        const float k = 0.2f;
        Mtx shear = { { 1.0f, k, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f, 0.0f } };
        Mtx turned;
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                turned[r][c] = rot[r][0] * shear[0][c] + rot[r][1] * shear[1][c] + rot[r][2] * shear[2][c];
            }
        }
        for (r = 0; r < 3; r++) {
            for (c = 0; c < 3; c++) {
                rot[r][c] = turned[r][c];
            }
            rot[r][3] = a[r] - (rot[r][0] * a[0] + rot[r][1] * a[1] + rot[r][2] * a[2]);
        }
    }
    HSD_CObjGetViewingMtx(HSD_CObjGetCurrent(), view);
    MTXConcat(view, rot, out);
    GXLoadPosMtxImm(out, 0);
}

/* The tab's sub-title of a PC page, or NULL for a stock kind. */
static const char* mnFight_PageTitle(MenuKind kind)
{
    const PcOptionsPage* page = mnFight_Page(kind);

    if (page != NULL) {
        return page->title;
    }
    return kind == MENU_KIND_SLIPPI_PAGE ? mnSlippi_Title() : NULL;
}

/* The tab's title on a PC page: the borrowed page's texture hidden, ours in
 * SIS text where it sits, following it through the tab's slide in and out. */
static HSD_Text* mnFight_title;
static char mnFight_titleShown[32];
static bool mnFight_titleHidden;

void mnFight_PanelTitle(HSD_JObj* panel, MenuKind cur, MenuKind prev, bool leaving)
{
    /* Sampled from the stock sub-titles: a light grey. */
    static const GXColor colour = { 0x9C, 0x9C, 0x9C, 0xFF };
    const char* text = mnFight_PageTitle(cur);
    HSD_JObj* joint = NULL;
    Vec3 pos;

    if (text == NULL && leaving) {
        text = mnFight_PageTitle(prev);
    }
    lb_80011E24(panel, &joint, PANEL_TITLE_JOINT, -1);
    if (joint == NULL) {
        return;
    }
    mnFight_titleJoint = joint;
    if (mnFight_title != NULL && (text == NULL || strcmp(text, mnFight_titleShown) != 0)) {
        HSD_SisLib_803A5CC4(mnFight_title);
        mnFight_title = NULL;
    }
    if (text == NULL) {
        if (mnFight_titleHidden) {
            HSD_JObjClearFlagsAll(joint, JOBJ_HIDDEN);
            mnFight_titleHidden = false;
        }
        return;
    }
    HSD_JObjSetFlagsAll(joint, JOBJ_HIDDEN);
    mnFight_titleHidden = true;
    if (mnFight_title == NULL) {
        snprintf(mnFight_titleShown, sizeof mnFight_titleShown, "%s", text);
        mnFight_title = HSD_SisLib_803A6754(0, mn_804D6BB4);
        mnFight_title->default_kerning = 1;
        /* Condensed a little: 14 letters in the tab that holds "Erase Data". */
        mnFight_title->font_size.x = 0.031f;
        mnFight_title->font_size.y = 0.038f;
        HSD_SisLib_803A6B98(mnFight_title, 0.0f, 0.0f, "%s", mnFight_titleShown);
        mnFight_title->render_callback = mnFight_TitleItalic;
    }
    lb_8000B1CC(joint, NULL, &pos);
    /* The stock sub-titles start a little in from the tab's slanted left edge
     * (Name Entry's, measured: ~12 px of 1280); the joint's origin is on it. */
    mnFight_title->pos_x = pos.x + 0.4f;
    mnFight_title->pos_y = -pos.y - 16.0f * mnFight_title->font_size.y;
    mnFight_title->pos_z = pos.z;
    HSD_SisLib_803A74F0(mnFight_title, 0, (GXColor*) &colour);
}

void mnFight_PanelTitleForget(void)
{
    mnFight_title = NULL;
    mnFight_titleHidden = false;
}
