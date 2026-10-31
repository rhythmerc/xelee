#include "mnxeleenet.h"

#include "forward.h"
#include "inlines.h"
#include "mnpage.h"
#include "mnmain.h"
#include "mnnamenew.h"
#include "mncharsel.h"
#include "types.h"
#include "pc/pc.h"
#include "pc/menu_screen.h"
#include "pc/xeleenet_menu.h"
#include <melee/gm/gm_1A36.h>
#include <melee/gm/gm_1A3F.h>
#include <melee/gm/gm_unsplit.h>
#include <melee/gm/types.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lblanguage.h>
#include <sysdolphin/baselib/archive.h>
#include <sysdolphin/baselib/sislib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* XeleeNet's pages (mnxeleenet.h), hosted as the Slippi pages are
 * (mnslippi.c). The page's way in and out, in frames, as Fight Controls':
 * hidden while the panel's tab slides in, then a fade and a rise. */
enum {
    XN_HOLD_FRAMES = 20,
    XN_IN_FRAMES = 10,
};

static struct {
    bool active;
    HSD_Archive* ext; ///< MnExtAll, for the keyboard
    bool page_up;     ///< the frame, backdrop and overlay are built
    int frames;       ///< since the page was built
    HSD_Text* hint;
    char hint_text[128];
} s;

static void mnXeleeNet_Settle(void);

const char* mnXeleeNet_Title(void)
{
    return pc_xn_menu_title(); /* the menu title reads "XeleeNet" (mnpage.c) */
}

bool mnXeleeNet_Active(void)
{
    return s.active;
}

/* The hint box: literal text, placed as the PC descriptions are (mnpage.c). */
static void mnXeleeNet_SetHint(const char* text)
{
    HSD_Text* t;

    if (s.hint != NULL) {
        HSD_SisLib_803A5CC4(s.hint);
    }
    t = mnPage_HintText(text);
    s.hint = t;
}

/* The menu's backdrop, camera and panel, as mnMain_Scene_OnEnter builds them,
 * the panel sliding in from the Slippi menu's frame to ours. */
static void mnXeleeNet_BuildPage(void)
{
    void* dp_[8];

    /* A new scene: the menu's tab title (the sub-menu's own, since they
     * have one) went with the last scene's texts. Drawing through the old
     * pointer drew nothing. */
    mnPage_PanelTitleForget();
    mn_804D6BB8 = lbArchive_LoadSymbols(
        "MnMaAll", &dp_[0], "MenMainBack_Top_joint", &dp_[1], "MenMainBack_Top_animjoint", &dp_[2],
        "MenMainBack_Top_matanim_joint", &dp_[3], "MenMainBack_Top_shapeanim_joint", &MenMain_cam,
        "ScMenMain_cam_int1_camera", &MenMain_lights, "ScMenMain_scene_lights", &MenMain_fog, "ScMenMain_fog",
        &dp_[4], "MenMainPanel_Top_joint", &dp_[5], "MenMainPanel_Top_animjoint", &dp_[6],
        "MenMainPanel_Top_matanim_joint", &dp_[7], "MenMainPanel_Top_shapeanim_joint", 0);
    DP_SET(MenMainBack_Top.joint, dp_[0]);
    DP_SET(MenMainBack_Top.animjoint, dp_[1]);
    DP_SET(MenMainBack_Top.matanim_joint, dp_[2]);
    DP_SET(MenMainBack_Top.shapeanim_joint, dp_[3]);
    DP_SET(MenMainPanel_Top.joint, dp_[4]);
    DP_SET(MenMainPanel_Top.animjoint, dp_[5]);
    DP_SET(MenMainPanel_Top.matanim_joint, dp_[6]);
    DP_SET(MenMainPanel_Top.shapeanim_joint, dp_[7]);
    if (lbLang_IsSavedLanguageUS()) {
        HSD_SisLib_803A62A0(0, "SdMenu.usd", "SIS_MenuData");
    } else {
        HSD_SisLib_803A62A0(0, "SdMenu.dat", "SIS_MenuData");
    }
    mn_804A04F0.cur_menu = MENU_KIND_ONLINE_SLIPPI;
    mn_804A04F0.prev_menu = MENU_KIND_ONLINE_SLIPPI;
    /* As Name Entry builds its screen: the camera, the text canvas the tab
     * and hint draw on (mn_8022BEDC), the backdrop, then the panel. */
    mn_8022C304();
    mn_8022BCF8();
    mn_8022BEDC(mn_8022BE34());
    mn_80229B2C();
    mn_80229DC0();
    /* The panel's think sees the change and plays our range in. */
    mn_804A04F0.cur_menu = MENU_KIND_SLIPPI_PAGE;
    mn_804A04F0.entering_menu = 1;
    mn_804D6BC8.cooldown = 5;

    s.page_up = true;
    s.frames = 0;
    s.hint = NULL;
    s.hint_text[0] = '\0';
}

/* Everything the page built goes, for the keyboard's own screen. */
static void mnXeleeNet_TearDown(void)
{
    if (!s.page_up) {
        return;
    }
    pc_xn_menu_show(false);
    s.hint = NULL; /* freed with every other text: */
    HSD_SisLib_803A5E70();
    mn_8022EBDC();
    mnPage_PanelTitleForget();
    s.page_up = false;
}

static bool mnXeleeNet_KeyboardDone(bool confirmed, const char* text, void* user)
{
    (void) user;
    pc_xn_menu_keyboard_done(confirmed, text);
    mnXeleeNet_Settle();
    return true;
}

/* What comes next: Melee's keyboard for the name, or the page. Leaving is
 * the scene's call (pc_xn_menu_result). */
static void mnXeleeNet_Settle(void)
{
    char initial[32];
    const char* prompt;
    int max_len;

    if (pc_xn_menu_keyboard(initial, &max_len, &prompt)) {
        const char* name = getenv("MELEE_XN_NAME");
        if (name != NULL && name[0] != '\0' && max_len <= 12) {
            pc_xn_menu_keyboard_done(true, name); /* a test's name: no typing */
        } else {
            mnXeleeNet_TearDown();
            mnNameNew_PcOpen(s.ext, initial, max_len, false, true, prompt, mnXeleeNet_KeyboardDone, NULL,
                             false, NULL);
            return;
        }
    }
    if (!s.page_up && !mnNameNew_PcActive()) {
        mnXeleeNet_BuildPage();
    }
}

void mnXeleeNet_Begin(int what)
{
    memset(&s, 0, sizeof s);
    s.active = true;
    /* What the keyboard needs: the menu text and its archive. */
    if (lbLang_IsSavedLanguageUS()) {
        HSD_SisLib_803A62A0(0, "SdMenu.usd", "SIS_MenuData");
        s.ext = lbArchive_LoadArchive("MnExtAll.usd");
    } else {
        HSD_SisLib_803A62A0(0, "SdMenu.dat", "SIS_MenuData");
        s.ext = lbArchive_LoadArchive("MnExtAll.dat");
    }
    pc_xn_menu_begin(what);
    mnXeleeNet_Settle();
}

void mnXeleeNet_Frame(void)
{
    u32 buttons;
    const char* hint;

    if (!s.active) {
        return;
    }
    mnXeleeNet_Settle();
    if (!s.page_up) {
        return;
    }
    s.frames++;
    /* A lobby's Details wants the CSS's portraits: read once a launch. */
    if (pc_xn_menu_portraits_wanted()) {
        mnCharSel_PcPortraits();
    }
    if (s.frames == XN_HOLD_FRAMES) {
        pc_xn_menu_show(true);
        pc_xn_menu_presence(0.0f);
    } else if (s.frames > XN_HOLD_FRAMES) {
        const int in = s.frames - XN_HOLD_FRAMES;
        pc_xn_menu_presence(in >= XN_IN_FRAMES ? 1.0f : (f32) in / XN_IN_FRAMES);
    }
    hint = pc_xn_menu_hint();
    if (hint != NULL && strcmp(hint, s.hint_text) != 0) {
        snprintf(s.hint_text, sizeof s.hint_text, "%s", hint);
        mnXeleeNet_SetHint(s.hint_text);
    }

    if (mn_804D6BC8.cooldown != 0) {
        Menu_DecrementAnimTimer();
        return;
    }
    buttons = mn_80229624(4);
    if (buttons & MenuInput_Up) {
        pc_xn_menu_input(PC_SCREEN_UP);
    } else if (buttons & MenuInput_Down) {
        pc_xn_menu_input(PC_SCREEN_DOWN);
    } else if (buttons & MenuInput_Left) {
        pc_xn_menu_input(PC_SCREEN_LEFT);
    } else if (buttons & MenuInput_Right) {
        pc_xn_menu_input(PC_SCREEN_RIGHT);
    }
    if (buttons & MenuInput_StartButton) {
        pc_xn_menu_input(PC_XN_IN_START);
    } else if (buttons & MenuInput_Confirm) {
        pc_xn_menu_input(PC_SCREEN_CONFIRM);
    } else if (buttons & MenuInput_Back) {
        pc_xn_menu_input(PC_SCREEN_BACK);
    }
    if (buttons & MenuInput_XButton) {
        pc_xn_menu_input(PC_XN_IN_X);
    } else if (buttons & MenuInput_YButton) {
        pc_xn_menu_input(PC_XN_IN_Y);
    }
}

void mnXeleeNet_End(void)
{
    if (!s.active) {
        return;
    }
    pc_xn_menu_end();
    if (s.ext != NULL) {
        lbArchive_80016EFC(s.ext);
        s.ext = NULL;
    }
    mnPage_PanelTitleForget();
    s.active = false;
}
