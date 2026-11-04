#include "mnpage.h"

#include "forward.h"
#include "inlines.h"
#include "mnmain.h"
#include "types.h"
#include "mndatadel.h"
#include "mnonline.h"
#include "mnmodes.h"
#include "mnslippi.h"
#include "mnxeleenet.h"
#include "pc/lang.h"
#include "pc/menu_screen.h"
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

/* The Options pages, each in one of the Options rows (SettingsMenuSelection;
 * a row with no page is hidden). The list's order is the rows'. A game
 * mode's rules page is one too, in a row of VS Mode > Xelee Melee instead
 * (mnmodes.c), with START on to its fight. */
typedef struct PcOptionsPage {
    int row;              ///< the Options row it opens from and goes back to
    const char* name;     ///< the row's label and the page's tab title
    const char* about;    ///< the row's description and the page's first hint
    AnimLoopSettings preview; ///< the preview pane's frames while the row is lit
    const PcMenuScreen* screen;
    /* A mode page's: the menu its row is in (MENU_KIND_XELEE_MELEE; 0 for
     * Options), its name and about as lang keys, and what START (and the
     * screen's PC_SCREEN_GO_START) does. */
    MenuKind parent;
    void (*start)(void);
} PcOptionsPage;

static const PcOptionsPage mnPage_pages[] = {
    /* Screen Display's preview (a screen). */
    { SEL_SETTINGS_SOUND, "Graphics", "Set the resolution and picture quality.", { 1400, 1449, 1420 },
      &pc_graphics_screen },
    /* Sound's (speakers). The stock Sound page's mono and balance mean
     * nothing beside the volumes (gm_801603B0), as Screen Display's
     * deflicker and video modes. */
    { SEL_SETTINGS_DISPLAY, "Audio", "Set the volumes and how the game sounds.", { 1350, 1399, 1370 },
      &pc_audio_screen },
    /* Erase Data's (the progress it erases, Gameplay's last action): no
     * stock preview shows rules. */
    { SEL_SETTINGS_ERASE, "Gameplay", "Set how fights play, and your saved progress.", { 1550, 1599, 1570 },
      &pc_gameplay_screen },
    /* Rumble's (a controller): Rumble's switch is in Controls. */
    { SEL_SETTINGS_FIGHT, "Controls", "Choose what each button does in a fight.", { 1300, 1349, 1320 },
      &pc_controls_screen },
    /* Language's (unused otherwise). Last: the list's end. */
    { SEL_SETTINGS_CREDITS, "Credits", "Xelee credits.", { 1500, 1549, 1520 }, &pc_credits_screen },
    /* VS Mode > Xelee Melee > Capture the Flag: Melee's Rules page's place
     * before the CSS (docs/xeleenet-modes-plan.md §2.3). */
    { SEL_XELEE_MELEE_CTF, "modes.ctf.tab", "modes.ctf.desc", { 700, 749, 720 }, &pc_ctf_rules_screen,
      MENU_KIND_XELEE_MELEE, mnModes_StartCtf },
};

/* Options' pages are the ones without a parent menu of their own. */
static bool mnPage_InOptions(const PcOptionsPage* page)
{
    return page->parent == 0;
}

/* The menu kind a page shows as: one for all of Options' (Erase Data's
 * panel, Options' colours), one for the modes' (VS. Mode's red frame). */
static MenuKind mnPage_Kind(const PcOptionsPage* page)
{
    return mnPage_InOptions(page) ? MENU_KIND_SETTINGS_PAGE : MENU_KIND_MODE_PAGE;
}

/* A page's tab title and first hint: literal for Options' pages, lang keys
 * for a mode's. */
static const char* mnPage_Name(const PcOptionsPage* page)
{
    return mnPage_InOptions(page) ? page->name : pc_tr(page->name);
}

static const char* mnPage_About(const PcOptionsPage* page)
{
    return mnPage_InOptions(page) ? page->about : pc_tr(page->about);
}

static const PcOptionsPage* mnPage_Find(MenuKind parent, int row)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(mnPage_pages); i++) {
        if (mnPage_pages[i].parent == parent && mnPage_pages[i].row == row) {
            return &mnPage_pages[i];
        }
    }
    return NULL;
}

static const PcOptionsPage* mnPage_ForRow(int row)
{
    return mnPage_Find(0, row);
}

bool mnPage_HasRow(int row)
{
    return mnPage_ForRow(row) != NULL;
}

const char* mnPage_Label(int row)
{
    const PcOptionsPage* page = mnPage_ForRow(row);

    return page != NULL ? page->name : NULL;
}

const char* mnPage_Description(int row)
{
    const PcOptionsPage* page = mnPage_ForRow(row);

    return page != NULL ? page->about : NULL;
}

int mnPage_FirstRow(void)
{
    int row = mnPage_pages[0].row;
    size_t i;

    for (i = 1; i < ARRAY_SIZE(mnPage_pages); i++) {
        if (mnPage_InOptions(&mnPage_pages[i]) && mnPage_pages[i].row < row) {
            row = mnPage_pages[i].row;
        }
    }
    return row;
}

int mnPage_RowCount(void)
{
    int count = 0;
    size_t i;

    for (i = 0; i < ARRAY_SIZE(mnPage_pages); i++) {
        if (mnPage_InOptions(&mnPage_pages[i]) && mnPage_pages[i].row + 1 > count) {
            count = mnPage_pages[i].row + 1;
        }
    }
    return count;
}

void mnPage_Previews(AnimLoopSettings* rows, int count)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(mnPage_pages); i++) {
        if (mnPage_InOptions(&mnPage_pages[i]) && mnPage_pages[i].row < count) {
            rows[mnPage_pages[i].row] = mnPage_pages[i].preview;
        }
    }
}

void mnPage_Update(void)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(mnPage_pages); i++) {
        mnPage_pages[i].screen->update();
    }
}

/* A page's screen way in and out, in frames: hidden while the Options rows
 * leave (as Sound's panel waits, its cursor = 0x14), a fade and a rise in,
 * and a fade out while the rows come back. */
enum {
    PAGE_HOLD_FRAMES = 20,
    PAGE_IN_FRAMES = 10,
    PAGE_OUT_FRAMES = 8,
};

typedef enum PagePhase {
    PAGE_HOLD,
    PAGE_IN,
    PAGE_SHOWN,
    PAGE_OUT,
} PagePhase;

typedef struct PageState {
    const PcOptionsPage* page;
    HSD_Text* hint;
    char hint_text[128]; ///< what the hint box shows
    PagePhase phase;
    int frames; ///< into the phase
} PageState;

static HSD_GObj* mnPage_gobj;
static const PcOptionsPage* mnPage_open; ///< the page taking input (or last open)
/* The page that sent the player to a stock page or scene (Erase Data,
 * placing the stage), to come back to; NULL for none. */
static const PcOptionsPage* mnPage_away;

/* The hint box at the stock size holds about 40 letters on its one line;
 * smaller, two lines of about 48. */
enum { HINT_FIT = 40, HINT_FIT_TWO = 48 };
#define HINT_SIZE 0.022f
#define HINT_SIZE_TWO 0.0175f
#define HINT_CELL 32.0f /* a glyph cell, in the text's own units */
#define HINT_LINE 36.0f /* two lines' pitch, a little apart */

HSD_Text* mnPage_HintText(const char* text)
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
static void mnPage_SetHint(PageState* state, const char* text)
{
    if (state->hint != NULL) {
        HSD_SisLib_803A5CC4(state->hint);
    }
    state->hint = mnPage_HintText(text);
}

static void mnPage_FreeHint(PageState* state)
{
    if (state->hint != NULL) {
        HSD_SisLib_803A5CC4(state->hint);
        state->hint = NULL;
    }
}

/* The page's own GObj: the screen's fades and the hint box. It outlives the
 * page by the fade out (the Options rows already back), then goes. */
static void mnPage_Frame(HSD_GObj* gobj)
{
    PageState* state = HSD_GObjGetUserData(gobj);
    const PcMenuScreen* screen = state->page->screen;
    const char* hint;

    if (mn_804A04F0.cur_menu != mnPage_Kind(state->page) && state->phase != PAGE_OUT) {
        /* Options writes its own hint as its rows come back. */
        mnPage_FreeHint(state);
        state->phase = PAGE_OUT;
        state->frames = 0;
    }
    state->frames++;
    switch (state->phase) {
    case PAGE_HOLD:
        if (state->frames >= PAGE_HOLD_FRAMES) {
            screen->show(true);
            screen->presence(0.0f);
            state->phase = PAGE_IN;
            state->frames = 0;
        }
        return;
    case PAGE_IN:
        screen->presence((f32) state->frames / PAGE_IN_FRAMES);
        if (state->frames >= PAGE_IN_FRAMES) {
            state->phase = PAGE_SHOWN;
        }
        break;
    case PAGE_SHOWN:
        break;
    case PAGE_OUT:
        screen->presence(1.0f - (f32) state->frames / PAGE_OUT_FRAMES);
        if (state->frames >= PAGE_OUT_FRAMES) {
            screen->show(false);
            mnPage_gobj = NULL;
            HSD_GObjFree(gobj);
        }
        return;
    }
    hint = screen->hint();
    if (hint != NULL && strcmp(hint, state->hint_text) != 0) {
        snprintf(state->hint_text, sizeof state->hint_text, "%s", hint);
        mnPage_SetHint(state, state->hint_text);
    }
}

static void mnPage_Think(HSD_GObj* gobj)
{
    const PcOptionsPage* page = mnPage_open;
    const PcMenuScreen* screen = page->screen;
    u64 events;
    int where;

    if (mn_804D6BC8.cooldown != 0) {
        Menu_DecrementAnimTimer();
        return;
    }
    events = Menu_GetAllInputs();
    /* A mode's page: START goes on to its fight from any row (its bottom
     * button does too, below), where Options' pages take it as A. Like the
     * rest of the input, only once the page is in. */
    if (page->start != NULL && (events & MenuInput_StartButton) && mnPage_gobj != NULL &&
        ((PageState*) HSD_GObjGetUserData(mnPage_gobj))->phase == PAGE_SHOWN)
    {
        where = PC_SCREEN_GO_START;
        goto leave;
    }
    /* The screen takes the menus' input; B with nothing to back out of
     * leaves the page, as does the screen's own asking (Graphics' restart
     * question's Later), which can also go on to a stock page or scene. */
    if (events & MenuInput_Up) {
        screen->input(PC_SCREEN_UP);
    } else if (events & MenuInput_Down) {
        screen->input(PC_SCREEN_DOWN);
    } else if (events & MenuInput_Left) {
        screen->input(PC_SCREEN_LEFT);
    } else if (events & MenuInput_Right) {
        screen->input(PC_SCREEN_RIGHT);
    } else if (events & MenuInput_Confirm) {
        screen->input(PC_SCREEN_CONFIRM);
    }
    where = screen->leaving != NULL ? screen->leaving() : PC_SCREEN_STAY;
leave:
    switch (where) {
    case PC_SCREEN_GO_START:
        if (page->start == NULL) {
            break;
        }
        /* The scene ends under the page, as for placing the stage; coming
         * back (B in the CSS) opens it again (gmmenumode.c). */
        screen->show(false);
        page->start();
        HSD_GObjFree(gobj);
        return;
    case PC_SCREEN_GO_ERASE_DATA:
        mnPage_away = page;
        mn_804A04F0.entering_menu = 1;
        mnDataDel_80250170();
        HSD_GObjFree(gobj);
        return;
    case PC_SCREEN_GO_PLACE_STAGE:
    case PC_SCREEN_GO_CALIBRATE:
        mnPage_away = page;
        screen->show(false); /* the scene ends under it */
        if (where == PC_SCREEN_GO_CALIBRATE) {
            mnOnline_CalibrateFromOptions();
        } else {
            mnOnline_PlaceFromOptions();
        }
        return;
    default:
        break;
    }
    if (where == PC_SCREEN_GO_BACK || ((events & MenuInput_Back) && !screen->input(PC_SCREEN_BACK))) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(mnPage_InOptions(page) ? MENU_KIND_SETTINGS : page->parent, page->row, 3);
        return;
    }
}

bool mnPage_Open(int row)
{
    return mnPage_OpenIn(0, row);
}

bool mnPage_OpenIn(MenuKind parent, int row)
{
    const PcOptionsPage* page = mnPage_Find(parent, row);
    HSD_GObj* gobj;
    HSD_GObjProc* proc;
    PageState* state;

    if (page == NULL) {
        return false;
    }
    /* Back in before the last fade out finished: that one goes now. */
    if (mnPage_gobj != NULL) {
        PageState* old = HSD_GObjGetUserData(mnPage_gobj);
        old->page->screen->show(false);
        mnPage_FreeHint(old);
        HSD_GObjFree(mnPage_gobj);
        mnPage_gobj = NULL;
    }
    mn_804D6BC8.cooldown = 5;
    mn_804A04F0.prev_menu = mn_804A04F0.cur_menu;
    mn_804A04F0.cur_menu = mnPage_Kind(page);
    mn_804A04F0.hovered_selection = 0;
    mnPage_open = page;

    gobj = GObj_Create(HSD_GOBJ_CLASS_ITEM, 7U, 0x80U);
    mnPage_gobj = gobj;
    state = HSD_MemAlloc(sizeof(PageState));
    state->page = page;
    state->hint = NULL;
    state->hint_text[0] = '\0';
    state->phase = PAGE_HOLD;
    state->frames = 0;
    GObj_InitUserData(gobj, 0U, HSD_Free, state);
    proc = HSD_GObj_SetupProc(gobj, mnPage_Frame, 0U);
    proc->flags_3 = HSD_GObj_804D783C;
    mnPage_SetHint(state, mnPage_About(page));

    proc = HSD_GObj_SetupProc(GObj_Create(0, 1, 0x80), mnPage_Think, 0);
    proc->flags_3 = HSD_GObj_804D783C;
    return true;
}

bool mnPage_Return(void)
{
    const PcOptionsPage* page = mnPage_away;

    mnPage_away = NULL;
    return page != NULL && mnPage_Open(page->row);
}

int mnPage_ReturnRow(void)
{
    const int row = mnPage_away != NULL ? mnPage_away->row : mnPage_FirstRow();

    mnPage_away = NULL;
    return row;
}

static HSD_JObj* mnPage_titleJoint;

/* The stock titles are slanted (italic) textures on the tab's joint; the
 * font is upright. Before the glyphs go out, the text's position matrix is
 * the joint's world matrix with an x-by-y shear, about the joint's origin
 * (as mnmain.c's mn_PcLabelLean leans the PC row labels). The joint's scale
 * stays in: the tab's slide out squeezes the title to nothing across (x
 * 1 to 0.001 in 9 frames, sliding right) and the slide in grows it back,
 * so the text goes and comes with the stock textures. */
static void mnPage_TitleItalic(void* gobj_)
{
    HSD_JObj* jobj = mnPage_titleJoint;
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
        for (r = 0; r < 3; r++) {
            /* A joint squeezed flat draws nothing, as its texture would. */
            rot[r][c] = len < 1e-6f ? 0.0f : jobj->mtx[r][c];
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
static const char* mnPage_Title(MenuKind kind)
{
    if (kind == MENU_KIND_SETTINGS_PAGE || kind == MENU_KIND_MODE_PAGE) {
        return mnPage_open != NULL ? mnPage_Name(mnPage_open) : NULL;
    }
    if (kind == MENU_KIND_XELEE_MELEE) {
        /* Special Melee's panel: its sub-title reads Xelee Melee instead. */
        return pc_tr("modes.tab");
    }
    if (kind == MENU_KIND_SLIPPI_PAGE) {
        return mnXeleeNet_Active() ? mnXeleeNet_Title() : mnSlippi_Title();
    }
    return NULL;
}

/* XeleeNet's pages: the menu's own title ("VS. Mode", joint 84) reads
 * "XeleeNet" too, as the wireframe's tab does, drawn the same way. Joint 86,
 * at the same place, is the parent's title (Main Menu in VS. Mode's range). */
typedef struct PanelMainTitle {
    int index;
    void (*italic)(void* gobj);
    HSD_JObj* joint;
    HSD_Text* text;
    char shown[32];
    bool hidden;
} PanelMainTitle;

static void mnPage_MainItalic(void* gobj);
static void mnPage_ParentItalic(void* gobj);
static PanelMainTitle mnPage_mainTitle = { 84, mnPage_MainItalic };
static PanelMainTitle mnPage_parentTitle = { 86, mnPage_ParentItalic };

static void mnPage_ItalicOn(void* gobj, HSD_JObj* joint)
{
    HSD_JObj* keep = mnPage_titleJoint;
    mnPage_titleJoint = joint;
    mnPage_TitleItalic(gobj);
    mnPage_titleJoint = keep;
}

static void mnPage_MainItalic(void* gobj)
{
    mnPage_ItalicOn(gobj, mnPage_mainTitle.joint);
}

static void mnPage_ParentItalic(void* gobj)
{
    mnPage_ItalicOn(gobj, mnPage_parentTitle.joint);
}

static void mnPage_PanelMainTitle(HSD_JObj* panel, PanelMainTitle* title, const char* text)
{
    static const GXColor colour = { 0x9C, 0x9C, 0x9C, 0xFF };
    HSD_JObj* joint = NULL;
    Vec3 pos;

    lb_80011E24(panel, &joint, title->index, -1);
    if (joint == NULL) {
        return;
    }
    title->joint = joint;
    if (text == NULL) {
        if (title->text != NULL) {
            HSD_SisLib_803A5CC4(title->text);
            title->text = NULL;
        }
        if (title->hidden) {
            HSD_JObjClearFlagsAll(joint, JOBJ_HIDDEN);
            title->hidden = false;
        }
        return;
    }
    HSD_JObjSetFlagsAll(joint, JOBJ_HIDDEN);
    title->hidden = true;
    /* Made again when the text changed (Online Mode to a network's tab, or
     * XeleeNet to LAN). */
    if (title->text != NULL && strcmp(title->shown, text) != 0) {
        HSD_SisLib_803A5CC4(title->text);
        title->text = NULL;
    }
    if (title->text == NULL) {
        snprintf(title->shown, sizeof title->shown, "%s", text);
        title->text = HSD_SisLib_803A6754(0, mn_804D6BB4);
        title->text->default_kerning = 1;
        title->text->font_size.x = 0.034f;
        title->text->font_size.y = 0.040f;
        HSD_SisLib_803A6B98(title->text, 0.0f, 0.0f, "%s", title->shown);
        title->text->render_callback = title->italic;
    }
    lb_8000B1CC(joint, NULL, &pos);
    title->text->pos_x = pos.x + 0.4f;
    title->text->pos_y = -pos.y - 16.0f * title->text->font_size.y;
    title->text->pos_z = pos.z;
    HSD_SisLib_803A74F0(title->text, 0, (GXColor*) &colour);
}

/* The tab's main title over a PC kind, or NULL for the stock texture. The
 * Online Mode menus borrow VS. Mode's panel, whose title would read VS. Mode:
 * Online Mode itself reads Online Mode, and each of its rows' menus and pages
 * its own name (Slippi; XeleeNet or LAN, the language overlay). */
static const char* mnPage_MainTitle(MenuKind kind)
{
    switch (kind) {
    case MENU_KIND_ONLINE:
        return pc_tr("online.title");
    case MENU_KIND_ONLINE_SLIPPI:
        return pc_tr("slippi.menu.tab");
    case MENU_KIND_ONLINE_XN:
        return pc_tr("xeleenet.menu.tab");
    case MENU_KIND_SLIPPI_PAGE: /* Slippi's sign-in pages, or XeleeNet's */
        return pc_tr(mnXeleeNet_Active() ? "xeleenet.menu.tab" : "slippi.menu.tab");
    default:
        return NULL;
    }
}

/* The tab's title on a PC page: the borrowed page's texture hidden, ours in
 * SIS text where it sits, following it through the tab's slide in and out. */
static HSD_Text* mnPage_title;
static char mnPage_titleShown[32];
static bool mnPage_titleHidden;

void mnPage_PanelTitle(HSD_JObj* panel, MenuKind cur, MenuKind prev, int state)
{
    /* Sampled from the stock sub-titles: a light grey. */
    static const GXColor colour = { 0x9C, 0x9C, 0x9C, 0xFF };
    const bool leaving = state == 1;
    const char* text = mnPage_Title(cur);
    HSD_JObj* joint = NULL;
    Vec3 pos;

    if (text == NULL && leaving) {
        text = mnPage_Title(prev);
    }
    /* While the tab slides out, the menu it is leaving keeps its title. */
    mnPage_PanelMainTitle(panel, &mnPage_mainTitle, mnPage_MainTitle(leaving ? prev : cur));
    /* The parent menu's title, beside it: squeezed out as a sub-menu slides
     * in, grown back as one slides out. VS. Mode's range has Main Menu there,
     * so Online Mode's sub-menus put their parent's in its place. */
    mnPage_PanelMainTitle(panel, &mnPage_parentTitle,
                          state == 2 ? NULL : mnPage_MainTitle(leaving ? cur : prev));
    lb_80011E24(panel, &joint, PANEL_TITLE_JOINT, -1);
    if (joint == NULL) {
        return;
    }
    mnPage_titleJoint = joint;
    if (mnPage_title != NULL && (text == NULL || strcmp(text, mnPage_titleShown) != 0)) {
        HSD_SisLib_803A5CC4(mnPage_title);
        mnPage_title = NULL;
    }
    if (text == NULL) {
        if (mnPage_titleHidden) {
            HSD_JObjClearFlagsAll(joint, JOBJ_HIDDEN);
            mnPage_titleHidden = false;
        }
        return;
    }
    HSD_JObjSetFlagsAll(joint, JOBJ_HIDDEN);
    mnPage_titleHidden = true;
    if (mnPage_title == NULL) {
        snprintf(mnPage_titleShown, sizeof mnPage_titleShown, "%s", text);
        mnPage_title = HSD_SisLib_803A6754(0, mn_804D6BB4);
        mnPage_title->default_kerning = 1;
        /* Condensed a little: 14 letters in the tab that holds "Erase Data". */
        mnPage_title->font_size.x = 0.031f;
        mnPage_title->font_size.y = 0.038f;
        HSD_SisLib_803A6B98(mnPage_title, 0.0f, 0.0f, "%s", mnPage_titleShown);
        mnPage_title->render_callback = mnPage_TitleItalic;
    }
    lb_8000B1CC(joint, NULL, &pos);
    /* The stock sub-titles start a little in from the tab's slanted left edge
     * (Name Entry's, measured: ~12 px of 1280); the joint's origin is on it. */
    mnPage_title->pos_x = pos.x + 0.4f;
    mnPage_title->pos_y = -pos.y - 16.0f * mnPage_title->font_size.y;
    mnPage_title->pos_z = pos.z;
    HSD_SisLib_803A74F0(mnPage_title, 0, (GXColor*) &colour);
}

void mnPage_PanelTitleForget(void)
{
    mnPage_title = NULL;
    mnPage_titleHidden = false;
    mnPage_mainTitle.text = NULL;
    mnPage_mainTitle.hidden = false;
    mnPage_parentTitle.text = NULL;
    mnPage_parentTitle.hidden = false;
}

void mnPage_SceneForget(void)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(mnPage_pages); i++) {
        mnPage_pages[i].screen->show(false);
    }
    mnPage_gobj = NULL;
    mnPage_open = NULL;
    mnPage_PanelTitleForget();
}
