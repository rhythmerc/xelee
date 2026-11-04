#include "gmctfmode.h"

#include "gm_1601.h"
#include "gm_1A3F.h"
#include "gm_unsplit.h"
#include "gmctf.h"
#include "gmmovieend.h"
#include "gmvsmelee.h"
#include "types.h"
#include <melee/if/if_2FD9.h>
#include <melee/mn/types.h>

#include <stdio.h>

#include "pc/lang.h"
#include "pc/pc.h"
#include "pc/settings.h"

/* GM_CTF's own VS data: the CSS's picks and the SSS's stage carry from
 * match to match as VS Mode's do, but not into VS Mode (and are not saved:
 * Melee's save has no room for a mode of ours). */
static VsModeData ctf_vs;
/* Games played since the mode was entered: the bases swap every other game
 * (decision 12), as a lobby's session does. */
static int ctf_games;
static bool ctf_deal;

/* The match's rules: Capture the Flag's (the rules page's, settings.h) over
 * Melee's own (Custom Rules: handicap, damage ratio, friendly fire, pause,
 * and the items when Items is on). Teams are on and the match is a timed
 * one, as a lobby's CTF is (xeleenet_menu.cpp's pc_xn_menu_rules). */
static void ctfStart(StartMeleeData* start, UNUSED StartMeleeData* previous)
{
    PcCtfSettings s;
    GmCtfRules ctf;

    pc_settings_ctf(&s);
    ctf.captures_to_win = (u8) (s.captures < 1 ? 1 : s.captures > 9 ? 9 : s.captures);
    ctf.home_required = s.home;
    ctf.carry_slow = s.slow;
    ctf.swap_sides = (u8) (ctf_games & 1);
    ctf.return_frames = (u16) (s.return_s * 60);
    ctf_games++;

    start->rules.is_teams = true;
    start->rules.match_kind = MatchKind_Time;
    start->rules.is_stock = false;
    start->rules.timer_enabled = s.minutes != 0;
    start->rules.time_limit = (u32) s.minutes * 60; /* seconds */
    if (!s.items) {
        start->rules.item_freq = -1;
    }
    gmCtf_SetRules(&ctf);
    pc_log_line("ctf mode: game %d on stage %d, first to %d, %d:00, home %d, return %d s, slow %d, items %d%s",
                ctf_games, start->rules.stkind, ctf.captures_to_win, s.minutes, s.home, s.return_s, s.slow,
                s.items, ctf.swap_sides ? ", sides swapped" : "");
}

static void onEnterCss(GameModeState* state)
{
    gmVsMelee_EnterCss(state, &ctf_vs, VS_MELEE);
}

static void onExitCss(GameModeState* state)
{
    gmVsMelee_ExitCss(state, &ctf_vs);
}

/* The stage select always runs (decision 17), whatever Melee's own Stage
 * Selection rule says (Random would skip it): the CTF maps are lit and the
 * rest dimmed by mnstagesel.c's off-list. */
static void onEnterSss(GameModeState* state)
{
    SSSData* sss = gm_GetGameModeStateEnterData(state);
    sss->vs = ctf_vs;
    sss->unk_stage = 0;
    sss->force_stage_id = -1;
}

static void onExitSss(GameModeState* state)
{
    gmVsMelee_ExitSss(state, &ctf_vs, 0);
}

static void onEnterVs(GameModeState* state)
{
    gmVsMelee_EnterVs(state, &ctf_vs, ctfStart, NULL);
}

/* A tie at time is No Contest (decision 14): never sudden death. */
static void onExitVs(GameModeState* state)
{
    gmVsMelee_ExitVs(state, 4, 4);
}

static void onEnterResults(GameModeState* state)
{
    gmVsMelee_EnterResults(state);
}

static void onExitResults(GameModeState* state)
{
    gmVsMelee_ExitResults(state, &ctf_vs, 0);
}

/* VS Melee's states (gmgiant.c's), less sudden death; the challenger and
 * prize states stay, as a VS match can unlock a fighter. */
GameModeState gm_Mode_Ctf_States[] = {
    { 0, 3, 0, onEnterCss, onExitCss, { GS_CSS, &gmVsMelee_CssData, &gmVsMelee_CssData } },
    { 1, 3, 0, onEnterSss, onExitSss, { GS_SSS, &gmVsMelee_SssData, &gmVsMelee_SssData } },
    { 2, 3, 0, onEnterVs, onExitVs, { GS_VS, &gmVsMelee_StartData, &gmVsMelee_VsExitInfo } },
    { 4, 3, 0, onEnterResults, onExitResults, { GS_RESULTS, &gmVsMelee_ResultsEnterData, NULL } },
    { 0x80, 2, 0, gm_ModeState_Approach_OnEnter, NULL,
      { GS_APPROACH, &gmVsMelee_ApproachData, &gmVsMelee_ApproachData } },
    { 0x81, 2, 0, gm_ModeState_ApproachVs_OnEnter, gm_ModeState_ApproachVs_OnExit,
      { GS_VS, &gmVsMelee_StartData, &gmVsMelee_VsExitInfo } },
    { 0xC0, 2, 0, gm_ModeState_Prize_OnEnter, gm_ModeState_Prize_OnExit,
      { GS_PRIZE_INTERFACE, &if_Scene_Prize_EnterData, NULL } },
    { GM_GAMEMODESTATE_TERMINATE },
};

void gmCtfMode_OnInit(void)
{
    gm_InitVsMode(&ctf_vs);
    ctf_vs.start.rules.is_teams = true;
}

void gmCtfMode_OnLoad(void)
{
    gmVsMelee_ResetKOCounts();
    ctf_vs.start.rules.is_teams = true;
    ctf_games = 0;
    ctf_deal = true;
}

void gmCtfMode_OnUnload(void)
{
    /* No CTF left in force for whatever mode runs next. */
    gmCtf_SetRules(NULL);
}

bool gmCtfMode_InForce(void)
{
    return gm_GetCurrentGameMode() == GM_CTF;
}

bool gmCtfMode_TakeDeal(void)
{
    const bool deal = ctf_deal;
    ctf_deal = false;
    return deal;
}

void gmCtfMode_Banner(char* out, size_t size)
{
    PcCtfSettings s;
    char n[4];

    pc_settings_ctf(&s);
    snprintf(n, sizeof n, "%d", s.captures);
    pc_trf(out, size, "modes.ctf.banner", "n", n, NULL);
}
