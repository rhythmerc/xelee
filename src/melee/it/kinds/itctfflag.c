#include "itctfflag.h"

#include <Runtime/platform.h>

#include <melee/it/inlines.h>
#include <melee/it/it_26B1.h>
#include <melee/it/it_2725.h>
#include <melee/it/it_3F14.h>
#include <melee/it/itcoll.h>
#include <melee/it/itgroundcoll.h>
#include <melee/it/item.h>
#include <melee/it/itmaplib.h>
#include <melee/it/types.h>
#include <melee/mp/mpcoll.h>
#include <dolphin/os.h>

/* The states, by index:
 *   0 on the ground
 *   1 falling (spawned in the air)
 *   2 held
 *   3 thrown or dropped
 *   4 walked off a ledge
 * Every landing goes back to 0 through it_8026E414, which never runs the
 * landing count that can destroy a common item (it_8026DDFC). */
enum {
    FLAG_GROUND,
    FLAG_FALL,
    FLAG_HELD,
    FLAG_THROWN,
    FLAG_AIR,
};

static bool flag_no_anim(Item_GObj* gobj)
{
    return false;
}

static void flag_no_phys(Item_GObj* gobj) {}

static void flag_landed(Item_GObj* gobj)
{
    Item* ip = GET_ITEM(gobj);
    it_8026B390(gobj); /* xDC8 x15: on the ground, it can be picked up */
    itResetVelocity(ip);
    Item_80268E5C(gobj, FLAG_GROUND, ITEM_ANIM_UPDATE);
    OSReport("ctf: flag landed at %.1f %.1f\n", ip->pos.x, ip->pos.y);
}

static void flag_fall(Item_GObj* gobj)
{
    Item_80268E5C(gobj, FLAG_FALL, ITEM_ANIM_UPDATE);
}

static bool flag_ground_coll(Item_GObj* gobj)
{
    it_8026D62C(gobj, flag_fall);
    return false;
}

static void flag_fall_phys(Item_GObj* gobj)
{
    Item* ip = GET_ITEM(gobj);
    it_80272860(gobj, ip->xCC_item_attr->x10_fall_speed, ip->xCC_item_attr->x14_fall_speed_max);
}

static bool flag_air_coll(Item_GObj* gobj)
{
    it_8026E414(gobj, flag_landed);
    return false;
}

static void flag_thrown_phys(Item_GObj* gobj)
{
    Item_ApplyFallingPhysics(gobj);
}

static bool flag_ledge_coll(Item_GObj* gobj)
{
    it_8026E8C4(gobj, flag_landed, flag_fall);
    return false;
}

static ItemStateTable flag_states[] = {
    { -1, flag_no_anim, flag_no_phys, flag_ground_coll },
    { -1, flag_no_anim, flag_fall_phys, flag_air_coll },
    { -1, flag_no_anim, flag_no_phys, NULL },
    { -1, flag_no_anim, flag_thrown_phys, flag_air_coll },
    { -1, flag_no_anim, flag_no_phys, flag_ledge_coll },
};

static void flag_spawned(Item_GObj* gobj)
{
    Item* ip = GET_ITEM(gobj);
    Item_80268E5C(gobj, ip->ground_or_air == GA_Air ? FLAG_FALL : FLAG_GROUND, ITEM_ANIM_UPDATE);
}

static void flag_destroyed(Item_GObj* gobj)
{
    OSReport("ctf: flag destroyed\n");
}

static void flag_picked_up(Item_GObj* gobj)
{
    Item_80268E5C(gobj, FLAG_HELD, ITEM_ANIM_UPDATE);
    OSReport("ctf: flag picked up\n");
}

static void flag_dropped(Item_GObj* gobj)
{
    Item_80268E5C(gobj, FLAG_THROWN, 6);
    OSReport("ctf: flag dropped\n");
}

static void flag_thrown(Item_GObj* gobj)
{
    Item_80268E5C(gobj, FLAG_THROWN, 6);
    OSReport("ctf: flag thrown\n");
}

static bool flag_bounce(Item_GObj* gobj)
{
    it_8026B3A8(gobj);
    it_802725D4(gobj);
    itColl_BounceOffVictim(gobj);
    return false;
}

static void flag_entered_air(Item_GObj* gobj)
{
    Item* ip = GET_ITEM(gobj);
    Item_80268E5C(gobj, FLAG_AIR, ITEM_ANIM_UPDATE);
    ip->facing_dir = (ip->x40_vel.x + ip->x7C.x + ip->x88.x) >= 0.0f ? 1.0f : -1.0f;
    mpCollSetFacingDir(&ip->x378_itemColl, -1.0f == ip->facing_dir ? -1 : 1);
}

static bool flag_reflected(Item_GObj* gobj)
{
    it_8026B3A8(gobj);
    return it_80273030(gobj);
}

static bool flag_shield_bounced(Item_GObj* gobj)
{
    it_8026B3A8(gobj);
    return itColl_BounceOffShield(gobj);
}

static void flag_evt_unk(Item_GObj* gobj, Item_GObj* ref_gobj)
{
    it_8026B894(gobj, ref_gobj);
}

struct ItemLogicTable itCtfFlag_LogicTable = {
    flag_states,
    flag_spawned,
    flag_destroyed,
    flag_picked_up,
    flag_dropped,
    flag_thrown,
    flag_bounce, /* dmg_dealt */
    NULL,
    flag_entered_air,
    flag_reflected,
    flag_bounce, /* clanked */
    NULL,
    flag_shield_bounced,
    flag_bounce, /* hit_shield */
    flag_evt_unk,
};

Article* itCtfFlag_Article(void)
{
    return DP(Article, it_804D6D24[It_Kind_M_Ball].v);
}
