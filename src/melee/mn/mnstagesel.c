#include <pc/gekko_fp.h>
#include "mnstagesel.h"

#include <placeholder.h>

#include "forward.h"
#include "inlines.h"
#include "mnmain.h"
#include <melee/gm/gm_unsplit.h>
#include <melee/gm/gmmain_lib.h>
#include <melee/lb/lb_00B0.h>
#include <melee/lb/lb_013B.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/lblanguage.h>
#include <melee/lb/types.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/fog.h>
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjgxlink.h>
#include <sysdolphin/baselib/gobjobject.h>
#include <sysdolphin/baselib/gobjplink.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/gobjuserdata.h>
#include <sysdolphin/baselib/jobj.h>
#include <sysdolphin/baselib/lobj.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/random.h>

#define MAX_ITER 100000
#define NUM_STAGES 29

static struct StageListInfo {
    /* +00 */ HSD_JObj* x0;
    /* +04 */ int x4;
    /* +08 */ u8 x8;
    /* +09 */ u8 x9;
    /* +0A */ u8 xA;
    /* +0B */ u8 stkind;
    /* +0C */ f32 xC;
    /* +10 */ f32 x10;
    /* +14 */ f32 x14;
    /* +18 */ f32 x18;
} mnStageSel_803F06D0[30] = {
    { 0, 0, 0x2, 0x00, 0x00, 0x04, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x01, 0x0C, 0x0B, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x02, 0x01, 0x05, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x03, 0x0D, 0x0C, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x04, 0x02, 0x0D, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x05, 0x0E, 0x0E, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x08, 0x04, 0x08, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x09, 0x10, 0x10, 2.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x0A, 0x05, 0x02, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x0B, 0x11, 0x11, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x0C, 0x06, 0x07, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x0D, 0x12, 0x16, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x06, 0x03, 0x06, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x07, 0x0F, 0x0F, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x12, 0x09, 0x09, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x13, 0x15, 0x12, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x10, 0x08, 0x0A, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x11, 0x14, 0x18, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x0E, 0x07, 0x03, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x0F, 0x13, 0x17, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x14, 0x0B, 0x13, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x15, 0x16, 0x14, 3.1F, 2.7F, 1.0F, 1.0F },
    { 0, 0, 0x2, 0x16, 0x0A, 0x19, 3.1F, 2.9F, 1.0F, 1.1F },
    { 0, 0, 0x2, 0x17, 0x17, 0x1B, 3.1F, 2.9F, 1.0F, 1.1F },
    { 0, 0, 0x2, 0x18, 0x18, 0x1F, 2.9F, 2.1F, 0.8F, 0.8F },
    { 0, 0, 0x2, 0x19, 0x19, 0x20, 2.9F, 2.1F, 0.8F, 0.8F },
    { 0, 0, 0x2, 0x1A, 0x1A, 0x1C, 2.9F, 2.1F, 0.8F, 0.8F },
    { 0, 0, 0x2, 0x1B, 0x1B, 0x1D, 2.9F, 2.1F, 0.8F, 0.8F },
    { 0, 0, 0x2, 0x1C, 0x1C, 0x1E, 2.9F, 2.1F, 0.8F, 0.8F },
    { 0, 0, 0x2, 0x1D, 0x00, 0x00, 3.6F, 2.7F, 1.2F, 1.0F },
};
ASSERT_SIZE(mnStageSel_803F06D0[0], 0x1C);

/* Read in place from MnSlMap.dat */
typedef struct DISC_STRUCT MnSelectStageModels {
    /* +00 */ StaticModelDesc icon_large;
    /* +10 */ StaticModelDesc icon_random;
    /* +20 */ StaticModelDesc icon_special;
    /* +30 */ StaticModelDesc stage_name;
    /* +40 */ StaticModelDesc icon_stacked;
    /* +50 */ StaticModelDesc menu_border;
    /* +60 */ StaticModelDesc stage_preview;
    /* +70 */ StaticModelDesc icon_hover;
    /* +80 */ StaticModelDesc cursor;
    /* +90 */ StaticModelDesc layout;
    /* +A0 */ StaticModelDesc background;
    /* +B0 */ StaticModelDesc now_loading;
} MnSelectStageModels;
DISC_ASSERT_SIZE(MnSelectStageModels, 0xC0);

typedef struct DISC_STRUCT MnSelectStageDataTable {
    /* 0x00 */ DISC_PTR(HSD_CObjDesc) cam;
    /* 0x04 */ DISC_PTR(HSD_LightDesc) light0;
    /* 0x08 */ DISC_PTR(HSD_LightDesc) light1;
    /* 0x0C */ DISC_PTR(HSD_FogDesc) fog;
    /* 0x10 */ MnSelectStageModels models;
} MnSelectStageDataTable;

static s8 mnStageSel_804D50A0 = -1;

static SSSData* sss_data;
static HSD_Archive* mnStageSel_804D6C94;
static MnSelectStageModels* sss_models;
static HSD_GObj* mnStageSel_804D6C9C;
static u32 mnStageSel_804D6CA0;
static u32 mnStageSel_804D6CA4;
static s32 mnStageSel_804D6CA8;
static s8 mnStageSel_804D6CAC;
static s8 mnStageSel_804D6CAD;
static u8 mnStageSel_804D6CAE;
static u8 mnStageSel_804D6CAF;

struct StageSelUserData {
    u16 x0;
    u16 x2;
    int x4;
};

#ifdef TARGET_PC
#include "pc/net.h"
#include "pc/xr_pointer.h"
#include "pc/net_lan.h"
#include "pc/net_match.h"
#include "pc/net_rank_session.h"
#include <sysdolphin/baselib/sislib.h>
#include "pc/pc.h"
#include "mnstagesel_slippi.h"
#include <melee/gm/gmonlinemode.h>
#include "pc/xeleenet_menu.h"
#include <sysdolphin/baselib/dobj.h>
#include <sysdolphin/baselib/mobj.h>
#include <sysdolphin/baselib/tobj.h>
#include <melee/lb/lbvector.h>
#include <aurora/aurora.h>
#include <sysdolphin/baselib/initialize.h>
#include <string.h>
#include <time.h>

/* A compact ranked list uses synchronized pads and makes both bans visible.
 * It does not depend on the archive's cursor geometry. */
static HSD_Text* rank_text;
static int rank_lines[7], rank_cursor, rank_axis;
static const unsigned rank_stages[] = {2, 3, 8, 28, 31, 32};
static const char* rank_names[] = {"FOUNTAIN OF DREAMS", "POKEMON STADIUM",
    "YOSHI'S STORY", "DREAM LAND", "BATTLEFIELD", "FINAL DESTINATION"};
static void rankStageDraw(void)
{
    int port = pc_rank_session_stage_port();
    HSD_SisLib_803A70A0(rank_text, rank_lines[0], "P%d %s", port + 1,
                       pc_rank_session_stage_prompt());
    for (int i = 0; i < 6; i++) {
        HSD_SisLib_803A70A0(rank_text, rank_lines[i + 1], "%s %s %s",
            i == rank_cursor ? ">" : " ", rank_names[i],
            pc_rank_session_stage_available(rank_stages[i]) ? "" : "(BANNED)");
    }
}
static void rankStageCreate(void)
{
    HSD_SisLib_803A62A0(0, lbLang_IsSavedLanguageUS() ? "SdMenu.usd" : "SdMenu.dat",
                       "SIS_MenuData");
    HSD_SisLib_803A611C(0, NULL, 9, 0xD, 0, 0xE, 0, 0x13);
    rank_text = HSD_SisLib_803A6754(0, 0);
    rank_text->default_kerning = 1;
    rank_cursor = rank_axis = 0;
    for (int i = 0; i < 7; i++) {
        rank_lines[i] = HSD_SisLib_803A6B98(rank_text, 50.0f, 100.0f + i * 40.0f, " ");
        HSD_SisLib_803A7548(rank_text, rank_lines[i], 0.55f, 0.55f);
    }
    rankStageDraw();
}
static void rankStageFrame(void)
{
    int port = pc_rank_session_stage_port();
    if (port < 0) return;
    HSD_PadStatus* pad = &HSD_PadCopyStatus[port];
    int axis = pad->stickY > 30 ? -1 : pad->stickY < -30 ? 1 : 0;
    int move = (pad->trigger & 8) ? -1 : (pad->trigger & 4) ? 1 :
               axis != rank_axis ? axis : 0;
    rank_axis = axis;
    if (move) rank_cursor = (rank_cursor + move + 6) % 6;
    if (pad->trigger & 0x200) {
        pc_rank_session_abort("ranked stage selection cancelled");
        gm_801A4B60();
        return;
    }
    if (pad->trigger & 0x1100) {
        if (!pc_rank_session_choose_stage(port, rank_stages[rank_cursor])) {
            lbAudioAx_80024030(3);
        } else if (pc_rank_session_stage()) {
            sss_data->vs.start.rules.stkind = pc_rank_session_stage();
            mnStageSel_804D6CAF = 2;
            gm_801A4B60();
            return;
        }
    }
    rankStageDraw();
}

/* Online: each player picks a stage on their own port (the SSS otherwise
 * merges every port into one cursor), picks are exchanged over the reliable
 * channel, and the stage is one of the two chosen by a coin flip from the
 * shared seed, so both peers agree without another message. Values are
 * table indices (mnStageSel_803F06D0), 30 = random. */
#define NET_MSG_STAGE_PICK 0x20
/* One pick per controller port, the sender's port riding in the message: a
 * XeleeNet lobby has up to four players (docs/xeleenet-plan.md). */
static int net_picks[4] = { -1, -1, -1, -1 };
static bool net_slippi_fixed; /* the ticket chose the stage; skip the pick exchange */

/* XeleeNet: a stage off the lobby's list (the host's stage_mask, in force on
 * every player as RULES; gm_80164330 reads it, as Random does) is dimmed and
 * can't be picked. */
static bool sss_off_list(int cell)
{
    return gmOnline_XnFlow() && cell >= 0 && cell < NUM_STAGES &&
           mnStageSel_803F06D0[cell].stkind != 0 &&
           !(u8) gm_80164330(mnStageSel_803F06D0[cell].xA);
}

/* An off-list icon drawn dim: its materials, over its joint and everything
 * under it (not its siblings: the next icon), get a last TEV stage scaling
 * their colour (HSD_MObjPcDim). Their own colour and alpha changed nothing:
 * the icons' textures carry their own TEV. */
static void sss_dim_icon(HSD_JObj* j, bool siblings)
{
    for (; j != NULL; j = siblings ? j->next : NULL) {
        if (!(j->flags & (JOBJ_INSTANCE | JOBJ_PTCL | JOBJ_SPLINE))) {
            HSD_DObj* d;
            for (d = j->u.dobj; d != NULL; d = d->next) {
                HSD_MObjPcDim(d->mobj);
            }
        }
        sss_dim_icon(j->child, true);
    }
}

/* Each frame in a XeleeNet lobby: dim the off-list icons, and tell the
 * overlay (xeleenet_menu.cpp) the lobby's list for its pill and legend. */
static void sss_mark_off_list(void)
{
    int i;
    if (!gmOnline_XnFlow()) {
        return;
    }
    for (i = 0; i < NUM_STAGES; i++) {
        if (sss_off_list(i) && mnStageSel_803F06D0[i].x0 != NULL) {
            sss_dim_icon(mnStageSel_803F06D0[i].x0, false);
        }
    }
    pc_xn_sss_frame(gmMainLib_GetGamePrefs()->stage_mask);
}

static bool netStageSel_Active(void)
{
    return pc_net_active();
}

static void netStageSel_Reset(void)
{
    for (int i = 0; i < 4; i++) {
        net_picks[i] = -1;
    }
    net_slippi_fixed = false;
}

/* Slippi Unranked: the server's ticket names the legal stages and the game
 * picks one at random, so there is nothing for the players to choose. Both
 * peers hash the shared seed with the number of games played so far, giving
 * the same cell without a message. Returns the table cell, or -1 when this
 * match lets the players pick. The table's stkind is the external stage id
 * the ticket uses. */
static int netStageSel_SlippiCell(void)
{
    static u32 games;
    uint16_t ids[16];
    int n = pc_net_match_slippi_stages(ids, 16);
    u32 r;
    int id;
    int i;
    if (n <= 0) {
        return -1;
    }
    r = (pc_net_seed() + games++ * 0x9E3779B9u) * 2654435761u;
    r ^= r >> 15;
    r *= 2246822519u;
    id = ids[(r >> 8) % (u32) n];
    for (i = 0; i < NUM_STAGES; i++) {
        if (mnStageSel_803F06D0[i].stkind == id) {
            pc_log_line("sss: Slippi ticket stage %d (cell %d of %d allowed)", id, i, n);
            return i;
        }
    }
    return -1;
}

static void netStageSel_Poll(void)
{
    u8 type;
    u8 buf[4];
    int n;
    while ((n = pc_net_recv_reliable(&type, buf, sizeof buf)) >= 0) {
        if (type == NET_MSG_STAGE_PICK && n >= 1) {
            /* [pick, port]; a one-byte pick is the other player of a 1v1 */
            int port = n >= 2 && buf[1] < 4 ? buf[1] : 1 - pc_net_local_player();
            if (port >= 0 && port < 4 && port != pc_net_local_player()) {
                net_picks[port] = buf[0];
                pc_log_line("sss: P%d picked %d", port + 1, buf[0]);
            }
        }
    }
}

static void netStageSel_SendPick(int idx)
{
    u8 b[2] = { (u8) idx, (u8) pc_net_local_player() };
    net_picks[pc_net_local_player() & 3] = idx;
    pc_net_send_reliable(NET_MSG_STAGE_PICK, b, 2);
    pc_log_line("sss: we picked %d", idx);
}

/* The ports in the session (ours too); a 1v1 is P1 and P2. */
static int netStageSel_Ports(void)
{
    int mask = pc_net_port_mask();
    return mask != 0 ? mask : 3;
}

/* True once every player in the session has picked. */
static bool netStageSel_AllPicked(void)
{
    int ports = netStageSel_Ports();
    for (int i = 0; i < 4; i++) {
        if ((ports & (1 << i)) && net_picks[i] < 0) {
            return false;
        }
    }
    return true;
}

/* Every player has picked: fold the picks in port order so the expression
 * is the same on every peer, then let the shared seed choose among them. */
static u32 netStageSel_Mix(void)
{
    int ports = netStageSel_Ports();
    u32 h = 0;
    if (!netStageSel_AllPicked()) {
        return pc_net_seed() * 2654435761u + (u32) mnStageSel_804D6CAE;
    }
    for (int i = 0; i < 4; i++) {
        if (ports & (1 << i)) {
            h = h * 31 + (u32) net_picks[i];
        }
    }
    return pc_net_seed() * 2654435761u + h;
}

static int netStageSel_Resolve(void)
{
    int ports = netStageSel_Ports();
    int list[4], n = 0;
    for (int i = 0; i < 4; i++) {
        if (ports & (1 << i)) {
            list[n++] = net_picks[i];
        }
    }
    int pick = list[(netStageSel_Mix() >> 16) % (u32) n];
    pc_log_line("sss: picks P1=%d P2=%d P3=%d P4=%d -> %d", net_picks[0], net_picks[1],
                net_picks[2], net_picks[3], pick);
    return pick;
}

/* Slippi Direct's Frozen Pokemon Stadium toggle (FrozenStadiumToggle/*.asm):
 * Z on any port flips it while the stage select is up for Direct's stage
 * pick. Like Slippi's static byte it stays as set for later picks. */
static bool sss_frozen_enabled;
static bool sss_frozen;
static bool sss_name_refresh; /* the toggle flipped: remake the hovered name */
#define SSS_ICON_STADIUM 18   /* Slippi's ID_GRPS */

void mnStageSel_SetFrozenToggle(bool enabled)
{
    sss_frozen_enabled = enabled;
}

bool mnStageSel_FrozenToggled(void)
{
    return sss_frozen;
}

static void frozenToggleFrame(void)
{
    int i;
    if (!sss_frozen_enabled) {
        return;
    }
    if (mnStageSel_804D6CAF != 0) {
        mnStageSel_SlippiBannerFrame(false);
        return;
    }
    for (i = 0; i < 4; i++) {
        if (HSD_PadCopyStatus[i].trigger & HSD_PAD_Z) {
            sss_frozen = !sss_frozen;
            sss_name_refresh = mnStageSel_804D6CAE == SSS_ICON_STADIUM;
            pc_log_line("sss: Frozen Stadium %s", sss_frozen ? "on" : "off");
            break;
        }
    }
    mnStageSel_SlippiBannerFrame(mnStageSel_804D6CAE == SSS_ICON_STADIUM);
}

/* "Random" online: the offline roll uses the live RNG and a per-machine
 * cooldown table, neither of which is in sync at this point (the peer's
 * pick lands on a different frame on each side). Draw from the seed
 * instead, over the stages the synced random-stage switches allow. */
static int netStageSel_Random(void)
{
    int allowed[NUM_STAGES];
    int n = 0;
    /* NUM_STAGES is 29 and the table holds 30: the last entry is the RANDOM
     * button, not a stage (StageListInfo table above, stkind 0). The bound
     * already excludes it; the stkind test says so out loud, because a
     * stkind of 0 starts a match with no stage that falls straight through
     * to the results screen, in sync, with nothing logged. */
    for (int i = 0; i < NUM_STAGES; i++) {
        if (mnStageSel_803F06D0[i].stkind != 0 &&
            (u8) gm_80164330(mnStageSel_803F06D0[i].xA)) {
            allowed[n++] = i;
        }
    }
    if (n == 0) {
        return 0;
    }
    u32 r = netStageSel_Mix() * 22695477u + 1u;
    return allowed[(r >> 8) % (u32) n];
}

/* XeleeNet (docs/xeleenet-ready-plan.md P5): the host decides the end. A
 * player's pick may change while others choose (a new STAGE_PICK); the host
 * sends FINAL once everyone has picked, or XN_PICK_TIMER frames after the
 * first pick, with every card already drawn (Random, and anyone still
 * choosing, from the lobby's list) and the winning port. Every player runs
 * the draw from FINAL's start frame, so the screens agree, and leaves through
 * the usual scene hand-off. The host's B is a message too (BACK): whichever
 * of the two the host sends is what everyone does. A guest's change that
 * misses FINAL just loses. */
#define NET_MSG_STAGE_FINAL 0x22 /* [pick[4], cell[4], timed_out, chosen, start BE32] */
#define NET_MSG_STAGE_TIMER 0x23 /* [deadline BE32]: the pick timer runs out then */
#define NET_MSG_STAGE_BACK 0x24  /* the host's B: everyone back to the CSS */
#define XN_PICK_TIMER 900        /* 15 s from the first pick */
#define XN_DRAW 120              /* the highlight across the cards */
#define XN_REST 60               /* then on the winner */
#define XN_LEAD 12               /* FINAL to the draw's first frame: the message's trip */
#define XN_RESEND 15             /* a changed pick goes out at most this often */
#define XN_THUMBS_AT 60          /* the icons' textures have settled by then */

static struct {
    bool final;     /* FINAL is in (or, on the host, out) */
    bool leaving;   /* the scene has asked to end */
    bool back_in;   /* the host's BACK came */
    bool back_want; /* the host pressed B; BACK still to go out */
    bool timer_out; /* the host's TIMER went */
    s8 pick[4];     /* FINAL's: each port's pick (30 Random, -1 not seated) */
    s8 cell[4];     /* and the stage its card shows */
    u8 timed_out;
    int chosen;
    s32 start, deadline, sent_at;
    int pending; /* our newest pick, not out yet */
    int lit;
    int frames;
    int over; /* the cell under the cursor, for the log */
} xn_sss;

static void xnSss_Reset(void)
{
    memset(&xn_sss, 0, sizeof xn_sss);
    xn_sss.chosen = -1;
    xn_sss.start = xn_sss.deadline = xn_sss.sent_at = -1;
    xn_sss.pending = -1;
    xn_sss.lit = -1;
    xn_sss.over = -1;
}

/* A stage on the lobby's list. */
static bool xnSss_Listed(int cell)
{
    return cell >= 0 && cell < NUM_STAGES && mnStageSel_803F06D0[cell].stkind != 0 &&
           (u8) gm_80164330(mnStageSel_803F06D0[cell].xA);
}

/* The host's draws come from its own generator: FINAL carries them, so
 * nothing about them has to agree, and the synced RNG stays untouched. */
static u32 xnSss_Rand(void)
{
    static u32 x;
    if (x == 0) {
        x = ((u32) time(NULL) * 2654435761u) ^ pc_net_seed() ^ (u32) pc_net_frame();
        x = x != 0 ? x : 1;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

static int xnSss_RandomCell(void)
{
    int allowed[NUM_STAGES];
    int i, n = 0;
    for (i = 0; i < NUM_STAGES; i++) {
        if (xnSss_Listed(i)) {
            allowed[n++] = i;
        }
    }
    return n > 0 ? allowed[xnSss_Rand() % (u32) n] : 0;
}

/* A on a stage (or Random): ours until FINAL. */
static void xnSss_Pick(int cell)
{
    int me = pc_net_local_player() & 3;
    if (xn_sss.final || xn_sss.leaving) {
        return;
    }
    sfxForward();
    if (net_picks[me] == cell) {
        return;
    }
    net_picks[me] = cell;
    xn_sss.pending = cell;
    pc_log_line("sss: we picked %d", cell);
}

static bool xnSss_Same(void)
{
    int ports = netStageSel_Ports();
    int i, first = -1;
    for (i = 0; i < 4; i++) {
        if (ports & (1 << i)) {
            if (first >= 0 && xn_sss.cell[i] != first) {
                return false;
            }
            first = xn_sss.cell[i];
        }
    }
    return true;
}

static void xnSss_LogFinal(const char* who)
{
    pc_log_line("sss: %s: picks P1=%d P2=%d P3=%d P4=%d, cards %d %d %d %d, timed out %x, "
                "P%d's card, the draw at frame %d%s",
                who, xn_sss.pick[0], xn_sss.pick[1], xn_sss.pick[2], xn_sss.pick[3],
                xn_sss.cell[0], xn_sss.cell[1], xn_sss.cell[2], xn_sss.cell[3],
                xn_sss.timed_out, xn_sss.chosen + 1, xn_sss.start,
                xnSss_Same() ? " (one stage: no draw)" : "");
}

/* The host: everyone has picked, or the timer has run out. */
static void xnSss_SendFinal(void)
{
    int ports = netStageSel_Ports();
    int cards[4], n = 0, i;
    u8 m[14];
    s32 start = pc_net_frame() + XN_LEAD;
    u8 timed_out = 0;
    s8 pick[4], cell[4];
    for (i = 0; i < 4; i++) {
        pick[i] = cell[i] = -1;
        if (!(ports & (1 << i))) {
            continue;
        }
        cards[n++] = i;
        if (net_picks[i] < 0) {
            timed_out |= 1 << i;
        }
        pick[i] = xnSss_Listed(net_picks[i]) ? net_picks[i] : 30;
        cell[i] = pick[i] < NUM_STAGES ? pick[i] : xnSss_RandomCell();
    }
    if (n == 0) {
        return;
    }
    for (i = 0; i < 4; i++) {
        m[i] = (u8) pick[i];
        m[4 + i] = (u8) cell[i];
    }
    m[8] = timed_out;
    m[9] = (u8) cards[xnSss_Rand() % (u32) n];
    m[10] = (u8) (start >> 24);
    m[11] = (u8) (start >> 16);
    m[12] = (u8) (start >> 8);
    m[13] = (u8) start;
    if (!pc_net_send_reliable(NET_MSG_STAGE_FINAL, m, sizeof m)) {
        return; /* the lane is full: again next frame */
    }
    memcpy(xn_sss.pick, pick, 4);
    memcpy(xn_sss.cell, cell, 4);
    xn_sss.timed_out = timed_out;
    xn_sss.chosen = m[9];
    xn_sss.start = start;
    xn_sss.final = true;
    xnSss_LogFinal("final");
}

static void xnSss_Poll(void)
{
    u8 type;
    u8 buf[16];
    int n, i;
    while ((n = pc_net_recv_reliable(&type, buf, sizeof buf)) >= 0) {
        if (type == NET_MSG_STAGE_PICK && n >= 2 && buf[1] < 4 &&
            buf[1] != pc_net_local_player() && !xn_sss.final)
        {
            net_picks[buf[1]] = buf[0];
            pc_log_line("sss: P%d picked %d", buf[1] + 1, buf[0]);
        } else if (type == NET_MSG_STAGE_TIMER && n >= 4 && pc_net_local_player() != 0) {
            xn_sss.deadline = (s32) ((u32) buf[0] << 24 | (u32) buf[1] << 16 | (u32) buf[2] << 8 | buf[3]);
        } else if (type == NET_MSG_STAGE_FINAL && n >= 14 && pc_net_local_player() != 0 &&
                   !xn_sss.final && buf[9] < 4)
        {
            int ports = netStageSel_Ports();
            bool ok = (ports & (1 << buf[9])) != 0;
            for (i = 0; i < 4; i++) {
                xn_sss.pick[i] = (s8) buf[i];
                xn_sss.cell[i] = (s8) buf[4 + i];
                if ((ports & (1 << i)) && (xn_sss.cell[i] < 0 || xn_sss.cell[i] >= NUM_STAGES)) {
                    ok = false;
                }
            }
            if (!ok) {
                pc_log_line("sss: a FINAL naming no stage or player ignored");
                continue;
            }
            xn_sss.timed_out = buf[8];
            xn_sss.chosen = buf[9];
            xn_sss.start = (s32) ((u32) buf[10] << 24 | (u32) buf[11] << 16 | (u32) buf[12] << 8 | buf[13]);
            xn_sss.final = true;
            xn_sss.pending = -1;
            for (i = 0; i < 4; i++) {
                net_picks[i] = xn_sss.pick[i];
            }
            xnSss_LogFinal("final from the host");
        } else if (type == NET_MSG_STAGE_BACK && pc_net_local_player() != 0 && !xn_sss.final) {
            xn_sss.back_in = true;
        }
    }
}

/* The host's B, or its BACK arriving: true once, on the frame to leave. */
static bool xnSss_Back(void)
{
    if (xn_sss.leaving || xn_sss.final) {
        return false;
    }
    if (pc_net_local_player() == 0 && (HSD_PadCopyStatus[0].trigger & HSD_PAD_B)) {
        xn_sss.back_want = true;
    }
    if (xn_sss.back_want && pc_net_send_reliable(NET_MSG_STAGE_BACK, NULL, 0)) {
        xn_sss.back_want = false;
        xn_sss.back_in = true;
    }
    if (xn_sss.back_in) {
        xn_sss.leaving = true;
        pc_log_line("sss: the host's B, everyone back to the CSS");
        return true;
    }
    return false;
}

/* Which card the highlight is on, t frames into the draw: it runs round the
 * cards a few times, slowing (cubic ease-out), and stops on the winner. */
static int xnSss_Lit(int t, int n, int win)
{
    float x, e;
    int steps = 3 * n + win;
    if (t >= XN_DRAW) {
        return win;
    }
    x = t <= 0 ? 0.0F : (float) t / XN_DRAW;
    e = 1.0F - (1.0F - x) * (1.0F - x) * (1.0F - x);
    return (int) (steps * e) % n;
}

/* XeleeNet's stage select draws smaller about the screen's centre
 * (PC_XN_SSS_ZOOM), leaving the picks card room under the grid, as the CSS
 * does (mncharsel.c cssXnCameraZoom). The menu camera's proc re-inits the
 * camera from its desc every frame, so the zoom goes on after it. */
static void sssXnCameraProc(HSD_GObj* gobj)
{
    HSD_CObj* cobj;
    mn_8022BA1C(gobj);
    cobj = GET_COBJ(gobj);
    if (HSD_CObjGetProjectionType(cobj) == PROJ_PERSPECTIVE) {
        float half = cobj->projection_param.perspective.fov * (float) M_PI / 360.0F;
        cobj->projection_param.perspective.fov =
            atanf(tanf(half) / PC_XN_SSS_ZOOM) * 360.0F / (float) M_PI;
    } else if (HSD_CObjGetProjectionType(cobj) == PROJ_ORTHO) {
        float cx = (cobj->projection_param.ortho.left + cobj->projection_param.ortho.right) / 2;
        float cy = (cobj->projection_param.ortho.top + cobj->projection_param.ortho.bottom) / 2;
        cobj->projection_param.ortho.left = cx + (cobj->projection_param.ortho.left - cx) / PC_XN_SSS_ZOOM;
        cobj->projection_param.ortho.right = cx + (cobj->projection_param.ortho.right - cx) / PC_XN_SSS_ZOOM;
        cobj->projection_param.ortho.top = cy + (cobj->projection_param.ortho.top - cy) / PC_XN_SSS_ZOOM;
        cobj->projection_param.ortho.bottom = cy + (cobj->projection_param.ortho.bottom - cy) / PC_XN_SSS_ZOOM;
    }
}

/* An icon's frame on screen, as fractions of it. The table's xC/x10 are the
 * cursor's hit box, which also covers the gaps; the frame is the size
 * Melee's own hover highlight is scaled to (x14/x18, fn_8025A560) times the
 * highlight's size at scale 1, measured on screen. */
#define XN_FRAME_HW 3.0F
#define XN_FRAME_HH 2.7F
static bool xnSss_IconOnScreen(int cell, float out[4])
{
    Vec3 pos, a, b, sa, sb;
    HSD_CObj* cobj;
    if (cell < 0 || cell >= NUM_STAGES || mnStageSel_803F06D0[cell].x0 == NULL ||
        mnStageSel_804D6C9C == NULL)
    {
        return false;
    }
    cobj = GET_COBJ(mnStageSel_804D6C9C);
    lb_8000B1CC(mnStageSel_803F06D0[cell].x0, NULL, &pos);
    a = b = pos;
    a.x -= XN_FRAME_HW * mnStageSel_803F06D0[cell].x14;
    a.y += XN_FRAME_HH * mnStageSel_803F06D0[cell].x18;
    b.x += XN_FRAME_HW * mnStageSel_803F06D0[cell].x14;
    b.y -= XN_FRAME_HH * mnStageSel_803F06D0[cell].x18;
    if (lbVector_WorldToScreen(cobj, &a, &sa, 1) == NULL ||
        lbVector_WorldToScreen(cobj, &b, &sb, 1) == NULL)
    {
        return false;
    }
    out[0] = sa.x / 640.0F;
    out[1] = sa.y / 480.0F;
    out[2] = (sb.x - sa.x) / 640.0F;
    out[3] = (sb.y - sa.y) / 480.0F;
    return true;
}

/* Each icon's picture for the cards, once the icons' texture animations
 * have set them: the C8 image and the palette in use. */
static void xnSss_Thumbs(void)
{
    int i;
    for (i = 0; i < NUM_STAGES; i++) {
        HSD_JObj* j = mnStageSel_803F06D0[i].x0;
        HSD_DObj* d;
        HSD_TObj* t;
        if (j == NULL || pc_xn_sss_has_thumb(i) || (j->flags & (JOBJ_INSTANCE | JOBJ_PTCL | JOBJ_SPLINE))) {
            continue;
        }
        /* The picture is on the icon's joint or its first child's. */
        if (j->u.dobj == NULL && j->child != NULL) {
            j = j->child;
        }
        for (d = j->u.dobj; d != NULL; d = d->next) {
            for (t = d->mobj != NULL ? d->mobj->tobj : NULL; t != NULL; t = t->next) {
                HSD_ImageDesc* im = t->imagedesc;
                HSD_Tlut* tl = t->tlut_no != (u8) -1 && t->tluttbl != NULL ? t->tluttbl[t->tlut_no] : t->tlut;
                if (im != NULL && im->format == GX_TF_C8 && tl != NULL && tl->lut != NULL) {
                    pc_xn_sss_thumb(i, DP(void, im->image_ptr), im->width, im->height, tl->lut,
                                    (int) tl->fmt, tl->n_entries);
                }
            }
        }
    }
}

/* The overlay (the rings, the picks card, the draw: RmlUi,
 * xeleenet_menu.cpp) is drawn into the stage select's own frame here, after
 * the icons and Melee's hover frame (link 4, 0x85, made after them) and
 * before the cursor (0x86), so the cursor stays on top of it. */
static void xnSss_OverlayRender(HSD_GObj* gobj, int pass)
{
    if (HSD_GetCurrentRenderPass() == HSD_RP_SCREEN) {
        aurora_rmlui_draw_here();
    }
}

static void xnSss_OverlayCreate(void)
{
    HSD_GObj* gobj = GObj_Create(HSD_GOBJ_CLASS_FIGHTER, 5, 0x80);
    GObj_SetupGXLink(gobj, (GObj_RenderFunc) xnSss_OverlayRender, 4, 0x85);
}

/* Once a frame in a XeleeNet lobby: the messages, our pick, the host's timer
 * and FINAL, the draw, and the overlay's state. */
static void xnSss_Frame(void)
{
    PcXnSssState st;
    s32 f = pc_net_frame();
    int me = pc_net_local_player() & 3;
    int ports = netStageSel_Ports();
    int cards[4], n = 0, win = 0, i;
    bool any = false, all = true;

    if (++xn_sss.frames == XN_THUMBS_AT) {
        xnSss_Thumbs();
    }
    if (mnStageSel_804D6CAE != xn_sss.over && mnStageSel_804D6CAE < NUM_STAGES && !xn_sss.final) {
        xn_sss.over = mnStageSel_804D6CAE;
        pc_log_line("sss: over cell %d", xn_sss.over); /* tools/net_test_xn_menu.py steers by it */
    }
    xnSss_Poll();
    if (pc_net_peer_status() != 0 && !xn_sss.leaving) {
        xn_sss.leaving = true;
        gm_801A4B60();
    }
    if (xn_sss.pending >= 0 && !xn_sss.final &&
        (xn_sss.sent_at < 0 || f - xn_sss.sent_at >= XN_RESEND))
    {
        u8 b[2] = { (u8) xn_sss.pending, (u8) me };
        if (pc_net_send_reliable(NET_MSG_STAGE_PICK, b, 2)) {
            xn_sss.sent_at = f;
            xn_sss.pending = -1;
        }
    }
    for (i = 0; i < 4; i++) {
        if (ports & (1 << i)) {
            any |= net_picks[i] >= 0;
            all &= net_picks[i] >= 0;
        }
    }
    if (me == 0 && !xn_sss.final && !xn_sss.leaving && !xn_sss.back_want) {
        if (any && xn_sss.deadline < 0) {
            xn_sss.deadline = f + XN_PICK_TIMER;
        }
        if (xn_sss.deadline >= 0 && !xn_sss.timer_out) {
            u8 m[4] = { (u8) (xn_sss.deadline >> 24), (u8) (xn_sss.deadline >> 16),
                        (u8) (xn_sss.deadline >> 8), (u8) xn_sss.deadline };
            xn_sss.timer_out = pc_net_send_reliable(NET_MSG_STAGE_TIMER, m, sizeof m);
        }
        if (all || (xn_sss.deadline >= 0 && f >= xn_sss.deadline)) {
            xnSss_SendFinal();
        }
    }

    memset(&st, 0, sizeof st);
    st.ports = (u8) ports;
    st.chosen = -1;
    st.lit = -1;
    st.t = -1;
    st.draw = XN_DRAW;
    st.rest = XN_REST;
    st.timer = xn_sss.deadline >= 0 && !xn_sss.final ? (xn_sss.deadline > f ? xn_sss.deadline - f : 0) : -1;
    st.timer_total = XN_PICK_TIMER;
    for (i = 0; i < 4; i++) {
        int p = (ports & (1 << i)) ? net_picks[i] : -1;
        st.pick[i] = (s8) p;
        st.pick_id[i] = p >= 0 && p < NUM_STAGES ? (s8) mnStageSel_803F06D0[p].xA : -1;
        st.outline_ok[i] = p >= 0 && p < NUM_STAGES && !xn_sss.final && xnSss_IconOnScreen(p, st.outline[i]);
        /* The upper grid's frames are square at the bottom right; Icicle
         * Mountain, Flat Zone and the bottom row cut all four corners. */
        st.outline_bevels[i] = p < 22 ? 0xB : 0xF;
    }
    if (xn_sss.final) {
        bool same = xnSss_Same();
        int t = f - xn_sss.start;
        for (i = 0; i < 4; i++) {
            if (ports & (1 << i)) {
                if (i == xn_sss.chosen) {
                    win = n;
                }
                cards[n++] = i;
            }
            st.cell[i] = xn_sss.cell[i];
            st.cell_id[i] = xn_sss.cell[i] >= 0 && xn_sss.cell[i] < NUM_STAGES
                                ? (s8) mnStageSel_803F06D0[xn_sss.cell[i]].xA
                                : -1;
            if (xn_sss.pick[i] >= NUM_STAGES) {
                st.random |= 1 << i;
            }
        }
        st.final = true;
        st.timed_out = xn_sss.timed_out;
        st.chosen = xn_sss.chosen;
        /* Every card the same stage: no draw, straight to the result. */
        st.draw = same ? 0 : XN_DRAW;
        st.t = t;
        if (n > 0 && t >= 0) {
            int lit = same ? cards[win] : cards[xnSss_Lit(t, n, win)];
            if (lit != xn_sss.lit) {
                if (t >= st.draw) {
                    sfxForward();
                } else {
                    sfxMove();
                }
                xn_sss.lit = lit;
            }
            st.lit = lit;
        }
        if (t >= st.draw + XN_REST && !xn_sss.leaving && xn_sss.chosen >= 0) {
            mnStageSel_804D6CAE = (u8) xn_sss.cell[xn_sss.chosen];
            mnStageSel_804D6CAF = 2;
            xn_sss.leaving = true;
        }
    }
    pc_xn_sss_picks(&st);
}
#endif

/// @todo .sdata2 order hack
#ifdef MUST_MATCH
static void order_sdata2(void)
{
    (void) S32_TO_F32;
}
#endif

/// Random stage selection
/// Returns an internal stage ID - 2 (since first 2 internal stage IDs are
/// invalid)
int mnStageSel_802599EC(void)
{
    int var_r0;
    int iter;
    bool var_r29 = true;
    int i;

    for (i = 0; i < NUM_STAGES; i++) {
        if (mnStageSel_803F06D0[i].x4 >= 0 &&
            (u8) gm_80164330(mnStageSel_803F06D0[i].xA))
        {
            break;
        }
    }
    if (i == NUM_STAGES) {
        for (i = 0; i < NUM_STAGES; i++) {
            mnStageSel_803F06D0[i].x4 = 0;
        }
    }
    while (var_r29) {
        for (i = 0; i < NUM_STAGES; i++) {
            if (mnStageSel_803F06D0[i].x4 > 0) {
                mnStageSel_803F06D0[i].x4--;
            }
        }
        for (i = 0; i < NUM_STAGES; i++) {
            if (mnStageSel_803F06D0[i].x4 == 0 &&
                (u8) gm_80164330(mnStageSel_803F06D0[i].xA))
            {
                var_r29 = false;
            }
        }
    }
    for (iter = 0; iter < MAX_ITER; iter++) {
        int tmp = HSD_Randi(NUM_STAGES);
        i = tmp;
        if (mnStageSel_803F06D0[i].x4 == 0) {
            if ((u8) gm_80164330(mnStageSel_803F06D0[i].xA)) {
                break;
            }
        }
    }
    if (iter >= MAX_ITER) {
        i = 0;
    }
    mnStageSel_803F06D0[i].x4 = -1;
    if (i < 22) {
        if (i & 1) {
            var_r0 = i - 1;
        } else {
            var_r0 = i + 1;
        }
        if (mnStageSel_803F06D0[var_r0].x4 >= 0) {
            mnStageSel_803F06D0[var_r0].x4 = 3;
        }
    }
    return i;
}

void mnStageSel_80259C28(void)
{
    HSD_JObj* jobj;
    HSD_GObj* gobj;
    u64 _[2];

#ifdef TARGET_PC
    if (rank_text) return;
#endif
    if (mnStageSel_804D6CA4 != 0) {
        return;
    }
    switch (mnStageSel_804D6CAE) {
    case 30:
        if (!(mnStageSel_804D6CA0 & 0x1000)) {
            return;
        }
        break;
    case 29:
        if (!(mnStageSel_804D6CA0 & 0x1100)) {
            return;
        }
        break;
    default:
        if (!(mnStageSel_804D6CA0 & 0x1100)) {
            return;
        }
        if (mnStageSel_804D6CAE < 0x1E &&
            mnStageSel_803F06D0[mnStageSel_804D6CAE].x8 >= 2
#ifdef TARGET_PC
            && !sss_off_list(mnStageSel_804D6CAE)
#endif
        )
        {
#ifdef TARGET_PC
            if (gmOnline_XnFlow() && netStageSel_Active()) {
                xnSss_Pick(mnStageSel_804D6CAE); /* the cursor stays: it may change */
                return;
            }
            if (netStageSel_Active() && !pc_rank_session_active()) {
                netStageSel_SendPick(mnStageSel_804D6CAE);
            }
#endif
            goto skip_randomize;
        }
#ifdef TARGET_PC
        if (sss_off_list(mnStageSel_804D6CAE)) {
            pc_log_line("sss: cell %d is off the lobby's list", mnStageSel_804D6CAE);
        }
#endif
        lbAudioAx_80024030(3);
        return;
    }
#ifdef TARGET_PC
    if (gmOnline_XnFlow() && netStageSel_Active()) {
        xnSss_Pick(30); /* Random: the host draws it from the list */
        return;
    }
    if (netStageSel_Active() && !pc_rank_session_active()) {
        /* Send the cell (30 = random) and defer the roll to the resolve so
         * both peers roll from the same RNG state. */
        netStageSel_SendPick(mnStageSel_804D6CAE);
        goto skip_randomize;
    }
#endif
    mnStageSel_804D6CAE = mnStageSel_802599EC();
skip_randomize:

    gobj = GObj_Create(HSD_GOBJ_CLASS_FIGHTER, 5, 0x80);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, sss_models->now_loading.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x87);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 0);
    HSD_JObjAddAnimAll(jobj, DP(HSD_AnimJoint, sss_models->now_loading.animjoint),
                       DP(HSD_MatAnimJoint,
                          sss_models->now_loading.matanim_joint), DP(HSD_ShapeAnimJoint,
                          sss_models->now_loading.shapeanim_joint));
    HSD_JObjReqAnimAll(gobj->hsd_obj, 0.0F);
    HSD_JObjAnimAll(gobj->hsd_obj);
    mnStageSel_804D6CAF = 1;
    mnStageSel_804D6CA4 = 0x1E;
    sfxForward();
}

void fn_80259D84(HSD_GObj* gobj)
{
    struct StageSelUserData* temp_r31 = HSD_GObjGetUserData(gobj);
    HSD_JObj* jobj = GET_JOBJ(gobj);

    switch (temp_r31->x2) {
    case 0:
        if (mnStageSel_804D6CAF == 0) {
            mnStageSel_80259C28();
        }
        if (++temp_r31->x4 >= 9U) {
            HSD_ForeachAnim(jobj, JOBJ_TYPE, ALL_TYPE_MASK, HSD_AObjStopAnim,
                            AOBJ_ARG_AOV, NULL);
            temp_r31->x2 = 1;
        }
        break;
    case 1:
        if (mnStageSel_804D6CAF == 0) {
            mnStageSel_80259C28();
        }
#ifdef TARGET_PC
        /* The Frozen toggle flipped: out with this name, in with the other
         * caption (Slippi does the same with a fake icon id). */
        if (temp_r31->x0 != mnStageSel_804D6CAE ||
            (sss_name_refresh && mnStageSel_804D6CAE == SSS_ICON_STADIUM))
        {
            sss_name_refresh = false;
#else
        if (temp_r31->x0 != mnStageSel_804D6CAE) {
#endif
            if (temp_r31->x0 < 0x1E &&
                mnStageSel_803F06D0[temp_r31->x0].x8 >= 2)
            {
                HSD_JObjReqAnimAllByFlags(jobj, 1, 10.0F);
            }
            sfxMove();
            temp_r31->x4 = 0;
            temp_r31->x2 = 2;
            mnStageSel_80259ED8(mnStageSel_804D6CAE);
        }
        break;
    case 2:
        if (++temp_r31->x4 > 0xAU) {
            HSD_GObjFree(gobj);
            temp_r31->x2++;
        }
        break;
    }
}

static void do_anim(HSD_JObj* jobj, int frame)
{
    HSD_JObjReqAnimAll(jobj, 0.0F);
    HSD_JObjReqAnimAllByFlags(jobj, 0x10, frame);
    HSD_JObjAnimAll(jobj);
    HSD_ForeachAnim(jobj, JOBJ_TYPE, TOBJ_MASK, HSD_AObjStopAnim, AOBJ_ARG_AOV,
                    NULL);
}

void mnStageSel_80259ED8(int id)
{
    HSD_GObj* gobj;
    HSD_JObj* jobj;
    struct StageSelUserData* temp_r3_2;
    u8 _[4];

    gobj = GObj_Create(HSD_GOBJ_CLASS_FIGHTER, 5, 0x80);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, sss_models->stage_name.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x84);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 1);
    HSD_JObjAddAnimAll(jobj,
                       DP(HSD_AnimJoint, sss_models->stage_name.animjoint),
                       DP(HSD_MatAnimJoint,
                          sss_models->stage_name.matanim_joint),
                       DP(HSD_ShapeAnimJoint,
                          sss_models->stage_name.shapeanim_joint));
    jobj = GET_JOBJ(gobj);
    temp_r3_2 = HSD_MemAlloc(sizeof(struct StageSelUserData));
    GObj_InitUserData(gobj, 4, HSD_Free, temp_r3_2);
    HSD_GObj_SetupProc(gobj, fn_80259D84, 1);
    temp_r3_2->x0 = id;
    temp_r3_2->x4 = 0;
    temp_r3_2->x2 = 0;
    if (id < 0x1E && mnStageSel_803F06D0[id].x8 >= 2) {
        do_anim(jobj, 20.0F * mnStageSel_803F06D0[id].x9);
    }
#ifdef TARGET_PC
    if (id == SSS_ICON_STADIUM && sss_frozen_enabled && sss_frozen) {
        mnStageSel_SlippiFrozenName(jobj);
    }
#endif
}

void fn_8025A090(HSD_GObj* gobj)
{
    u32 var_r3;
    struct {
        u32 x0;
        u32 x4;
    }* temp_r30;
    HSD_JObj* jobj;

    jobj = GET_JOBJ(gobj);
    temp_r30 = HSD_GObjGetUserData(gobj);
    var_r3 = mnStageSel_804D6CAE;
    /* 0x1E is the "random" sentinel (see :485), one past the end of the
     * 30-entry table. */
    if (mnStageSel_804D6CAE >= 0x1E ||
        mnStageSel_803F06D0[mnStageSel_804D6CAE].x8 < 2)
    {
        var_r3 = 0x1E;
    }
    if (temp_r30->x0 != var_r3) {
        temp_r30->x0 = var_r3;
        temp_r30->x4 = 0;
        if ((s32) var_r3 < 0x1D) {
            HSD_JObjReqAnimAll(jobj, 50.0F * mnStageSel_803F06D0[var_r3].x9);
            HSD_JObjAnimAll(jobj);
            HSD_ForeachAnim(jobj, JOBJ_TYPE, ALL_TYPE_MASK, HSD_AObjStopAnim,
                            AOBJ_ARG_AOV, NULL);
            HSD_JObjSetTranslateX(jobj, 0.0F);
        } else {
            HSD_JObjSetTranslateX(jobj, 100.0F);
        }
    }
    if (temp_r30->x4 < 0x5A) {
        temp_r30->x4++;
        if (temp_r30->x4 == 0x14 && temp_r30->x0 < 0x1E) {
            HSD_JObjReqAnimAll(jobj,
                               50.0F * mnStageSel_803F06D0[temp_r30->x0].x9);
        }
        if (temp_r30->x4 == 0x45) {
            HSD_ForeachAnim(jobj, JOBJ_TYPE, ALL_TYPE_MASK, HSD_AObjStopAnim,
                            AOBJ_ARG_AOV, NULL);
        }
    } else if (mnStageSel_804D6CAF) {
        mnStageSel_804D6CAF = 2;
    }
}

void fn_8025A310(HSD_GObj* gobj)
{
    Vec3 sp1C;
    Vec3 sp10;
    f32 temp_f1;
    f32 temp_f2;
    int i;
    HSD_JObj* jobj;
    u32 unused;

    jobj = gobj->hsd_obj;
    if (mnStageSel_804D6CAF != 0
#ifdef TARGET_PC
        || (gmOnline_XnFlow() && xn_sss.final) /* the draw's panel is up */
#endif
    )
    {
        HSD_JObjSetFlags(jobj, JOBJ_HIDDEN);
        return;
    }
    HSD_JObjGetTranslation(jobj, &sp1C);
    sp1C.x = gk_fmadds(0.03f, mnStageSel_804D6CAC, sp1C.x);
    if (-27.0F > sp1C.x) {
        sp1C.x = -27.0F;
    }
    if (27.0F < sp1C.x) {
        sp1C.x = 27.0F;
    }
    sp1C.y = gk_fmadds(0.03f, mnStageSel_804D6CAD, sp1C.y);
    if (-19.0F > sp1C.y) {
        sp1C.y = -19.0F;
    }
    if (19.0F < sp1C.y) {
        sp1C.y = 19.0F;
    }
#ifdef TARGET_PC
    /* Pointing at the menus (pc/xr_pointer.h): the cursor goes where the
     * pointer is when port 1 may steer it, and a click is A only over a
     * stage (below); A anywhere else takes the last stage it was on. */
    bool pointed = false;
    {
        float px, py, w[3];
        bool leads;
        if (mnStageSel_804D50A0 <= 0 && pc_xr_pointer_at(&px, &py, &leads)) {
            pointed = true;
            pc_xr_pointer_target(false);
            if (leads && pc_xr_pointer_unproject(GET_COBJ(mnStageSel_804D6C9C),
                                                 px, py, sp1C.z, w))
            {
                sp1C.x = w[0] < -27.0F ? -27.0F : w[0] > 27.0F ? 27.0F : w[0];
                sp1C.y = w[1] < -19.0F ? -19.0F : w[1] > 19.0F ? 19.0F : w[1];
            }
        }
    }
#endif

    HSD_JObjSetTranslate(jobj, &sp1C);
    lb_8000B1CC(jobj, NULL, &sp1C);
    for (i = 0; i < 0x1E; i++) {
        if (mnStageSel_803F06D0[i].x8 != 0) {
            lb_8000B1CC(mnStageSel_803F06D0[i].x0, NULL, &sp10);
            temp_f2 = sp10.x;
            temp_f1 = mnStageSel_803F06D0[i].xC;
            if (temp_f2 - temp_f1 < sp1C.x && temp_f2 + temp_f1 > sp1C.x) {
                if (sp10.y - mnStageSel_803F06D0[i].x10 < sp1C.y &&
                    sp10.y + mnStageSel_803F06D0[i].x10 > sp1C.y)
                {
#ifdef TARGET_PC
                    if (sss_off_list(i)) {
                        /* Off the lobby's list: as empty space, no hover,
                         * name or preview, and A picks nothing (cell 30:
                         * only START, Random from the list). */
                        static int logged = -1;
                        if (mnStageSel_804D6CAE != 0x1E || logged != i) {
                            logged = i;
                            pc_log_line("sss: cell %d is off the lobby's list", i);
                        }
                        mnStageSel_804D6CAE = 0x1E;
                        return;
                    }
#endif
#ifdef TARGET_PC
                    if (pointed) {
                        pc_xr_pointer_target(true);
                    }
#endif
                    mnStageSel_804D6CAE = i;
                    return;
                }
            }
        }
    }
}

void fn_8025A560(HSD_GObj* gobj)
{
    struct StageSelUserData {
        int x0;
    }* temp_r30;
    Vec3 sp10;
    HSD_JObj* jobj = GET_JOBJ(gobj);
    temp_r30 = HSD_GObjGetUserData(gobj);

    if (mnStageSel_804D6CAE < 0x16 && (mnStageSel_804D6CAE & 1) &&
        mnStageSel_803F06D0[mnStageSel_804D6CAE].x8 == 0)
    {
        HSD_JObjSetTranslateX(jobj, 100.0F);
    } else if (mnStageSel_804D6CAE < 0x1E) {
        lb_8000B1CC(mnStageSel_803F06D0[mnStageSel_804D6CAE].x0, NULL, &sp10);
        HSD_JObjSetTranslateX(jobj, sp10.x);
        HSD_JObjSetTranslateY(jobj, sp10.y);
        HSD_JObjSetScaleX(jobj, mnStageSel_803F06D0[mnStageSel_804D6CAE].x14);
        HSD_JObjSetScaleY(jobj, mnStageSel_803F06D0[mnStageSel_804D6CAE].x18);
    } else {
        HSD_JObjSetTranslateX(jobj, 100.0F);
    }
    if (mnStageSel_804D6CAF != 0) {
        HSD_JObjReqAnimAll(jobj, 0.0F);
        HSD_JObjAnimAll(jobj);
        return;
    }
    if (++temp_r30->x0 >= 10) {
        temp_r30->x0 = 0;
        HSD_JObjReqAnimAll(jobj, 0.0F);
        HSD_JObjAnimAll(jobj);
    }
}

void fn_8025A91C(HSD_GObj* gobj)
{
    HSD_JObj* jobj = GET_JOBJ(gobj);
    if (++mnStageSel_804D6CA8 >= 0xFA) {
        mnStageSel_804D6CA8 = 0;
        HSD_JObjReqAnimAll(jobj, 0.0F);
        HSD_JObjAnimAll(jobj);
    }
}

void fn_8025A974(HSD_GObj* gobj, intptr_t unused)
{
    HSD_FogSet(gobj->hsd_obj);
}

static const Vec3 mnStageSel_803B8550 = { 0, -13, 0 };

static inline void make_stage_icon(HSD_JObj** out)
{
    HSD_GObj* gobj;
    HSD_JObj* jobj;
    gobj = GObj_Create(4, 5, 0x80);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, sss_models->icon_stacked.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x83);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 3);
    HSD_JObjAddAnimAll(jobj,
                       DP(HSD_AnimJoint, sss_models->icon_stacked.animjoint),
                       DP(HSD_MatAnimJoint,
                          sss_models->icon_stacked.matanim_joint),
                       DP(HSD_ShapeAnimJoint,
                          sss_models->icon_stacked.shapeanim_joint));
    *out = GET_JOBJ(gobj);
}

static inline void attach_menu_model(HSD_GObj* gobj)
{
    HSD_JObj* jobj;
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, sss_models->background.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x80);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 0);
    HSD_JObjAddAnimAll(jobj,
                       DP(HSD_AnimJoint, sss_models->background.animjoint),
                       DP(HSD_MatAnimJoint,
                          sss_models->background.matanim_joint),
                       DP(HSD_ShapeAnimJoint,
                          sss_models->background.shapeanim_joint));
}

static inline void make_bg_model(HSD_JObj** out)
{
    HSD_GObj* gobj;
    HSD_JObj* jobj;
    gobj = GObj_Create(4, 5, 0x80);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, sss_models->menu_border.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x82);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 0);
    HSD_JObjAddAnimAll(jobj,
                       DP(HSD_AnimJoint, sss_models->menu_border.animjoint),
                       DP(HSD_MatAnimJoint,
                          sss_models->menu_border.matanim_joint),
                       DP(HSD_ShapeAnimJoint,
                          sss_models->menu_border.shapeanim_joint));
#ifdef TARGET_PC
    if (sss_frozen_enabled) {
        mnStageSel_SlippiBannerCreate(jobj, DP(HSD_Joint, sss_models->menu_border.joint));
    }
#endif
    *out = gobj->hsd_obj;
}

static inline void make_icon_root(HSD_JObj** icons)
{
    HSD_GObj* gobj;
    HSD_JObj* jobj;
    gobj = GObj_Create(4, 5, 0x80);
    jobj = HSD_JObjLoadJoint(DP(HSD_Joint, sss_models->layout.joint));
    HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
    GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x82);
    HSD_GObj_SetupProc(gobj, mn_8022EAE0, 4);
    HSD_JObjAddAnimAll(jobj,
                       DP(HSD_AnimJoint, sss_models->layout.animjoint),
                       DP(HSD_MatAnimJoint,
                          sss_models->layout.matanim_joint),
                       DP(HSD_ShapeAnimJoint,
                          sss_models->layout.shapeanim_joint));
    icons[0] = GET_JOBJ(gobj)->child;
    HSD_JObjReqAnimAll(icons[0], 0.0F);
    HSD_JObjAnimAll(icons[0]);
}

static inline HSD_JObj* get_jobj(HSD_GObj* gobj)
{
    HSD_JObj* jobj = GET_JOBJ(gobj);
    return jobj;
}

void mnStageSel_Scene_OnEnter(void* arg0)
{
    HSD_JObj* spDC[0x13];
    u8 _[0xDC - 0xD8];
    Vec3 spCC;

    int i;
    MnSelectStageDataTable* sss_data_table;

    PAD_STACK(0xDC - 0x50);

    sss_data = (SSSData*) arg0;

    if (sss_data->force_stage_id < 0) {
        if (lbLang_IsSavedLanguageUS() != 0) {
            mnStageSel_804D6C94 = lbArchive_LoadArchive("MnSlMap.usd");
        } else {
            mnStageSel_804D6C94 = lbArchive_LoadArchive("MnSlMap.dat");
        }
        sss_data_table = HSD_ArchiveGetPublicAddress(mnStageSel_804D6C94,
                                                     "MnSelectStageDataTable");
        MenMain_cam = DP(HSD_CObjDesc, sss_data_table->cam);
        sss_models = &sss_data_table->models;
        mnStageSel_804D6CAF = 0;
        mnStageSel_804D6CA0 = 0;
        mnStageSel_804D6CAC = 0;
        mnStageSel_804D6CAD = 0;
        mnStageSel_804D6CAE = 0x1E;
        mnStageSel_804D50A0 = sss_data->unk_stage - 1;
#ifdef TARGET_PC
        if (netStageSel_Active()) {
            if (!pc_rank_session_active()) {
                mnStageSel_804D50A0 = pc_net_local_player();
                netStageSel_Reset();
                xnSss_Reset();
                {
                    int cell = netStageSel_SlippiCell();
                    if (cell >= 0) {
                        mnStageSel_804D6CAE = (u8) cell;
                        mnStageSel_804D6CAF = 2; /* chosen; resolve below */
                        net_slippi_fixed = true;
                    }
                }
            } else {
                mnStageSel_804D50A0 = -1;
            }
        }
#endif
        mnStageSel_804D6CA4 = 0x14;

        {
            HSD_GObj* gobj = mnStageSel_804D6C9C = GObj_Create(2, 3, 0x80);
            HSD_CObj* cobj = HSD_CObjLoadDesc(MenMain_cam);
            HSD_GObjObject_80390A70(gobj, HSD_GObj_CameraKind, cobj);
            GObj_SetupGXLinkMax(gobj, HSD_GObj_803910D8, 0);
            gobj->gxlink_prios = 0x11;
#ifdef TARGET_PC
            HSD_GObj_SetupProc(gobj, gmOnline_XnFlow() ? sssXnCameraProc : mn_8022BA1C, 5);
#else
            HSD_GObj_SetupProc(gobj, mn_8022BA1C, 5);
#endif
        }

        {
            HSD_GObj* gobj;
            HSD_LObj* lobj1;
            HSD_LObj* lobj2;
            gobj = GObj_Create(3, 4, 0x80);
            lobj1 = HSD_LObjLoadDesc(DP(HSD_LightDesc, sss_data_table->light0));
            lobj2 = HSD_LObjLoadDesc(DP(HSD_LightDesc, sss_data_table->light1));
            HSD_LObjSetNext(lobj1, lobj2);
            HSD_GObjObject_80390A70(gobj, (u8) HSD_GObj_LightKind, lobj1);
            GObj_SetupGXLink(gobj, HSD_GObj_LObjCallback, 0, 0x80);
        }

        {
            HSD_GObj* gobj = GObj_Create(0xE, 0xF, 0);
            HSD_Fog* fog = HSD_FogLoadDesc(DP(HSD_FogDesc, sss_data_table->fog));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_FogKind, fog);
            GObj_SetupGXLink(gobj, fn_8025A974, 0, 0x80);
        }

        {
            HSD_JObj* jobj2;
            HSD_GObj* gobj;
            gobj = GObj_Create(4, 5, 0x80);
            attach_menu_model(gobj);
            {
                HSD_GObj* g = gobj;
                jobj2 = GET_JOBJ(g);
                HSD_GObj_SetupProc(g, fn_8025A91C, 0);
                HSD_JObjReqAnimAll(jobj2, 0.0F);
                HSD_JObjAnimAll(jobj2);
            }
        }

        {
            HSD_JObj* temp_r22_4;
            make_bg_model(&temp_r22_4);
            HSD_JObjReqAnimAll(temp_r22_4, 0.0F);
            HSD_JObjAnimAll(temp_r22_4);
        }

        make_icon_root(spDC);

        for (i = 0; i < 0x12; i++) {
            spDC[i + 1] = spDC[i]->next;
            HSD_JObjReqAnimAll(spDC[i + 1], 0.0F);
            HSD_JObjAnimAll(spDC[i + 1]);
        }

        for (i = 0; i < 0x1D; i++) {
            mnStageSel_803F06D0[i].x8 =
                gm_80164430(mnStageSel_803F06D0[i].stkind) ? 2 : 1;
        }

        for (i = 0; i <= 0xA; i++) {
            HSD_JObj* temp_r22_6;
            HSD_JObj* jobj;
            make_stage_icon(&temp_r22_6);
            jobj = temp_r22_6;
            lb_8000C1C0(jobj, spDC[i]);
            mnStageSel_803F06D0[i * 2].x0 = temp_r22_6->child->next;
            switch (mnStageSel_803F06D0[i * 2].x8) {
            case 0:
                HSD_JObjSetFlags(mnStageSel_803F06D0[i * 2].x0, JOBJ_HIDDEN);
                break;
            case 1:
                HSD_JObjReqAnimAllByFlags(mnStageSel_803F06D0[i * 2].x0, 0x10,
                                          1.0F);
                break;
            default:
                HSD_JObjReqAnimAllByFlags(mnStageSel_803F06D0[i * 2].x0, 0x10,
                                          mnStageSel_803F06D0[i * 2].x9 / 2 +
                                              2);
                break;
            }
            mnStageSel_803F06D0[i * 2 + 1].x0 = temp_r22_6->child;
            switch (mnStageSel_803F06D0[i * 2 + 1].x8) {
            case 0:
                HSD_JObjSetFlags(mnStageSel_803F06D0[i * 2 + 1].x0,
                                 JOBJ_HIDDEN);
                break;
            case 1:
                HSD_JObjReqAnimAllByFlags(mnStageSel_803F06D0[i * 2 + 1].x0,
                                          0x10, 1.0F);
                break;
            default:
                HSD_JObjReqAnimAllByFlags(
                    mnStageSel_803F06D0[i * 2 + 1].x0, 0x10,
                    mnStageSel_803F06D0[i * 2 + 1].x9 / 2 + 2);
                break;
            }
            HSD_JObjAnimAll(jobj);
            HSD_ForeachAnim(jobj, JOBJ_TYPE, TOBJ_MASK, HSD_AObjStopAnim,
                            AOBJ_ARG_AOV, NULL);
        }

        for (i = 0xB; i <= 0xF; i++) {
            HSD_JObj* jobj;
            HSD_GObj* gobj = GObj_Create(4, 5, 0x80);
            s32 temp_r22_7;
            HSD_JObj* temp_r23_3;
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        sss_models->icon_special.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x83);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 3);
            HSD_JObjAddAnimAll(jobj,
                               DP(HSD_AnimJoint,
                                  sss_models->icon_special.animjoint),
                               DP(HSD_MatAnimJoint,
                                  sss_models->icon_special.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  sss_models->icon_special.shapeanim_joint));
            temp_r23_3 = gobj->hsd_obj;
            lb_8000C1C0(temp_r23_3, spDC[i]);
            mnStageSel_803F06D0[i + 13].x0 = temp_r23_3;
            switch (mnStageSel_803F06D0[i + 13].x8) {
            case 1:
                mnStageSel_803F06D0[i + 13].x8 = 0;
                /* fallthrough */
            case 0:
                HSD_JObjSetFlagsAll(temp_r23_3, JOBJ_HIDDEN);
                break;
            default:
                temp_r22_7 = mnStageSel_803F06D0[i + 13].x9 - 0x16;
                do_anim(temp_r23_3, temp_r22_7);
                break;
            }
        }

        {
            HSD_JObj* jobj;
            HSD_GObj* gobj = GObj_Create(4, 5, 0x80);
            HSD_JObj* temp_r22_8;
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        sss_models->icon_random.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x83);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 3);
            HSD_JObjAddAnimAll(jobj,
                               DP(HSD_AnimJoint,
                                  sss_models->icon_random.animjoint),
                               DP(HSD_MatAnimJoint,
                                  sss_models->icon_random.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  sss_models->icon_random.shapeanim_joint));
            temp_r22_8 = gobj->hsd_obj;
            lb_8000C1C0(temp_r22_8, spDC[0x10]);
            do_anim(temp_r22_8, 2);
            mnStageSel_803F06D0[0x1D].x0 = temp_r22_8;
        }

        for (i = 0x11; i <= 0x12; i++) {
            HSD_JObj* jobj;
            HSD_AnimJoint* animjoint;
            HSD_GObj* gobj = GObj_Create(4, 5, 0x80);
            s32 temp_r22_9;
            HSD_JObj* temp_r23_6;
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        sss_models->icon_large.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x83);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 3);
            animjoint = DP(HSD_AnimJoint, sss_models->icon_large.animjoint);
            HSD_JObjAddAnimAll(jobj, animjoint,
                               DP(HSD_MatAnimJoint,
                                  sss_models->icon_large.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  sss_models->icon_large.shapeanim_joint));

            temp_r23_6 = gobj->hsd_obj;
            lb_8000C1C0(temp_r23_6, spDC[i]);
            mnStageSel_803F06D0[i + 5].x0 = temp_r23_6;
            switch (mnStageSel_803F06D0[i + 5].x8) {
            case 1:
                mnStageSel_803F06D0[i + 5].x8 = 0;
                /* fallthrough */
            case 0:
                HSD_JObjSetFlagsAll(temp_r23_6, JOBJ_HIDDEN);
                break;
            default:
                temp_r22_9 = mnStageSel_803F06D0[i + 5].x9 - 0x14;
                do_anim(temp_r23_6, temp_r22_9);
                break;
            }
        }

        {
            HSD_JObj* jobj;
            HSD_GObj* gobj;
            gobj = GObj_Create(4, 5, 0x80);
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        sss_models->cursor.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x86);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 2);
            HSD_JObjAddAnimAll(jobj,
                               DP(HSD_AnimJoint,
                                  sss_models->cursor.animjoint),
                               DP(HSD_MatAnimJoint,
                                  sss_models->cursor.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  sss_models->cursor.shapeanim_joint));

            {
                HSD_JObj* jobj2;
                HSD_GObj* g;
                jobj = get_jobj(gobj);
                spCC = mnStageSel_803B8550;
                g = gobj;
                jobj2 = jobj;
                HSD_GObj_SetupProc(g, fn_8025A310, 2);
                do_anim(jobj2, mnStageSel_804D50A0 + 1);
                HSD_JObjSetTranslate(jobj2, &spCC);
            }
        }

        {
            HSD_JObj* jobj;
            HSD_GObj* gobj;
            gobj = GObj_Create(HSD_GOBJ_CLASS_FIGHTER, 5, 0x80);
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        sss_models->stage_name.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x84);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 1);
            HSD_JObjAddAnimAll(jobj,
                               DP(HSD_AnimJoint,
                                  sss_models->stage_name.animjoint),
                               DP(HSD_MatAnimJoint,
                                  sss_models->stage_name.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  sss_models->stage_name.shapeanim_joint));
            {
                struct StageSelUserData* userdata;
                HSD_JObj* jobj = GET_JOBJ(gobj);
                userdata = HSD_MemAlloc(sizeof(struct StageSelUserData));
                GObj_InitUserData(gobj, 4, HSD_Free, userdata);
                HSD_GObj_SetupProc(gobj, fn_80259D84, 1);
                userdata->x0 = 0x1E;
                userdata->x4 = 0;
                userdata->x2 = 0;
            }
        }

        {
            HSD_JObj* jobj;
            HSD_GObj* gobj = GObj_Create(4, 5, 0x80);
            jobj = HSD_JObjLoadJoint(DP(HSD_Joint,
                                        sss_models->stage_preview.joint));
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, jobj);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x81);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 1);
            HSD_JObjAddAnimAll(jobj,
                               DP(HSD_AnimJoint,
                                  sss_models->stage_preview.animjoint),
                               DP(HSD_MatAnimJoint,
                                  sss_models->stage_preview.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  sss_models->stage_preview.shapeanim_joint));

            {
                HSD_GObj* g;
                HSD_JObj* jobj;
                struct foo {
                    int x0, x4;
                }* temp_r3_14;
                g = gobj;
                jobj = GET_JOBJ(g);
                temp_r3_14 = HSD_MemAlloc(sizeof(*temp_r3_14));
                GObj_InitUserData(g, 4, HSD_Free, temp_r3_14);
                HSD_GObj_SetupProc(g, fn_8025A090, 1);
                HSD_JObjReqAnimAll(jobj, 0.0F);
                HSD_JObjAnimAll(jobj);
                HSD_ForeachAnim(jobj, JOBJ_TYPE, ALL_TYPE_MASK,
                                HSD_AObjStopAnim, AOBJ_ARG_AOV, NULL);
                HSD_JObjSetTranslateX(jobj, 100.0F);
                temp_r3_14->x0 = 0x1E;
                temp_r3_14->x4 = 0;
            }
        }

        {
            HSD_JObj* jobj;
            HSD_GObj* gobj = GObj_Create(4, 5, 0x80);
            HSD_JObj* temp_r3_15 =
                HSD_JObjLoadJoint(DP(HSD_Joint,
                                     sss_models->icon_hover.joint));
            s32* temp_r3_16;
            HSD_GObjObject_80390A70(gobj, HSD_GObj_JObjKind, temp_r3_15);
            GObj_SetupGXLink(gobj, HSD_GObj_JObjCallback, 4, 0x85);
            HSD_GObj_SetupProc(gobj, mn_8022EAE0, 1);
            HSD_JObjAddAnimAll(temp_r3_15,
                               DP(HSD_AnimJoint,
                                  sss_models->icon_hover.animjoint),
                               DP(HSD_MatAnimJoint,
                                  sss_models->icon_hover.matanim_joint),
                               DP(HSD_ShapeAnimJoint,
                                  sss_models->icon_hover.shapeanim_joint));
            jobj = gobj->hsd_obj;
            temp_r3_16 = HSD_MemAlloc(sizeof(*temp_r3_16));
            GObj_InitUserData(gobj, 4, HSD_Free, temp_r3_16);
            *temp_r3_16 = 0;
            HSD_GObj_SetupProc(gobj, fn_8025A560, 1);
            HSD_JObjSetTranslateX(jobj, 100.0F);
        }

        lbAudioAx_80023F28(gmMainLib_8015ECB0());
#ifdef TARGET_PC
        if (pc_rank_session_active()) rankStageCreate();
        if (gmOnline_XnFlow()) xnSss_OverlayCreate();
#endif
    }
}

static inline HSD_PadStatus* get_pad(u8 i)
{
    return &HSD_PadCopyStatus[i];
}

/// OnFrame
void mnStageSel_Scene_OnFrame(void)
{
#ifdef TARGET_PC
    if (rank_text) {
        if (!pc_rank_session_active()) { gm_801A4B60(); return; }
        rankStageFrame();
        return;
    }
#endif
#ifdef TARGET_PC
    frozenToggleFrame();
    sss_mark_off_list();
    if (gmOnline_XnFlow() && netStageSel_Active()) {
        xnSss_Frame();
    }
#endif
    if (sss_data->force_stage_id >= 0) {
        mnStageSel_804D6CAF = 2;
        sss_data->vs.start.rules.stkind = sss_data->force_stage_id;
        gm_801A4B60();
        return;
    }
    if (sss_data->no_lras == 0 && mn_8022F218()) {
        sfxBack();
        lb_800145F4();
        HSD_GObjFree(mnStageSel_804D6C9C);
        mn_8022F268();
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
        return;
    }
    bool b_pressed = false;
    if (mnStageSel_804D50A0 < 0) {
        mnStageSel_804D6CA0 = 0;
        mnStageSel_804D6CA0 |= HSD_PadCopyStatus[0].trigger;
        mnStageSel_804D6CA0 |= HSD_PadCopyStatus[1].trigger;
        mnStageSel_804D6CA0 |= HSD_PadCopyStatus[2].trigger;
        mnStageSel_804D6CA0 |= HSD_PadCopyStatus[3].trigger;
        b_pressed = (mnStageSel_804D6CA0 & 0x200) != 0;
        {
            int i;
            for (i = 0; i < 4; i++) {
                mnStageSel_804D6CAC = get_pad(i)->stickX;
                mnStageSel_804D6CAD = get_pad(i)->stickY;
                if (get_pad(i)->stickX < -0x1E || get_pad(i)->stickX > +0x1E ||
                    get_pad(i)->stickY < -0x1E || get_pad(i)->stickY > +0x1E)
                {
                    break;
                }
            }
        }
    } else {
        mnStageSel_804D6CA0 = get_pad(mnStageSel_804D50A0)->trigger;
        mnStageSel_804D6CAC = get_pad(mnStageSel_804D50A0)->stickX;
        mnStageSel_804D6CAD = get_pad(mnStageSel_804D50A0)->stickY;
#ifdef TARGET_PC
        if (gmOnline_XnFlow()) {
            /* XeleeNet: the host alone takes everyone back to the CSS, by
             * message (xnSss_Back, below). Once the draw is made, the
             * cursor stops too. */
            if (xn_sss.final) {
                mnStageSel_804D6CA0 = 0;
                mnStageSel_804D6CAC = mnStageSel_804D6CAD = 0;
            }
        } else if (netStageSel_Active() && !pc_rank_session_active()) {
            b_pressed = (HSD_PadCopyStatus[0].trigger & 0x200) ||
                        (HSD_PadCopyStatus[1].trigger & 0x200);
        } else
#endif
        {
            b_pressed = (mnStageSel_804D6CA0 & 0x200) != 0;
        }
    }
    if (mnStageSel_804D6CAC < -0x1E) {
        mnStageSel_804D6CAC += 0x1E;
    } else if (mnStageSel_804D6CAC > 0x1E) {
        mnStageSel_804D6CAC -= 0x1E;
    } else {
        mnStageSel_804D6CAC = 0;
    }
    if (mnStageSel_804D6CAD < -0x1E) {
        mnStageSel_804D6CAD += 0x1E;
    } else if (mnStageSel_804D6CAD > 0x1E) {
        mnStageSel_804D6CAD -= 0x1E;
    } else {
        mnStageSel_804D6CAD = 0;
    }
    if (mnStageSel_804D6CA4 != 0) {
        mnStageSel_804D6CA4 -= 1;
        return;
    }
#ifdef TARGET_PC
    if (gmOnline_XnFlow() && netStageSel_Active()) {
        b_pressed = xnSss_Back();
    }
#endif
    if (sss_data->x1 == 0 && b_pressed && mnStageSel_804D6CAF == 0)
    {
        sfxBack();
        gm_801A4B60();
    }
    if (mnStageSel_804D6CAF == 2) {
#ifdef TARGET_PC
        if (netStageSel_Active() && !pc_rank_session_active()) {
            netStageSel_Poll();
            if (pc_net_peer_status() != 0) {
                gm_801A4B60();
                return;
            }
            if (gmOnline_XnFlow()) {
                /* FINAL's card, set by xnSss_Frame */
            } else if (!net_slippi_fixed) {
                if (!netStageSel_AllPicked()) {
                    return; /* someone still choosing; keep showing our pick */
                }
                mnStageSel_804D6CAE = netStageSel_Resolve();
                if (mnStageSel_804D6CAE >= NUM_STAGES) {
                    mnStageSel_804D6CAE = netStageSel_Random();
                }
            }
        }
#endif
        /* Never index the sentinel cells, including in offline play. */
        if (mnStageSel_804D6CAE < 0 || mnStageSel_804D6CAE >= NUM_STAGES) {
            mnStageSel_804D6CAE = mnStageSel_802599EC();
        }
        sss_data->vs.start.rules.stkind =
            mnStageSel_803F06D0[mnStageSel_804D6CAE].stkind;
#ifdef TARGET_PC
        if (netStageSel_Active())
            pc_log_line("sss: resolved cell %d stage %d", mnStageSel_804D6CAE,
                        sss_data->vs.start.rules.stkind);
#endif
        gm_801A4B60();
    }
}

void mnStageSel_Scene_OnExit(UNUSED void* exit_data)
{
#ifdef TARGET_PC
    pc_xn_sss_show(false);
    HSD_MObjPcDimClear(); /* the icons' materials go with the scene */
    if (rank_text) { HSD_SisLib_803A5CC4(rank_text); rank_text = NULL; }
#endif
    if (mnStageSel_804D6C94 != NULL) {
        lbArchive_80016EFC(mnStageSel_804D6C94);
        mnStageSel_804D6C94 = NULL;
    }
    {
        SSSData* sss = sss_data;
        sss->start_game = mnStageSel_804D6CAF == 2 ? true : false;
        if (sss->start_game) {
            PreloadedGameModeState* cache = lbDvd_GetPreloadCacheScene();
            cache->game_cache.stkind = sss->vs.start.rules.stkind;
            lbDvd_80018254();
        }
    }
}

int mnSelStageRandom(void)
{
    return mnStageSel_803F06D0[mnStageSel_802599EC()].stkind;
}

int mnStageSel_8025BC08(int idx)
{
    return mnStageSel_803F06D0[idx].stkind;
}
