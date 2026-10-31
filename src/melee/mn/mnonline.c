#include "mnonline.h"

#include "forward.h"
#include "inlines.h"
#include "mnmain.h"
#include "mnpage.h"
#include "types.h"
#include <melee/gm/gmonlinemode.h>
#include <stdio.h>
#include <melee/gm/gmscene.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <sysdolphin/baselib/gobj.h>

#include "pc/lang.h"
#include "pc/net_match.h"
#include "pc/net_punch.h"
#include "pc/slippi/slp_user.h"
#include "pc/xr_calibrate.h"
#include "pc/xr_place.h"
#include "pc/xeleenet.h"

/* Main Menu > Online Mode. Each entry selects its GM_ONLINE lobby mode. The
 * labels are drawn text (the menu's own labels are textures with no room for
 * new words); all of it is keys (resources/lang/en.yaml). */

static const char* const online_labels[] = {
    "xeleenet.menu.online.label",
    "slippi.menu.online.label",
    "online.lan.label",
};

static const char* const online_descriptions[] = {
    "xeleenet.menu.online.desc",
    "slippi.menu.online.desc",
    "online.lan.desc",
};

/* Online Mode > Slippi: signed out it is Login and Register; signed in it is
 * Unranked, Direct, Place Stage (only with a headset presenting) and Logout. */
enum { row_unranked, row_direct, row_place, row_logout, row_count };
static const char* const slippi_in_rows[row_count] = { "slippi.menu.unranked", "slippi.menu.direct",
                                                       "slippi.menu.place_stage", "slippi.menu.logout" };
static const char* const slippi_out_rows[] = { "slippi.menu.login", "slippi.menu.register" };

/* `row`'s label or desc ("slippi.menu.login" + ".label"). */
static const char* slippiText(const char* row, const char* what)
{
    char key[64];
    snprintf(key, sizeof key, "%s.%s", row, what);
    return pc_tr(key);
}

/* The signed-in menu's row at `selection`, or -1. */
static int slippiRow(int selection)
{
    const bool place = pc_xr_place_available();
    if (selection < 0 || selection >= (place ? row_count : row_count - 1)) {
        return -1;
    }
    return !place && selection >= row_place ? selection + 1 : selection;
}

/* Online Mode > XeleeNet (wireframe M2): Host, Join, Name, by key.
 * The Name row shows the name and the key's tag. */
static const char* const xn_labels[] = {
    "xeleenet.menu.online.host.label",
    "xeleenet.menu.online.join.label",
    "xeleenet.menu.online.name.label",
};
static const char* const xn_descriptions[] = {
    "xeleenet.menu.online.host.desc",
    "xeleenet.menu.online.join.desc",
    "xeleenet.menu.online.name.desc",
};

/* The Name row's description: the name and the key's tag, since the pill has
 * no room for them (SIS text has no ';' either). */
static const char* xnNameDescription(void)
{
    static char text[128];
    char full[48];
    const char* name = pc_xn_name();
    const char* tag = pc_xn_tag();
    if (name[0] == '\0') {
        return pc_tr("xeleenet.menu.online.name.desc_none");
    }
    snprintf(full, sizeof full, "%s%s%s", name, tag[0] ? "#" : "", tag);
    return pc_trf(text, sizeof text, xn_descriptions[SEL_XN_NAME], "name", full, NULL);
}

static const char* notice;

static int slippi_built_count;

int mnOnline_SlippiCount(void)
{
    return pc_net_match_slippi_ready() ? (pc_xr_place_available() ? row_count : row_count - 1) : 2;
}

void mnOnline_SlippiBuilt(void)
{
    slippi_built_count = mnOnline_SlippiCount();
}

const char* mnOnline_Label(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_MAIN && selection == SEL_MAIN_ONLINE) {
        return pc_tr("online.label");
    }
    if (kind == MENU_KIND_SETTINGS) {
        return mnPage_Label(selection); /* its pages' (mnpage.c) */
    }
    if (kind == MENU_KIND_ONLINE && selection >= 0 &&
        selection < (int) ARRAY_SIZE(online_labels))
    {
        return pc_tr(online_labels[selection]);
    }
    if (kind == MENU_KIND_ONLINE_XN && selection >= 0 &&
        selection < (int) ARRAY_SIZE(xn_labels))
    {
        return pc_tr(xn_labels[selection]);
    }
    if (kind == MENU_KIND_ONLINE_SLIPPI) {
        if (!pc_net_match_slippi_ready()) {
            return selection >= 0 && selection < (int) ARRAY_SIZE(slippi_out_rows)
                       ? slippiText(slippi_out_rows[selection], "label")
                       : NULL;
        }
        if (slippiRow(selection) >= 0) {
            return slippiText(slippi_in_rows[slippiRow(selection)], "label");
        }
    }
    return NULL;
}

const char* mnOnline_Description(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_MAIN && selection == SEL_MAIN_ONLINE) {
        return pc_tr("online.desc");
    }
    if (kind == MENU_KIND_SETTINGS) {
        return mnPage_Description(selection);
    }
    if (kind == MENU_KIND_ONLINE && selection >= 0 &&
        selection < (int) ARRAY_SIZE(online_descriptions))
    {
        return pc_tr(online_descriptions[selection]);
    }
    if (kind == MENU_KIND_ONLINE_XN && selection >= 0 &&
        selection < (int) ARRAY_SIZE(xn_descriptions))
    {
        return selection == SEL_XN_NAME ? xnNameDescription() : pc_tr(xn_descriptions[selection]);
    }
    if (kind == MENU_KIND_ONLINE_SLIPPI) {
        if (!pc_net_match_slippi_ready()) {
            return selection >= 0 && selection < (int) ARRAY_SIZE(slippi_out_rows)
                       ? slippiText(slippi_out_rows[selection], "desc")
                       : NULL;
        }
        if (slippiRow(selection) >= 0) {
            return slippiText(slippi_in_rows[slippiRow(selection)], "desc");
        }
    }
    return NULL;
}

const char* mnOnline_TakeNotice(void)
{
    const char* s = notice;
    notice = NULL;
    return s;
}

static void enterOnline(OnlineKind kind)
{
    MenuExitData* data = gm_GetCurrentSceneExitData();
    sfxForward();
    gmOnline_SetKind(kind);
    data->pending_mode = GM_ONLINE;
    gm_801A4B60();
}

/* Unranked, Direct or LAN with a headset presenting calibrates the room
 * light first, once a launch with Roomtone (src/pc/xr_calibrate.h), and
 * places the stage if Place Stage hasn't run this launch, even with a stage
 * spot found in the room: online there is no hold before each fight, so
 * this is the player's only chance (src/pc/xr_place.h). Then it goes on to
 * the mode. */
static void enterAfterPlacing(OnlineKind kind)
{
    const bool calibrate = pc_xr_calibrate_needed();
    const bool place = pc_xr_place_needed_online();
    if (calibrate || place) {
        pc_xr_place_plan(calibrate, place,
                         place ? pc_tr("online.place") :
                         kind == ONLINE_KIND_UNRANKED ? pc_tr("slippi.menu.place.unranked") :
                         kind == ONLINE_KIND_DIRECT ? pc_tr("slippi.menu.place.direct") :
                                                      pc_tr("xeleenet.menu.place"));
        gmOnline_SetKindAfterPlace(kind);
        enterOnline(ONLINE_KIND_PLACE);
        return;
    }
    enterOnline(kind);
}

void mnOnline_PlaceFromOptions(void)
{
    gmOnline_SetPlaceFromOptions(true);
    enterOnline(ONLINE_KIND_PLACE);
}

void mnOnline_CalibrateFromOptions(void)
{
    pc_xr_place_plan(true, false, "BACK TO OPTIONS");
    gmOnline_SetPlaceFromOptions(true);
    enterOnline(ONLINE_KIND_PLACE);
}

/* Up/Down wrap over `count` entries. */
static void moveHover(u32 buttons, int count)
{
    if (buttons & MenuInput_Up) {
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + count - 1) % count;
    } else if (buttons & MenuInput_Down) {
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + 1) % count;
    }
}

/// @brief Online menu think, after mn_8022D594: Slippi or LAN
void mnOnline_Think(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);

    mn_804A04F0.buttons = buttons;
    if (buttons & MenuInput_Confirm) {
        /* As the stock thinks do. Left at 0 by a B, the panel played the way
         * in as a way out: Online Mode's title leaving, Main Menu arriving. */
        mn_804A04F0.entering_menu = 1;
        switch (mn_804A04F0.hovered_selection) {
        case SEL_ONLINE_XN:
        case SEL_ONLINE_LAN:
            /* LAN is XeleeNet's menu and pages on this network: the lobby
             * service is the host's game (docs/lan-lobbies-plan.md). */
            sfxForward();
            pc_xn_set_network(mn_804A04F0.hovered_selection == SEL_ONLINE_LAN ? PC_XN_NET_LAN
                                                                              : PC_XN_NET_SERVICE);
            mn_80229894(MENU_KIND_ONLINE_XN, SEL_XN_HOST, 1);
            break;
        case SEL_ONLINE_SLIPPI:
            sfxForward();
            mn_80229894(MENU_KIND_ONLINE_SLIPPI, SEL_SLIPPI_UNRANKED, 1);
            break;
        default:
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_MAIN, SEL_MAIN_ONLINE, 3);
    } else {
        moveHover(buttons, (int) ARRAY_SIZE(online_labels));
    }
}

/// @brief Online Mode > Slippi think: Login, or the Slippi modes and Logout
void mnOnline_SlippiThink(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);
    bool signed_in = pc_net_match_slippi_ready();

    mn_804A04F0.buttons = buttons;
    if (mnOnline_SlippiCount() != slippi_built_count) {
        /* The account changed under the menu (a sign-out, or a sign-in that
         * finished): build it again with the rows that now apply. */
        mn_80229894(MENU_KIND_ONLINE_SLIPPI, 0, 1);
        return;
    }
    if (buttons & MenuInput_Confirm) {
        if (!signed_in) {
            enterOnline(mn_804A04F0.hovered_selection == 1 ? ONLINE_KIND_REGISTER : ONLINE_KIND_LOGIN);
            return;
        }
        switch (slippiRow(mn_804A04F0.hovered_selection)) {
        case row_unranked:
            enterAfterPlacing(ONLINE_KIND_UNRANKED);
            break;
        case row_direct:
            enterAfterPlacing(ONLINE_KIND_DIRECT);
            break;
        case row_place:
            enterOnline(ONLINE_KIND_PLACE);
            break;
        case row_logout:
            sfxForward();
            pc_slippi_account_forget();
            pc_net_match_slippi_refresh();
            break; /* the account check above rebuilds the menu next frame */
        default:
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_ONLINE, SEL_ONLINE_SLIPPI, 3);
    } else {
        moveHover(buttons, mnOnline_SlippiCount());
    }
}

/// @brief Online Mode > XeleeNet or LAN think: Host Lobby, Join, Name.
/// Each goes to the lobby scene (gmonlinemode.c), which asks for a name the
/// first time and shows the keyboard, the joining steps and the room.
void mnOnline_XnThink(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);

    mn_804A04F0.buttons = buttons;
    if (buttons & MenuInput_Confirm) {
        switch (mn_804A04F0.hovered_selection) {
        case SEL_XN_HOST:
            gmOnline_SetXnIntent(XN_INTENT_HOST);
            enterAfterPlacing(ONLINE_KIND_XN);
            break;
        case SEL_XN_JOIN:
            gmOnline_SetXnIntent(XN_INTENT_JOIN);
            enterAfterPlacing(ONLINE_KIND_XN);
            break;
        case SEL_XN_NAME:
            gmOnline_SetXnIntent(XN_INTENT_NAME);
            enterOnline(ONLINE_KIND_XN);
            break;
        default:
            break;
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_ONLINE, pc_xn_network() == PC_XN_NET_LAN ? SEL_ONLINE_LAN : SEL_ONLINE_XN, 3);
    } else {
        moveHover(buttons, (int) ARRAY_SIZE(xn_descriptions));
    }
}
