#include <pc/gekko_fp.h>
#include "ftCo_SpecialS.h"

#include <Runtime/platform.h>

#include <melee/it/kinds/itctfflag.h>

#include <dolphin/mtx.h>
#include <melee/ft/fighter.h>
#include <melee/ft/ft_081B.h>
#include <melee/ft/ftcommon.h>
#include <melee/ft/ftdata.h>
#include <melee/ft/types.h>

/* 0960CC */ static void doEnter(Fighter_GObj* gobj);

/// Check @c SpecialS input without entering the state.
bool ftCo_SpecialS_HasInput(Fighter* fp)
{
    if (fp->input.pressed_buttons & HSD_PAD_B &&
        ABS(fp->input.lstick[0].x) >= p_ftCommonData->x218)
    {
        return true;
    }
    return false;
}

bool ftCo_SpecialS_CheckInput(Fighter_GObj* gobj)
{
    Fighter* fp = gobj->user_data;
#ifdef TARGET_PC
    if (itCtfFlag_Is(fp->item_gobj)) {
        return false; /* no specials while carrying a CTF flag */
    }
#endif
    if (ftData_SpecialS[fp->kind] == NULL) {
        return false;
    }
    if (fp->x688 == 0) {
        if (fp->input.lstick[0].x * fp->facing_dir < -p_ftCommonData->x220) {
            ftCommon_UpdateFacing(fp);
        }
        doEnter(gobj);
        return true;
    }
    return false;
}

static void doEnter(Fighter_GObj* gobj)
{
    u8 _[8];
    Fighter* fp = gobj->user_data;
    fp->gr_vel = gk_fmadds(-(fp->gr_vel * (1 - fp->co_attrs.specials_ground_speed_retention)), ft_GetGroundFrictionMultiplier(fp), fp->gr_vel);
    ftData_SpecialS[fp->kind](gobj);
}
