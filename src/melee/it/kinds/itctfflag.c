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
#include <melee/cm/camera.h>
#include <melee/gm/gmctf.h>
#include <melee/ft/ftlib.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lbcollision.h>
#include <melee/mp/mpcoll.h>
#include <melee/mp/mplib.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <math.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/pobj.h>
#include <sysdolphin/baselib/state.h>
#include <sysdolphin/baselib/tev.h>

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
    ip->xDD4_itemVar.ctfflag.team = 0; /* the spawner sets the real team */
    ip->xDD4_itemVar.ctfflag.return_left = 0;
    ip->xDD4_itemVar.ctfflag.thrown = 0;
    Item_80268E5C(gobj, ip->ground_or_air == GA_Air ? FLAG_FALL : FLAG_GROUND, ITEM_ANIM_UPDATE);
}

static void flag_destroyed(Item_GObj* gobj)
{
    OSReport("ctf: flag destroyed\n");
    gmCtf_FlagDestroyed(gobj);
}

static void flag_picked_up(Item_GObj* gobj)
{
    Item_80268E5C(gobj, FLAG_HELD, ITEM_ANIM_UPDATE);
    OSReport("ctf: flag picked up\n");
}

static void flag_dropped(Item_GObj* gobj)
{
    Item_80268E5C(gobj, FLAG_THROWN, 6);
    GET_ITEM(gobj)->xDD4_itemVar.ctfflag.thrown = 0;
    OSReport("ctf: flag dropped\n");
}

static void flag_thrown(Item_GObj* gobj)
{
    Item* ip = GET_ITEM(gobj);
    Item_80268E5C(gobj, FLAG_THROWN, 6);
    ip->xDD4_itemVar.ctfflag.thrown = 1;
    /* Item_8026AD20 set the launch velocity just before this callback: a
     * shorter throw, the same arc time (CTF_FLAG_THROW_SPEED). */
    ip->x40_vel.x *= CTF_FLAG_THROW_SPEED;
    ip->x40_vel.z *= CTF_FLAG_THROW_SPEED;
    OSReport("ctf: flag thrown at %.1f %.1f vel %.3f %.3f\n", ip->pos.x, ip->pos.y, ip->x40_vel.x, ip->x40_vel.y);
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

bool itCtfFlag_Is(Item_GObj* gobj)
{
    return gobj != NULL && GET_ITEM(gobj)->kind == It_Kind_Ctf_Flag;
}

/* ---- the look: drawn here, not from the borrowed article's model ---- */

/* Team colours: the cloth, its shaded lower edge. */
static const GXColor flag_cloth[2] = { { 0xE8, 0x3A, 0x3A, 0xFF }, { 0x3F, 0x6F, 0xF0, 0xFF } };
static const GXColor flag_shade[2] = { { 0x9A, 0x1E, 0x22, 0xFF }, { 0x22, 0x3E, 0x9A, 0xFF } };
static const GXColor flag_pole = { 0xE8, 0xE2, 0xCF, 0xFF };
static const GXColor flag_pole_amb = { 0x60, 0x5C, 0x50, 0xFF };
static const GXColor flag_knob = { 0xF0, 0xC2, 0x26, 0xFF };
static const GXColor flag_knob_amb = { 0x70, 0x58, 0x10, 0xFF };

#define FLAG_POLE_LEN 18.0f
#define FLAG_POLE_R 0.7f
#define FLAG_CLOTH_W 7.5f
#define FLAG_CLOTH_H 5.0f
#define FLAG_CLOTH_SEGS 6
/* Blink for the last 3 s of an auto-return (gmctf.c counts it down). */
#define FLAG_BLINK_FRAMES 180


static void flag_draw_cloth(const Vec3* top, float trail, u8 team)
{
    Mtx view;
    int i;
    /* The wave runs on wall time: draws happen several times a frame (more
     * in the headset), and it is no game state. */
    const float phase = (float) (OSTicksToMilliseconds(OSGetTime()) % 100000) * 0.0096f;

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
    GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_NOOP);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GXSetZMode(GX_ENABLE, GX_LEQUAL, GX_ENABLE);
    GXSetCullMode(GX_CULL_NONE);
    HSD_CObjGetViewingMtx(HSD_CObjGetCurrent(), view);
    GXLoadPosMtxImm(view, GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);

    /* A strip from the pole out to the free edge: top row, bottom row. */
    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 2 * (FLAG_CLOTH_SEGS + 1));
    for (i = 0; i <= FLAG_CLOTH_SEGS; i++) {
        const float u = (float) i / FLAG_CLOTH_SEGS;
        const float x = top->x + trail * FLAG_CLOTH_W * u;
        const float wave = sinf(phase - u * 3.0f) * 0.9f * u;
        const float z = top->z + wave;
        const float sag = 0.8f * u;
        GXPosition3f32(x, top->y - sag + wave * 0.3f, z);
        GXColor4u8(flag_cloth[team].r, flag_cloth[team].g, flag_cloth[team].b, 0xFF);
        GXPosition3f32(x, top->y - FLAG_CLOTH_H - sag + wave * 0.3f, z);
        GXColor4u8(flag_shade[team].r, flag_shade[team].g, flag_shade[team].b, 0xFF);
    }
    GXEnd();
}

void itCtfFlag_Draw(HSD_GObj* gobj, intptr_t pass)
{
    Item* ip = GET_ITEM(gobj);
    const u8 team = ip->xDD4_itemVar.ctfflag.team & 1;
    const bool held = ip->msid == FLAG_HELD;
    Vec3 at, bottom, top, cloth_top;
    GXColor pole = flag_pole, pole_amb = flag_pole_amb, knob = flag_knob, knob_amb = flag_knob_amb;
    float trail;

    if (Camera_80031060() != 0 || !ip->xDAA_flag.b7) {
        return;
    }
    if (held && ip->owner != NULL && ftLib_IsFighter(ip->owner) && !ftLib_IsItemVisible(ip->owner, gobj)) {
        return;
    }
    {
        const u16 left = ip->xDD4_itemVar.ctfflag.return_left;
        if (left != 0 && left <= FLAG_BLINK_FRAMES && (left / 6) % 2 == 0) {
            return; /* the auto-return warning */
        }
    }

    if (held && ip->owner != NULL && ftLib_IsFighter(ip->owner)) {
        /* The held item's root stays put; Melee pins a bone of its model to
         * the hand (it_80274F48). The hand itself: the part pickup stored. */
        lb_8000B1CC(ftLib_GetPartJObj(ip->owner, ip->xDC4), NULL, &at);
        /* Gripped a third of the way up; the cloth trails behind. */
        bottom = at;
        bottom.y -= FLAG_POLE_LEN * 0.33f;
        trail = -ftLib_GetFacingDir(ip->owner);
    } else {
        /* The item's position is the middle of its collision box, about
         * 2.8 units up: stand the pole on the floor it landed on (xC30),
         * or in the air, at the box's bottom. */
        float dy;
        bottom = ip->pos;
        bottom.y -= ip->xC1C.bottom * ip->scl;
        if (ip->msid == FLAG_GROUND && ip->xC30 >= 0 &&
            mpLib_8004DD90_Floor(ip->xC30, &ip->pos, &dy, NULL, NULL) != -1)
        {
            bottom.y = ip->pos.y + dy;
        }
        trail = ip->facing_dir;
    }
    top = bottom;
    top.y += FLAG_POLE_LEN;

    lbColl_80008FC8(bottom, top, &pole, &pole_amb, FLAG_POLE_R);
    lbColl_80008FC8(top, top, &knob, &knob_amb, FLAG_POLE_R * 2.0f);
    cloth_top = top;
    cloth_top.y -= FLAG_POLE_R * 2.0f;
    flag_draw_cloth(&cloth_top, trail >= 0.0f ? 1.0f : -1.0f, team);

    HSD_StateInvalidate(-1);
    HSD_StateInitTev();
    HSD_ClearVtxDesc();
}

void itCtfFlag_DrawStand(const Vec3* at, u8 team, bool empty)
{
    GXColor plate = flag_cloth[team & 1], plate_amb = flag_shade[team & 1];
    Vec3 l = *at, r = *at, top = *at;

    /* A plate on the floor under the stand. */
    l.x -= 4.0f;
    r.x += 4.0f;
    l.y += 0.4f;
    r.y += 0.4f;
    lbColl_80008FC8(l, r, &plate, &plate_amb, 0.6f);
    if (empty) {
        /* The flag's ghost: where it goes back to. */
        GXColor ghost = flag_cloth[team & 1], ghost_amb = flag_shade[team & 1];
        ghost.a = 0x50;
        ghost_amb.a = 0x50;
        top.y += FLAG_POLE_LEN;
        lbColl_80008FC8(*at, top, &ghost, &ghost_amb, FLAG_POLE_R);
        lbColl_80008FC8(top, top, &ghost, &ghost_amb, FLAG_POLE_R * 2.0f);
    }
    HSD_StateInvalidate(-1);
    HSD_StateInitTev();
    HSD_ClearVtxDesc();
}

Article* itCtfFlag_Article(void)
{
    return DP(Article, it_804D6D24[It_Kind_M_Ball].v);
}
