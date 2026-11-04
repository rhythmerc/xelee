#include "mnmodes.h"

#include "forward.h"
#include "inlines.h"
#include "mnmain.h"
#include "mnpage.h"
#include "types.h"
#include <melee/gm/gmscene.h>
#include <melee/gm/types.h>
#include <melee/lb/lbaudio_ax.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjplink.h>

#include "pc/lang.h"

/* The Game Modes menu's rows, in SEL_GAME_MODES_* order: a label and a
 * description each. */
static const char* const mode_labels[SEL_GAME_MODES_COUNT] = { "modes.ctf.label" };
static const char* const mode_descriptions[SEL_GAME_MODES_COUNT] = { "modes.ctf.desc" };

const char* mnModes_Label(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_VS && selection == SEL_VS_GAME_MODES) {
        return pc_tr("modes.label");
    }
    if (kind == MENU_KIND_GAME_MODES && selection >= 0 && selection < SEL_GAME_MODES_COUNT) {
        return pc_tr(mode_labels[selection]);
    }
    return NULL;
}

const char* mnModes_Description(MenuKind kind, int selection)
{
    if (kind == MENU_KIND_VS && selection == SEL_VS_GAME_MODES) {
        return pc_tr("modes.desc");
    }
    if (kind == MENU_KIND_GAME_MODES && selection >= 0 && selection < SEL_GAME_MODES_COUNT) {
        return pc_tr(mode_descriptions[selection]);
    }
    return NULL;
}

/// @brief VS Mode > Game Modes think: A opens the mode's rules page, B goes
/// back to VS Mode's row
void mnModes_Think(HSD_GObj* gp)
{
    u32 buttons = mn_80229624(4);

    mn_804A04F0.buttons = buttons;
    if (buttons & MenuInput_Confirm) {
        mn_804D6BC8.cooldown = 5;
        mn_804A04F0.entering_menu = 1;
        sfxForward();
        if (mnPage_OpenIn(MENU_KIND_GAME_MODES, mn_804A04F0.hovered_selection)) {
            HSD_GObjFree(gp);
        }
    } else if (buttons & MenuInput_Back) {
        sfxBack();
        mn_804A04F0.entering_menu = 0;
        mn_80229894(MENU_KIND_VS, SEL_VS_GAME_MODES, 3);
    } else if (SEL_GAME_MODES_COUNT > 1 && (buttons & (MenuInput_Up | MenuInput_Down))) {
        /* One mode so far: nothing to move to, so no move sound either. */
        sfxMove();
        mn_804A04F0.hovered_selection =
            (mn_804A04F0.hovered_selection + ((buttons & MenuInput_Up) ? SEL_GAME_MODES_COUNT - 1 : 1)) %
            SEL_GAME_MODES_COUNT;
    }
}

void mnModes_StartCtf(void)
{
    MenuExitData* data = gm_GetCurrentSceneExitData();
    sfxForward();
    data->pending_mode = GM_CTF;
    gm_801A4B60();
}
