/* SPDX-License-Identifier: GPL-3.0-or-later */
/* XeleeNet's pages (xeleenet_menu.h), drawn in the Slippi pages' line work
 * (slippi-setup.rcss, with xeleenet.rcss on top), and the CSS banner.
 *
 * For working on their look: MELEE_XN_PREVIEW=<page> shows a page with
 * sample data and no network (tools/xn_preview.py boots and screenshots
 * it), and F5 reloads every RmlUi stylesheet in the running game, and the text
 * (resources/lang/en.yaml, src/pc/lang.h). */
#include "xeleenet_menu.h"

#include "lang.h"
#include "launcher.h"
#include "menu_screen.h"
#include "net.h"
#include "net_identity.h"
#include "net_punch.h"
#include "pc.h"
#include "xeleenet.h"
#include "xeleenet_fighters.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <aurora/rmlui.hpp>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

extern "C" void lbAudioAx_80024030(int);
extern "C" void gmOnline_XnApplySettings(void);

namespace {

enum { SFX_BACK = 0, SFX_CONFIRM = 1, SFX_MOVE = 2 };

// The layout, in px, scaled to fit the frame's inside, as the Slippi pages'.
constexpr float kScreenW = 1120.f, kScreenH = 660.f;
// The Lobby Settings overlay's, scaled to fit the window over the CSS.
constexpr float kOverlayW = 1120.f, kOverlayH = 712.f;

const char* const kLeft = "\xe2\x80\xb9"; // the value arrows
const char* const kRight = "\xe2\x80\xba";
const char* const kDot = " \xc2\xb7 ";
const int kListRows = 5; // B1's rows in view
// The players' colours, as Melee's ports.
const char* const kPortColor[4] = {"#e8463c", "#4a78f0", "#f0c226", "#3dbb5a"};

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

std::string enc(const std::string& s) { return Rml::StringUtilities::EncodeRml(s); }

void sfx(int which) { lbAudioAx_80024030(which); }

// MELEE_XN_PREVIEW: the page shown with sample data, else "".
std::string s_preview;
// The next visit's note (pc_xn_menu_note), a lang key.
std::string s_note;
// A lobby that closed under us (pc_xn_menu_hide_lobby), and when.
std::string s_hidden_code;
uint64_t s_hidden_at = 0;
constexpr uint64_t kHiddenMs = 30000;
// B1 stops refreshing itself this long after the last button or click.
constexpr uint64_t kIdleMs = 5 * 60 * 1000;

// ---- the stages (H2) ----------------------------------------------------------
// GamePrefs.stage_mask's bit is a Random Stage Switch id (mnstagesw.c,
// gm_1601.c lbl_803B7808); the grid shows them competitive first.
struct Stage {
    int bit;
    const char* name; // its key
};
const Stage kStages[] = {
    {24, "xeleenet.stages.names.battlefield"},      {25, "xeleenet.stages.names.final_destination"},
    {26, "xeleenet.stages.names.dream_land"},       {4, "xeleenet.stages.names.yoshis_story"},
    {5, "xeleenet.stages.names.fountain"},          {7, "xeleenet.stages.names.pokemon_stadium"},
    {0, "xeleenet.stages.names.peachs_castle"},     {12, "xeleenet.stages.names.rainbow_cruise"},
    {1, "xeleenet.stages.names.kongo_jungle"},      {13, "xeleenet.stages.names.jungle_japes"},
    {2, "xeleenet.stages.names.great_bay"},         {14, "xeleenet.stages.names.temple"},
    {3, "xeleenet.stages.names.brinstar"},          {15, "xeleenet.stages.names.brinstar_depths"},
    {16, "xeleenet.stages.names.yoshis_island"},    {17, "xeleenet.stages.names.green_greens"},
    {6, "xeleenet.stages.names.corneria"},          {18, "xeleenet.stages.names.venom"},
    {19, "xeleenet.stages.names.poke_floats"},      {8, "xeleenet.stages.names.mute_city"},
    {20, "xeleenet.stages.names.big_blue"},         {9, "xeleenet.stages.names.onett"},
    {21, "xeleenet.stages.names.fourside"},         {10, "xeleenet.stages.names.icicle_mountain"},
    {11, "xeleenet.stages.names.mushroom_kingdom"}, {22, "xeleenet.stages.names.mushroom_kingdom_2"},
    {23, "xeleenet.stages.names.flat_zone"},        {28, "xeleenet.stages.names.kongo_jungle_64"},
    {27, "xeleenet.stages.names.yoshis_island_64"},
};
constexpr int kStageCount = static_cast<int>(sizeof kStages / sizeof kStages[0]);
constexpr unsigned kAllStages = (1u << 29) - 1;
constexpr unsigned kCompetitive = 1u << 24 | 1u << 25 | 1u << 26 | 1u << 4 | 1u << 5 | 1u << 7;
constexpr int kGridAcross = 6;

// ---- the fighters (docs/xeleenet-fighter-list-plan.md) ----------------------------
// The 25 entries in the CSS's order (xn_fighter_order), each a name key; the
// mask has a bit per CharacterKind, Sheik's following Zelda's.
const char* const kFighterNames[XN_FIGHTER_ENTRIES] = {
    "dr_mario", "mario", "luigi",  "bowser",  "peach",  "yoshi",   "dk",     "falcon",    "ganondorf",
    "falco",    "fox",   "ness",   "ics",     "kirby",  "samus",   "zelda",  "link",      "young_link",
    "pichu",    "pikachu", "puff", "mewtwo",  "gnw",    "marth",   "roy",
};
constexpr int kFightersAcross = 9;

// A picture decoded from the game's textures, for RmlUi (a texture provider
// serves it): the stage select's icons, the CSS's portraits.
struct Picture {
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
};
// The CSS's portraits for Details' fighter grid (D5 of
// docs/xeleenet-lobby-details-plan.md), by entry: read out of MnSlChr once a
// launch, after the first Details (mnCharSel_PcPortraits), served as
// xnfighter://<entry>.
std::array<Picture, XN_FIGHTER_ENTRIES> s_portraits;
bool s_portraits_wanted = false, s_portraits_asked = false;

std::string fighter_name(int entry) {
    return tr(("xeleenet.fighters.names." + std::string(kFighterNames[entry])).c_str());
}

// "all", "high", "low" or "custom": the list's id (settings JSON) and key.
const char* fighter_list_id(unsigned mask) {
    return mask == XN_FIGHTERS_ALL ? "all" : mask == XN_FIGHTERS_HIGH ? "high" : mask == XN_FIGHTERS_LOW ? "low"
                                                                                                         : "custom";
}

// "Low tiers (12)": the Host page's value.
std::string fighter_list_label(unsigned mask) {
    return tr(("xeleenet.host.fighter_lists." + std::string(fighter_list_id(mask))).c_str(),
              {{"n", xn_fighters_count(mask)}});
}

// "Low tiers", for the rules line: a list's id as the listing has it (no count
// for a guest's), "" for All.
std::string fighter_list_short(const char* id) {
    if (id == nullptr || id[0] == '\0' || std::strcmp(id, "all") == 0)
        return "";
    const bool known = std::strcmp(id, "high") == 0 || std::strcmp(id, "low") == 0;
    return tr(("xeleenet.rules_line.fighters_" + std::string(known ? id : "custom")).c_str());
}

int stage_count(unsigned mask) {
    int n = 0;
    for (const Stage& s : kStages)
        n += (mask >> s.bit) & 1;
    return n;
}

// B1's row summaries (docs/xeleenet-lobby-details-plan.md D4): the fighter
// list, "" when it's All (or unknown).
std::string fighters_summary(unsigned mask) {
    if (mask == 0 || mask == XN_FIGHTERS_ALL)
        return "";
    if (mask == XN_FIGHTERS_HIGH || mask == XN_FIGHTERS_LOW)
        return tr(mask == XN_FIGHTERS_HIGH ? "xeleenet.rules_line.fighters_high" : "xeleenet.rules_line.fighters_low");
    const int n = xn_fighters_count(mask);
    for (int i = 0; i < XN_FIGHTER_ENTRIES && n == 1; i++)
        if ((mask >> xn_fighter_order[i]) & 1)
            return tr("xeleenet.lobbies.summary.only", {{"name", fighter_name(i)}});
    return tr("xeleenet.lobbies.summary.fighters", {{"n", n}});
}

// The stage list, "" when it's Competitive (or unknown).
std::string stages_summary(unsigned mask) {
    if (mask == 0 || mask == kCompetitive)
        return "";
    if (mask == kAllStages)
        return tr("xeleenet.lobbies.summary.all_stages");
    const int n = stage_count(mask);
    for (const Stage& s : kStages)
        if (n == 1 && ((mask >> s.bit) & 1))
            return tr("xeleenet.lobbies.summary.only", {{"name", tr(s.name)}});
    return tr("xeleenet.lobbies.summary.stages", {{"n", n}});
}

// ---- the lobby's settings (H1) --------------------------------------------------

enum Preset { kOneVsOne, kDoubles, kFreeForAll, kCustom, kPresetCount };
const char* const kPresetNames[kPresetCount] = {"xeleenet.host.presets.1v1", "xeleenet.host.presets.doubles",
                                                "xeleenet.host.presets.ffa", "xeleenet.host.presets.custom"};
const char* const kPresetIds[kPresetCount] = {"1v1", "doubles", "ffa", "custom"};
const int kStocks[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 99};
const int kMinutes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 30};
const char* const kItemNames[] = {"xeleenet.host.items.off",  "xeleenet.host.items.very_low",
                                  "xeleenet.host.items.low",  "xeleenet.host.items.medium",
                                  "xeleenet.host.items.high", "xeleenet.host.items.very_high"};
const char* const kItemIds[] = {"off", "very_low", "low", "medium", "high", "very_high"};
const char* const kHandicapNames[] = {"xeleenet.rules.handicap.off", "xeleenet.rules.handicap.auto",
                                      "xeleenet.rules.handicap.on"};

// Kept for the next lobby, this launch and the next (xeleenet-lobby.txt).
struct HostSettings {
    int preset = kOneVsOne;
    std::string name;
    bool open = true; // Public: Join's list shows it
    int seats = 2;
    bool teams = false;
    bool stock = true;
    int stocks = 4;
    int minutes = 8;
    int items = -1;
    bool friendly_fire = false;
    int handicap = 0;
    int damage = 10;
    bool pause = true;
    unsigned stages = kCompetitive;
    unsigned fighters = XN_FIGHTERS_ALL;
    bool frozen = true;
    bool hide_code = false;
};
HostSettings s_host;
bool s_host_loaded;
// What the pages change: the host's own settings, or the Lobby Settings
// overlay's draft of them while it's open.
HostSettings* s_edit = &s_host;

void host_save() {
    char path[1024];
    FILE* f = pc_xn_profile_file("xeleenet-lobby.txt", path, sizeof path) ? std::fopen(path, "w") : nullptr;
    if (f == nullptr)
        return;
    const HostSettings& h = s_host;
    std::fprintf(f,
                 "preset=%d\nname=%s\nopen=%d\nseats=%d\nteams=%d\nstock=%d\nstocks=%d\nminutes=%d\nitems=%d\n"
                 "ff=%d\nhandicap=%d\ndamage=%d\npause=%d\nstages=%u\nfighters=%u\nfrozen=%d\nhide=%d\n",
                 h.preset, h.name.c_str(), h.open, h.seats, h.teams, h.stock, h.stocks, h.minutes, h.items,
                 h.friendly_fire, h.handicap, h.damage, h.pause, h.stages, h.fighters, h.frozen, h.hide_code);
    std::fclose(f);
}

// A settings file (xeleenet-lobby.txt's lines) into h; false when it can't be read.
bool host_read(const char* path, HostSettings& h) {
    char line[96];
    FILE* f = std::fopen(path, "r");
    if (f == nullptr)
        return false;
    while (std::fgets(line, sizeof line, f) != nullptr) {
        line[std::strcspn(line, "\r\n")] = '\0';
        char* eq = std::strchr(line, '=');
        if (eq == nullptr)
            continue;
        *eq = '\0';
        const std::string k = line, v = eq + 1;
        const int n = std::atoi(v.c_str());
        if (k == "preset")
            h.preset = std::clamp(n, 0, kPresetCount - 1);
        else if (k == "name")
            h.name = v.substr(0, 20);
        else if (k == "open")
            h.open = n != 0;
        else if (k == "seats")
            h.seats = std::clamp(n, 2, 4);
        else if (k == "teams")
            h.teams = n != 0;
        else if (k == "stock")
            h.stock = n != 0;
        else if (k == "stocks")
            h.stocks = std::clamp(n, 1, 99);
        else if (k == "minutes")
            h.minutes = std::clamp(n, 0, 99);
        else if (k == "items")
            h.items = std::clamp(n, -1, 4);
        else if (k == "ff")
            h.friendly_fire = n != 0;
        else if (k == "handicap")
            h.handicap = std::clamp(n, 0, 2);
        else if (k == "damage")
            h.damage = std::clamp(n, 5, 20);
        else if (k == "pause")
            h.pause = n != 0;
        else if (k == "stages")
            h.stages = static_cast<unsigned>(std::strtoul(v.c_str(), nullptr, 10)) & kAllStages;
        else if (k == "fighters")
            h.fighters = xn_fighters_normal(static_cast<unsigned>(std::strtoul(v.c_str(), nullptr, 10)));
        else if (k == "frozen")
            h.frozen = n != 0;
        else if (k == "hide")
            h.hide_code = n != 0;
    }
    std::fclose(f);
    if (h.stages == 0)
        h.stages = kCompetitive;
    if (h.fighters == 0)
        h.fighters = XN_FIGHTERS_ALL;
    return true;
}

void host_load() {
    if (s_host_loaded)
        return;
    s_host_loaded = true;
    char path[1024];
    if (pc_xn_profile_file("xeleenet-lobby.txt", path, sizeof path))
        host_read(path, s_host);
}

void preset(int p) {
    HostSettings& h = s_host;
    h.preset = p;
    switch (p) {
    case kOneVsOne:
        h.seats = 2, h.teams = false, h.stock = true, h.stocks = 4, h.minutes = 8, h.items = -1;
        h.friendly_fire = false, h.stages = kCompetitive, h.fighters = XN_FIGHTERS_ALL, h.frozen = true;
        break;
    case kDoubles:
        h.seats = 4, h.teams = true, h.stock = true, h.stocks = 4, h.minutes = 8, h.items = -1;
        h.friendly_fire = true, h.stages = kCompetitive, h.fighters = XN_FIGHTERS_ALL, h.frozen = true;
        break;
    case kFreeForAll:
        h.seats = 4, h.teams = false, h.stock = false, h.stocks = 4, h.minutes = 2, h.items = 2;
        h.friendly_fire = false, h.stages = kAllStages, h.fighters = XN_FIGHTERS_ALL, h.frozen = false;
        break;
    default:
        break;
    }
}

template <size_t N> int index_of(const int (&values)[N], int v) {
    for (size_t i = 0; i < N; i++)
        if (values[i] == v)
            return static_cast<int>(i);
    return 0;
}

template <size_t N> int step(const int (&values)[N], int v, int dir) {
    const int n = static_cast<int>(N);
    return values[(index_of(values, v) + dir + n) % n];
}

std::string on_off(bool on) { return tr(on ? "xeleenet.host.values.on" : "xeleenet.host.values.off"); }

std::string minutes_label(int m) {
    return m == 0 ? tr("xeleenet.host.values.no_limit") : tr("xeleenet.host.values.minutes", {{"n", m}});
}

std::string lobby_name() {
    return s_host.name.empty() ? tr("xeleenet.host.lobby_name", {{"name", pc_xn_name()}}) : s_host.name;
}

std::string stage_list_label(unsigned mask) {
    const char* kind = mask == kCompetitive ? "xeleenet.host.stage_lists.competitive"
                       : mask == kAllStages ? "xeleenet.host.stage_lists.all"
                                            : "xeleenet.host.stage_lists.custom";
    return tr(kind, {{"n", stage_count(mask)}});
}

// "Doubles · 4 stocks · 8:00": a lobby in a line.
std::string rules_line(bool teams, int seats, bool stock, int stocks, int minutes, const char* items) {
    const std::string mode = tr(teams        ? "xeleenet.rules_line.doubles"
                                : seats == 2 ? "xeleenet.rules_line.1v1"
                                             : "xeleenet.rules_line.ffa");
    std::string r = mode + kDot +
                    (stock ? tr("xeleenet.rules_line.stocks", {{"n", stocks}}) : tr("xeleenet.rules_line.time"));
    r += kDot + minutes_label(minutes);
    if (items != nullptr && items[0] != '\0' && std::strcmp(items, "off") != 0)
        r += kDot + tr("xeleenet.rules_line.items");
    return r;
}

// The masks go last: LAN's announcement cuts them off its `s` and sends them
// as keys of their own (xeleenet_lan.c host_announce).
std::string settings_json() {
    const HostSettings& h = s_host;
    char b[512];
    std::snprintf(b, sizeof b,
                  "{\"preset\":\"%s\",\"seats\":%d,\"mode\":\"%s\",\"win\":\"%s\",\"stocks\":%d,\"time_min\":%d,"
                  "\"items\":\"%s\",\"stage_list\":\"%s\",\"fighter_list\":\"%s\",\"pick\":\"everyone\","
                  "\"format\":\"endless\",\"first_to\":2,\"place_secs\":15,\"fighter_mask\":%u,\"stage_mask\":%u}",
                  kPresetIds[h.preset], h.seats, h.teams ? "teams" : "ffa", h.stock ? "stock" : "time", h.stocks,
                  h.minutes, kItemIds[h.items + 1],
                  h.stages == kCompetitive ? "competitive" : h.stages == kAllStages ? "all" : "custom",
                  fighter_list_id(h.fighters), h.fighters, h.stages);
    return b;
}

// ---- the Lobby Settings' Apply (docs/xeleenet-settings-overlay-plan.md) ---------------

// Whether two settings play the same: everything but the name, the
// visibility and the code's hiding, which only the listing shows.
bool plays_same(const HostSettings& a, const HostSettings& b) {
    return a.seats == b.seats && a.teams == b.teams && a.stock == b.stock && a.stocks == b.stocks &&
           a.minutes == b.minutes && a.items == b.items && (a.teams && a.friendly_fire) == (b.teams && b.friendly_fire) &&
           a.handicap == b.handicap && a.damage == b.damage && a.pause == b.pause && a.stages == b.stages &&
           a.fighters == b.fighters && a.frozen == b.frozen;
}

bool lan();

// The seats before the last Apply, should the service refuse the new ones.
int s_seats_before = 0;
// When the last Apply that changed play went out (the "Applying" notice).
uint64_t s_applying_at = 0;

// The host's settings become `next`. The listing hears at once; a change to
// play goes to every player's CSS on one frame (gmOnline_XnApplySettings).
void apply_host(const HostSettings& next) {
    const HostSettings was = s_host;
    const bool play = !plays_same(was, next);
    s_host = next;
    if (play)
        s_host.preset = kCustom; // the preset's rules no longer stand as they were
    host_save();
    s_seats_before = was.seats;
    pc_xn_update_settings(lobby_name().c_str(), s_host.open || lan(), settings_json().c_str());
    pc_log_line("xeleenet: settings applied%s %s", play ? "" : " (the listing only)", settings_json().c_str());
    if (play) {
        s_applying_at = SDL_GetTicks();
        gmOnline_XnApplySettings();
    }
}

// MELEE_XN_TEST_APPLY=<frames>:<file> (tools/net_test_xn_menu.py): once the
// file is there, the host applies the settings in it (xeleenet-lobby.txt's
// lines) that many CSS frames later, with no overlay. Once.
void test_apply_frame() {
    static int state = -1, wait = 0;
    static std::string file;
    if (state < 0) {
        const char* v = std::getenv("MELEE_XN_TEST_APPLY");
        const char* colon = v != nullptr ? std::strchr(v, ':') : nullptr;
        state = colon != nullptr ? 0 : 3;
        if (colon != nullptr) {
            wait = std::atoi(v);
            file = colon + 1;
        }
    }
    if (state == 0 || state == 1) {
        if (pc_net_active() && pc_net_local_player() != 0)
            return; // a guest's game: the host applies
        if (state == 0) {
            FILE* f = std::fopen(file.c_str(), "r");
            if (f == nullptr)
                return;
            std::fclose(f);
            state = 1;
            pc_log_line("xeleenet: test apply in %d frames (%s)", wait, file.c_str());
        }
        if (--wait > 0)
            return;
        HostSettings next = s_host;
        if (host_read(file.c_str(), next))
            apply_host(next);
        state = 2;
    }
}

// ---- sample data for MELEE_XN_PREVIEW -----------------------------------------------

PcXnListing sample(const char* code, const char* name, const char* host, const char* tag, const char* country,
                   int taken, int seats, bool teams, bool stock, int stocks, int minutes, bool first_to, bool playing,
                   bool xr) {
    PcXnListing x{};
    std::snprintf(x.code, sizeof x.code, "%s", code);
    std::snprintf(x.name, sizeof x.name, "%s", name);
    std::snprintf(x.host, sizeof x.host, "%s", host);
    std::snprintf(x.tag, sizeof x.tag, "%s", tag);
    std::snprintf(x.country, sizeof x.country, "%s", country);
    x.taken = taken, x.seats = seats, x.teams = teams, x.stock = stock, x.stocks = stocks, x.time_min = minutes;
    x.first_to = first_to, x.first_to_n = 2, x.playing = playing, x.xr = xr;
    std::snprintf(x.items, sizeof x.items, "off");
    x.fighter_mask = XN_FIGHTERS_ALL, x.stage_mask = kCompetitive;
    return x;
}

void sample_seat(PcXnListing& x, int port, const char* name, const char* tag, bool xr) {
    PcXnSeat& s = x.players[x.nplayers++];
    s = PcXnSeat{};
    std::snprintf(s.name, sizeof s.name, "%s", name);
    std::snprintf(s.tag, sizeof s.tag, "%s", tag);
    s.port = static_cast<uint8_t>(port);
    s.xr = xr;
    s.extra = name[0] == '\0'; // another pad of P1's machine
}

// tools/xn_preview.py's details (XeleeNet: 3 of 4, Low tiers, Competitive)
// and lan-details (full, in a game, a custom stage list, another pad's seat).
PcXnListing details_sample(bool on_lan) {
    PcXnListing x;
    if (!on_lan) {
        x = sample("K7MQ2D", "Low tier FFA", "RHYTHM", "K7Q2MPXA", "US", 3, 4, false, true, 4, 8, false, false, true);
        sample_seat(x, 0, "RHYTHM", "K7Q2MPXA", false);
        sample_seat(x, 1, "KOOPA", "Z2RWQ4NE", true);
        sample_seat(x, 2, "WAVEDASH KID", "3MPAT2LA", false);
        x.fighter_mask = XN_FIGHTERS_LOW, x.stage_mask = kCompetitive;
        return x;
    }
    x = sample("C0UCH1", "couch crew", "KOOPA", "Z2RWQ4NE", "", 4, 4, true, true, 3, 7, true, true, true);
    std::snprintf(x.address, sizeof x.address, "192.168.0.57");
    sample_seat(x, 0, "KOOPA", "Z2RWQ4NE", false);
    sample_seat(x, 1, "STAR", "Q4NEK7Q2", true);
    sample_seat(x, 2, "MOSS", "T2LA3MPA", false);
    sample_seat(x, 3, "", "", false);
    std::snprintf(x.items, sizeof x.items, "low");
    x.fighter_mask = XN_FIGHTERS_ALL;
    x.stage_mask = kAllStages & ~(1u << 10 | 1u << 23 | 1u << 22);
    return x;
}

// tools/xn_preview.py's css-2, css-3 and css-4: a full lobby of that many
// seats, else 0. Read from the environment, as the CSS asks before this
// file's first update.
int preview_full_seats() {
    const char* p = std::getenv("MELEE_XN_PREVIEW");
    if (p == nullptr || std::strncmp(p, "css-", 4) != 0 || p[4] < '2' || p[4] > '4' || p[5] != '\0')
        return 0;
    return p[4] - '0';
}

// The punch's view, or the preview's: three players, one still connecting.
// css-N: N players, all in, one with a name at the service's cap (12).
void view(PcXnPunchView* v) {
    if (s_preview.empty() && std::getenv("MELEE_XN_PREVIEW") == nullptr) {
        pc_xn_punch_view(v);
        return;
    }
    std::memset(v, 0, sizeof *v);
    v->phase = PC_XN_PUNCH_LOBBY;
    v->host = (s_preview == "banner" || s_preview == "lan-banner" || s_preview.rfind("css-", 0) == 0) &&
              s_preview != "css-editing" && s_preview != "css-settings-guest";
    std::snprintf(v->code, sizeof v->code, "K7MQ2D");
    const char* names[3] = {"PIXEL#3MPA", "KOOPA#Z2RW", "RHYTHM#K7Q2"};
    const char* full_names[4] = {"RHYTHM#K7Q2", "WAVEDASH KID#3MPA", "PIXEL#Z2RW", "KOOPA#8HJD"};
    const int full = preview_full_seats();
    v->nplayers = full ? full : 3;
    for (int i = 0; i < v->nplayers; i++) {
        v->players[i].port = i;
        std::snprintf(v->players[i].name, sizeof v->players[i].name, "%s", full ? full_names[i] : names[i]);
        v->players[i].local = i == (full ? 0 : 2);
        v->players[i].linked = full || i != 1;
        v->players[i].owner = i;
    }
}

// ---- the screens --------------------------------------------------------------

enum class Page { Host, Stages, Fighters, More, Lobbies, Details, Code, Joining, Failed };
// In Page's order. The board's class is the name in lower case (set_board).
const char* page_name(Page p) {
    static const char* const names[] = {"Host",    "Stages", "Fighters", "More",  "Lobbies",
                                        "Details", "Code",   "Joining",  "Failed"};
    return names[static_cast<int>(p)];
}
constexpr int kPageCount = static_cast<int>(Page::Failed) + 1;

// A focusable thing: its element id, its place (col, row) and what it does.
struct Item {
    std::string id;
    int col, row;
    bool enabled = true;
    std::function<void(int)> change; // left/right, -1 or +1; unset: not a value
    std::function<void()> act;       // A
    std::string hint;
};

int s_result = PC_XN_MENU_STAY;
int s_what = PC_XN_MENU_HOST;

// LAN: these pages on this network, with the host's game for the lobby
// service (docs/lan-lobbies-plan.md). Their text is LAN's where it differs
// (xeleenet.lan.*, the language overlay pc_xn_set_network sets).
bool lan() { return pc_xn_network() == PC_XN_NET_LAN; }

// The lobby's seats: the host's own settings, a guest's listing of them (the
// last one known, should it lapse: the CSS lays its doors out by this on
// every player), or 4 when never known.
int lobby_seats(bool host) {
    static int known;
    PcXnListing info{};
    // In a session, the seats its RULES put in force (every player's the same).
    if (pc_net_active() && pc_net_rules_seats() >= 2)
        return pc_net_rules_seats();
    if (host)
        return s_host.seats;
    if (pc_xn_lobby_info(&info) && info.seats)
        known = info.seats;
    return known ? known : 4;
}
// An address typed on LAN's keypad (after the code's '@'): four numbers.
bool address_ok(const std::string& a) {
    return std::count(a.begin(), a.end(), '.') == 3 && a.front() != '.' && a.back() != '.' &&
           a.find("..") == std::string::npos;
}
// How long LAN's list says it's looking before it says there's nothing.
constexpr uint64_t kLanLookMs = 3000;

// Melee's keyboard, when a page wants it: for the player's name or the lobby's.
enum class Typing { None, Player, Lobby };
struct {
    bool wanted = false;
    Typing what = Typing::None;
    std::string initial, prompt;
    int max_len = 12;
} s_kb;

void ask_keyboard(Typing what, const std::string& initial, const std::string& prompt, int max_len) {
    s_kb.wanted = true;
    s_kb.what = what;
    s_kb.initial = initial;
    s_kb.prompt = prompt;
    s_kb.max_len = max_len;
}

void load_fonts() {
    static bool done;
    if (done)
        return;
    done = true;
    const std::string dir = pc_resources_dir();
    for (const char* face : {"menu.ttf", "menu-bold.ttf"})
        Rml::LoadFontFace(dir + face);
    Rml::LoadFontFace(dir + "menu-heavy.ttf", false, static_cast<Rml::Style::FontWeight>(800));
}

// A legend entry: a GameCube button's glyph (xeleenet.rcss .gl) and its label.
std::string legend_key(const std::string& button, const char* label) {
    const std::string face = button == "stick"   ? "<div class=\"nub\"></div>"
                             : button == "start" ? "START"
                                                 : std::string(1, static_cast<char>(std::toupper(button[0])));
    return "<div class=\"lg\"><div class=\"gl " + button + "\">" + face + "</div>" + enc(tr(label)) + "</div>";
}

void settings_done(bool apply); // the Lobby Settings overlay closes (below)
void s_draft_set(bool host);     // and its draft (below)

// The doors' rectangles in the CSS as Melee lays it out, as fractions of the
// game's picture (the RmlUi context is that picture: Aurora's content
// framebuffer, not the window): four in a row under the portraits, measured
// on its frames. XeleeNet's CSS draws smaller about the centre
// (PC_XN_CSS_ZOOM), and css_zoom() moves a point the same way.
constexpr float kDoorX0 = 0.0668f, kDoorPitch = 0.2194f, kDoorW = 0.2080f, kDoorY0 = 0.536f, kDoorH = 0.4005f;
constexpr float css_zoom(float f) { return 0.5f + (f - 0.5f) * PC_XN_CSS_ZOOM; }

// The CSS's legend: below the doors, centred in the room the zoom leaves, a
// 1120x46 strip scaled by `scale` with its top left at (left, top), in the
// context's px. Lobby Settings puts its own legend in the same place.
struct LegendPlace {
    float scale, left, top;
};
LegendPlace css_legend_place(Rml::Vector2i d) {
    const float ls = 0.8f * d.y / 720.f;
    const float below = (css_zoom(kDoorY0 + kDoorH) + 1.f) / 2.f;
    return {ls, d.x / 2 - 560.f * ls, below * d.y - 23.f * ls};
}

// Lobby Settings is up: the CSS's own legend gives way to the overlay's.
bool s_settings_up = false;

struct Screen : Rml::EventListener {
    Rml::ElementDocument* document = nullptr;
    bool shown = false;
    float presence = 0.f;
    Rml::Vector2i dims{0, 0}, placedDims{0, 0};
    float placedPresence = -1.f;
    bool pointerWas = false, pointerPressed = false;

    Page page = Page::Host;
    std::vector<Item> items;
    int cursor = 0;
    std::string cursorId; // kept across rebuilds
    std::string last;     // the board as last built

    std::string hostNote; // H1's error line, or why we're back on H1 or B1 (pc_xn_menu_note)
    std::string code;     // J1's code
    std::string failWhy;          // J3: xeleenet.failed.<failWhy>
    std::string failHost, failErr; // what fills it in
    int failStep = 0;      // the track's red dot: 0 Find, 1 Connect
    bool failCode = false; // the code's fault: Edit code
    PcXnListing joining{}; // the lobby being joined, as listed
    bool joiningKnown = false;

    // Join's list (B1).
    std::vector<PcXnListing> rows;
    int others = 0;
    bool showFull = false;
    int fetch = 0; // 0 fetching, 1 have rows, -1 failed
    uint64_t fetchedAt = 0, fetchAsked = 0;
    uint64_t touchedAt = 0; // the last button or click; the list stops refreshing itself a while after
    uint64_t lookFrom = 0; // LAN: when the list started looking
    bool blockedLogged = false;

    std::string hintText;
    std::string lastLegend;
    int scrollTop = 0; // B1's first row in view

    // Details (docs/xeleenet-lobby-details-plan.md): the lobby as last listed,
    // its row on B1 (B goes back to it), and whether it has left the list.
    PcXnListing details{};
    int detailsRow = 0;
    bool detailsGone = false;
    // The Lobby Settings overlay over the CSS (docs/xeleenet-settings-overlay-
    // plan.md, C2): these pages again, in a document of their own, on the
    // draft (s_edit); a guest's read-only, built from the rules in force.
    bool overlay = false;
    bool readOnly = false;
    std::string lastHint;

    bool load() {
        if (document != nullptr)
            return true;
        auto* context = aurora::rmlui::get_context();
        if (context == nullptr)
            return false;
        load_fonts();
        const char* rml = overlay ? "xeleenet-settings.rml" : "xeleenet.rml";
        document = context->LoadDocument(std::string(pc_resources_dir()) + rml);
        if (document == nullptr) {
            pc_log_line("xeleenet pages: could not load %s", rml);
            return false;
        }
        document->AddEventListener(Rml::EventId::Click, this);
        document->AddEventListener(Rml::EventId::Mouseover, this);
        return true;
    }

    void go(Page p, const char* focus = nullptr) {
        if (p != page)
            pc_log_line("xeleenet pages: %s -> %s%s%s", page_name(page), page_name(p), focus != nullptr ? " at " : "",
                        focus != nullptr ? focus : "");
        page = p;
        cursorId = focus != nullptr ? focus : "";
        cursor = 0;
        refresh();
    }

    // ---- H1 and its pages ----------------------------------------------------

    // A changed row's old value, in the overlay: "was 4".
    static std::string was_tag(const std::string& was) {
        return was.empty() ? std::string()
                           : "<span class=\"was\">" + enc(tr("xeleenet.settings.was", {{"value", was}})) + "</span>";
    }

    // name, hint and note are keys. `was`: the overlay's old value, when it
    // changed; `limit`: the value can't go lower (dimmed in place). Read-only
    // rows show their value with no arrows and take no cursor.
    void choice(std::string& r, const std::string& id, int col, int row, const char* name, const std::string& value,
                std::function<void(int)> change, const char* hint, bool enabled = true, const char* note = nullptr,
                const std::string& was = std::string(), bool limit = false) {
        const bool ro = readOnly && enabled;
        r += "<div id=\"" + id + "\" class=\"xrow" + (enabled ? "" : " off") + (ro ? " ro" : "") +
             "\"><div class=\"n\">" + enc(tr(name)) +
             (note != nullptr ? "<span class=\"note\">" + enc(tr(note)) + "</span>" : std::string()) + "</div>";
        r += "<div class=\"v\">" + was_tag(was) + "<span class=\"arr" + (limit ? " lim" : "") + "\">" +
             std::string(kLeft) + "</span><span class=\"val" + (limit ? " lim" : "") + "\">" + enc(value) +
             "</span><span class=\"arr\">" + kRight + "</span></div></div>";
        Item it{id, col, row, enabled && !ro};
        it.change = std::move(change);
        it.hint = tr(hint);
        items.push_back(std::move(it));
    }

    void link(std::string& r, const std::string& id, int col, int row, const char* name, const std::string& value,
              std::function<void()> act, const char* hint, const std::string& was = std::string()) {
        r += "<div id=\"" + id + "\" class=\"xrow link\"><div class=\"n\">" + enc(tr(name)) + "</div><div class=\"v\">" +
             was_tag(was) + enc(value) + "<span class=\"go\">" + kRight + "</span></div></div>";
        Item it{id, col, row, true};
        it.act = std::move(act);
        it.hint = tr(hint);
        items.push_back(std::move(it));
    }

    void build_overlay_host(); // after the banner's state, which it reads

    static void group(std::string& r, const char* name) {
        r += "<div class=\"grp\"><span>" + enc(tr(name)) + "</span></div>";
    }

    void build_host() {
        if (overlay) {
            build_overlay_host();
            return;
        }
        HostSettings& h = *s_edit;
        auto custom = [] { s_edit->preset = kCustom; };
        std::string r;
        r += "<div id=\"preset\" class=\"pbar\"><div class=\"k\">" + enc(tr("xeleenet.host.preset")) +
             "</div><div class=\"pv\"><span class=\"arr\">" + kLeft + "</span>" + enc(tr(kPresetNames[h.preset])) +
             "<span class=\"arr\">" + kRight + "</span></div><div class=\"pk\">";
        for (int i = 0; i < kPresetCount; i++)
            r += (i > 0 ? kDot : "") + enc(tr(kPresetNames[i]));
        r += "</div></div>";
        {
            Item it{"preset", 0, 0, true};
            it.change = [](int d) { preset((s_edit->preset + d + kPresetCount) % kPresetCount); };
            it.hint = tr("xeleenet.host.hints.preset");
            items.push_back(std::move(it));
        }
        r += "<div class=\"col left\">";
        group(r, "xeleenet.host.groups.lobby");
        r += "<div id=\"lname\" class=\"xrow link\"><div class=\"n\">" + enc(tr("xeleenet.host.rows.name")) +
             "</div><div class=\"v\">" + enc(lobby_name()) + "<span class=\"type\">" + enc(tr("xeleenet.host.type")) +
             "</span></div></div>";
        {
            Item it{"lname", 0, 1, true};
            it.act = [] {
                ask_keyboard(Typing::Lobby, s_edit->name.empty() ? lobby_name() : s_edit->name,
                             tr("xeleenet.host.ask_name"), 20);
            };
            it.hint = tr("xeleenet.host.hints.name");
            items.push_back(std::move(it));
        }
        if (!lan()) // every LAN lobby is listed
            choice(r, "vis", 0, 2, "xeleenet.host.rows.visibility",
                   tr(h.open ? "xeleenet.host.values.public" : "xeleenet.host.values.private"),
                   [](int) { s_edit->open = !s_edit->open; }, "xeleenet.host.hints.visibility");
        choice(r, "seats", 0, 3, "xeleenet.host.rows.seats", std::to_string(h.seats),
               [custom](int d) { s_edit->seats = 2 + (s_edit->seats - 2 + d + 3) % 3, custom(); },
               "xeleenet.host.hints.seats");
        group(r, "xeleenet.host.groups.rules");
        choice(r, "teams", 0, 4, "xeleenet.host.rows.teams", on_off(h.teams),
               [custom](int) { s_edit->teams = !s_edit->teams, custom(); }, "xeleenet.host.hints.teams");
        if (h.stock)
            choice(r, "stocks", 0, 5, "xeleenet.host.rows.stocks", std::to_string(h.stocks),
                   [custom](int d) { s_edit->stocks = step(kStocks, s_edit->stocks, d), custom(); },
                   "xeleenet.host.hints.stocks");
        else
            choice(r, "stocks", 0, 5, "xeleenet.host.rows.win_by", tr("xeleenet.host.values.time"),
                   [custom](int) { s_edit->stock = true, custom(); }, "xeleenet.host.hints.win_by");
        choice(r, "time", 0, 6, "xeleenet.host.rows.time", minutes_label(h.minutes),
               [custom](int d) { s_edit->minutes = step(kMinutes, s_edit->minutes, d), custom(); },
               "xeleenet.host.hints.time");
        choice(r, "items", 0, 7, "xeleenet.host.rows.items", tr(kItemNames[h.items + 1]),
               [custom](int d) { s_edit->items = (s_edit->items + 1 + d + 6) % 6 - 1, custom(); },
               "xeleenet.host.hints.items");
        if (h.teams)
            choice(r, "ff", 0, 8, "xeleenet.host.rows.ff", on_off(h.friendly_fire),
                   [custom](int) { s_edit->friendly_fire = !s_edit->friendly_fire, custom(); },
                   "xeleenet.host.hints.ff");
        link(r, "more", 0, 9, "xeleenet.host.rows.more", tr("xeleenet.host.values.more"), [this] { go(Page::More); },
             "xeleenet.host.hints.more");
        r += "</div><div class=\"col right\">";
        group(r, "xeleenet.host.groups.stages");
        link(r, "flist", 1, 1, "xeleenet.host.rows.fighter_list", fighter_list_label(h.fighters),
             [this] { go(Page::Fighters); }, "xeleenet.host.hints.fighter_list");
        link(r, "slist", 1, 2, "xeleenet.host.rows.stage_list", stage_list_label(h.stages),
             [this] { go(Page::Stages); }, "xeleenet.host.hints.stage_list");
        choice(r, "pick", 1, 3, "xeleenet.host.rows.stage_pick", tr("xeleenet.host.values.stage_pick"), [](int) {},
               "xeleenet.host.hints.stage_pick", false, "xeleenet.host.soon");
        choice(r, "frozen", 1, 4, "xeleenet.host.rows.frozen", on_off(h.frozen),
               [custom](int) { s_edit->frozen = !s_edit->frozen, custom(); }, "xeleenet.host.hints.frozen");
        choice(r, "place", 1, 5, "xeleenet.host.rows.placement", tr("xeleenet.host.values.placement"), [](int) {},
               "xeleenet.host.hints.placement", false, "xeleenet.host.soon");
        group(r, "xeleenet.host.groups.set");
        choice(r, "format", 1, 6, "xeleenet.host.rows.format", tr("xeleenet.host.values.format"), [](int) {},
               "xeleenet.host.hints.format", false, "xeleenet.host.soon");
        group(r, "xeleenet.host.groups.joining");
        choice(r, "ask", 1, 7, "xeleenet.host.rows.ask", on_off(false), [](int) {}, "xeleenet.host.hints.ask", false,
               "xeleenet.host.soon");
        if (!lan()) // no codes on LAN
            choice(r, "hide", 1, 8, "xeleenet.host.rows.hide_code", on_off(h.hide_code),
                   [](int) { s_edit->hide_code = !s_edit->hide_code; }, "xeleenet.host.hints.hide_code");
        r += "<button id=\"open\" class=\"gold\">" + enc(tr("xeleenet.host.open")) + "</button></div>";
        {
            Item it{"open", 1, 9, true};
            it.act = [this] { open_lobby(); };
            it.hint = tr("xeleenet.host.hints.open");
            items.push_back(std::move(it));
        }
        set_board(r);
    }

    void build_more() {
        HostSettings& h = *s_edit;
        auto custom = [] { s_edit->preset = kCustom; };
        std::string r = "<div class=\"col single\">";
        group(r, "xeleenet.rules.group");
        choice(r, "win", 0, 0, "xeleenet.rules.rows.win_by",
               tr(h.stock ? "xeleenet.rules.values.stock" : "xeleenet.rules.values.time"),
               [custom](int) { s_edit->stock = !s_edit->stock, custom(); }, "xeleenet.rules.hints.win_by");
        choice(r, "pause", 0, 1, "xeleenet.rules.rows.pause", on_off(h.pause),
               [custom](int) { s_edit->pause = !s_edit->pause, custom(); }, "xeleenet.rules.hints.pause");
        choice(r, "handicap", 0, 2, "xeleenet.rules.rows.handicap", tr(kHandicapNames[h.handicap]),
               [custom](int d) { s_edit->handicap = (s_edit->handicap + d + 3) % 3, custom(); },
               "xeleenet.rules.hints.handicap");
        char dmg[16];
        std::snprintf(dmg, sizeof dmg, "%.1f", h.damage / 10.f);
        choice(r, "damage", 0, 3, "xeleenet.rules.rows.damage", tr("xeleenet.rules.values.damage", {{"n", dmg}}),
               [custom](int d) { s_edit->damage = 5 + (s_edit->damage - 5 + d + 16) % 16, custom(); },
               "xeleenet.rules.hints.damage");
        r += "<button id=\"done\" class=\"gold\">" + enc(tr("xeleenet.rules.done")) + "</button></div>";
        Item it{"done", 0, 4, true};
        it.act = [this] { go(Page::Host, "more"); };
        it.hint = tr("xeleenet.rules.hints.done");
        items.push_back(std::move(it));
        set_board(r);
    }

    void build_stages() {
        const unsigned m = s_edit->stages;
        std::string r = "<div class=\"chips\">";
        const char* chips[3] = {"xeleenet.stages.chips.competitive", "xeleenet.stages.chips.all",
                                "xeleenet.stages.chips.custom"};
        const bool on[3] = {m == kCompetitive, m == kAllStages, m != kCompetitive && m != kAllStages};
        for (int i = 0; i < 3; i++) {
            const std::string id = "chip" + std::to_string(i);
            r += "<div id=\"" + id + "\" class=\"chipx" + (on[i] ? " on" : "") + (i == 2 ? " dash" : "") + "\">" +
                 enc(tr(chips[i])) + "</div>";
            Item it{id, i, 0, i < 2 && !readOnly};
            it.act = [i] {
                s_edit->stages = i == 0 ? kCompetitive : kAllStages;
                s_edit->preset = kCustom;
            };
            it.hint = tr("xeleenet.stages.hint");
            items.push_back(std::move(it));
        }
        r += "<div class=\"count\">" +
             enc(tr("xeleenet.stages.count", {{"n", stage_count(m)}, {"total", kStageCount}})) +
             "</div></div><div id=\"grid\">";
        for (int i = 0; i < kStageCount; i++) {
            const Stage& s = kStages[i];
            const std::string id = "st" + std::to_string(i);
            r += "<div id=\"" + id + "\" class=\"tile" + ((m >> s.bit) & 1 ? " on" : "") + "\">" + enc(tr(s.name)) +
                 "</div>";
            Item it{id, i % kGridAcross, 1 + i / kGridAcross, !readOnly};
            it.act = [b = s.bit] {
                const unsigned want = s_edit->stages ^ (1u << b);
                if (want != 0) // one stage at least stays on
                    s_edit->stages = want;
                s_edit->preset = kCustom;
            };
            it.hint = tr("xeleenet.stages.hint");
            items.push_back(std::move(it));
        }
        r += "</div>";
        set_board(r);
    }

    // The fighter list (docs/xeleenet-fighter-list-plan.md): the 25 entries in
    // the CSS's own rows of 9, 9 and 7. A turns one on or off (one at least
    // stays on), Y keeps only it, X steps through the presets.
    void build_fighters() {
        const unsigned m = s_edit->fighters;
        std::string r = "<div class=\"chips\">";
        const char* chips[4] = {"xeleenet.fighters.chips.all", "xeleenet.fighters.chips.high",
                                "xeleenet.fighters.chips.low", "xeleenet.fighters.chips.custom"};
        const unsigned sets[3] = {XN_FIGHTERS_ALL, XN_FIGHTERS_HIGH, XN_FIGHTERS_LOW};
        const bool on[4] = {m == sets[0], m == sets[1], m == sets[2], m != sets[0] && m != sets[1] && m != sets[2]};
        for (int i = 0; i < 4; i++) {
            const std::string id = "chip" + std::to_string(i);
            r += "<div id=\"" + id + "\" class=\"chipx" + (on[i] ? " on" : "") + (i == 3 ? " dash" : "") + "\">" +
                 enc(tr(chips[i])) + "</div>";
            Item it{id, i, 0, i < 3 && !readOnly};
            if (i < 3)
                it.act = [set = sets[i]] {
                    s_edit->fighters = set;
                    s_edit->preset = kCustom;
                };
            it.hint = tr("xeleenet.fighters.hint");
            items.push_back(std::move(it));
        }
        r += "<div class=\"count\">" +
             enc(tr("xeleenet.fighters.count", {{"n", xn_fighters_count(m)}, {"total", XN_FIGHTER_ENTRIES}})) +
             "</div></div><div id=\"fgrid\">";
        for (int i = 0; i < XN_FIGHTER_ENTRIES; i++) {
            const int row = i / kFightersAcross, col = i % kFightersAcross + (row == 2 ? 1 : 0);
            if (i % kFightersAcross == 0)
                r += std::string(i ? "</div>" : "") + "<div class=\"frow" + (row == 2 ? " short" : "") + "\">";
            const unsigned bit = 1u << xn_fighter_order[i];
            const std::string id = "fi" + std::to_string(i);
            r += "<div id=\"" + id + "\" class=\"ftile" + (m & bit ? " on" : "") + "\">" + enc(fighter_name(i)) +
                 (overlay ? held_chips(xn_fighter_order[i]) : std::string()) + "</div>";
            Item it{id, col, 1 + row, !readOnly};
            it.act = [bit] {
                const unsigned want = xn_fighters_normal(s_edit->fighters ^ bit);
                if (want != 0) // one fighter at least stays on
                    s_edit->fighters = want;
                s_edit->preset = kCustom;
            };
            it.hint = tr(xn_fighter_order[i] == XN_FIGHTER_ZELDA ? "xeleenet.fighters.hint_zelda"
                         : xn_fighter_order[i] == 0x0E           ? "xeleenet.fighters.hint_ics"
                                                                 : "xeleenet.fighters.hint_one",
                         {{"name", fighter_name(i)}});
            items.push_back(std::move(it));
        }
        r += "</div></div>";
        if (overlay)
            r += "<div id=\"fheld\">" + enc(held_off_note()) + "</div>";
        set_board(r);
    }

    // C2b: a chip in the player's colour on each fighter someone holds, and
    // the line under the grid for a held one that's off the draft's list.
    std::string held_chips(int ckind) const;
    std::string held_off_note() const;

    // Y on the fighter page: only the highlighted fighter.
    bool fighters_only_this() {
        if (readOnly || page != Page::Fighters || items.empty() || items[cursor].id.rfind("fi", 0) != 0)
            return false;
        const int entry = std::atoi(items[cursor].id.c_str() + 2);
        s_edit->fighters = xn_fighters_normal(1u << xn_fighter_order[entry]);
        s_edit->preset = kCustom;
        return true;
    }

    void open_lobby() {
        if (!s_preview.empty())
            return;
        const char* url = pc_xn_url();
        if (url[0] == '\0' && !lan()) {
            hostNote = tr("xeleenet.host.no_server");
            sfx(SFX_BACK);
            return;
        }
        pc_xn_host_options(lobby_name().c_str(), s_host.open || lan(), settings_json().c_str());
        if (!pc_xn_punch_start(url, pc_xn_name(), nullptr, s_host.seats)) {
            hostNote = pc_xn_punch_error();
            sfx(SFX_BACK);
            return;
        }
        hostNote.clear();
        host_save();
        pc_net_set_teams(s_host.teams);
        pc_net_set_frozen_stadium(s_host.frozen ? 1 : 0);
        sfx(SFX_CONFIRM);
        pc_log_line("xeleenet: opening \"%s\" (%s), %s", lobby_name().c_str(), s_host.open ? "public" : "private",
                    settings_json().c_str());
        s_result = PC_XN_MENU_HOSTED;
    }

    // ---- Join: B1, J1-J4 -------------------------------------------------------

    void browse() {
        fetchAsked = SDL_GetTicks();
        if (!s_preview.empty()) {
            rows = {
                sample("K7MQ2D", "Friday netplay", "PIXEL", "3MPA", "US", 3, 4, true, true, 4, 8, true, false, true),
                sample("H3TR8B", "items on chaos", "KOOPA", "Z2RW", "US", 2, 4, false, false, 4, 3, false, true, false),
                sample("W9PX4C", "Ledge practice FD only", "WAVE", "P9TX", "CA", 1, 2, false, true, 4, 8, false, false,
                       true),
                sample("S4NQE7", "Sunday crew", "WAVEDASH KID", "Q4NE", "GB", 1, 4, true, true, 3, 7, true, false,
                       false),
                sample("M2KD7F", "Marth dittos", "LUNA", "8KQD", "US", 1, 2, false, true, 4, 8, false, false, false),
                sample("B8HC3N", "Brinstar beginners", "MOSS", "T2LA", "DE", 2, 4, false, true, 4, 8, false, false,
                       false),
                sample("R6VW9J", "Quest crew", "ORBIT", "2WXZ", "US", 3, 4, true, true, 4, 8, true, false, true),
                sample("C5PL2Q", "Late night FFA", "NOVA", "C3RE", "JP", 2, 4, false, false, 4, 4, false, true,
                       false),
            };
            // The row summaries (D4): All stages, Final Dest. only, Marth
            // only, and a long line that drops its stage part (Mushroom K.
            // II only).
            rows[1].stage_mask = kAllStages;
            rows[2].stage_mask = 1u << 25;
            rows[3].fighter_mask = 1u << 0x0E, rows[3].stage_mask = 1u << 22;
            rows[4].fighter_mask = 1u << 0x09;
            rows[5].fighter_mask = XN_FIGHTERS_LOW;
            others = 2, fetch = 1, fetchedAt = SDL_GetTicks() - 4000;
            if (lan()) {
                rows.resize(s_preview == "lan-lobbies" ? 3 : 0);
                for (PcXnListing& x : rows)
                    x.country[0] = '\0';
                others = rows.empty() ? 0 : 1, lookFrom = 0;
            }
            return;
        }
        PcNetIdentity id;
        const char* url = pc_xn_url();
        if ((url[0] == '\0' && !lan()) || !pc_xn_identity(&id)) {
            fetch = -1;
            return;
        }
        // Details asks for the full ones too: its lobby filling up isn't it
        // closing (poll_browse keeps them off B1).
        fetch = pc_xn_browse(url, &id, showFull || page == Page::Details) ? 0 : -1;
        std::memset(&id, 0, sizeof id);
    }

    // B1 and Details read the list (Details follows its lobby in it).
    bool browsing() const { return page == Page::Lobbies || page == Page::Details; }

    void poll_browse() {
        if (!browsing() || (fetch != 0 && !lan()) || !s_preview.empty())
            return; // LAN's list is live: read every frame
        PcXnListing out[PC_XN_LIST_MAX];
        int n = 0, o = 0;
        const int st = pc_xn_browse_poll(out, PC_XN_LIST_MAX, &n, &o);
        if (st == 1) {
            rows.clear();
            const bool hide = !s_hidden_code.empty() && SDL_GetTicks() - s_hidden_at < kHiddenMs;
            // Details shows its lobby's newest row; one that has gone stays
            // shown, closed.
            bool found = false;
            for (int i = 0; i < n; i++) {
                if (hide && s_hidden_code == out[i].code)
                    continue;
                if (page == Page::Details && std::strcmp(out[i].code, details.code) == 0) {
                    details = out[i];
                    detailsRow = static_cast<int>(rows.size());
                    found = true;
                }
                if (showFull || out[i].taken < out[i].seats)
                    rows.push_back(out[i]);
            }
            if (page == Page::Details)
                detailsGone = !found;
            others = o;
            fetch = 1;
            fetchedAt = SDL_GetTicks();
        } else if (st < 0) {
            fetch = -1;
            fetchedAt = SDL_GetTicks();
        }
    }

    void build_lobbies() {
        std::string r = "<div class=\"bhead\"><div id=\"full\" class=\"chipx" + std::string(showFull ? " on" : "") +
                        "\">" + enc(tr(showFull ? "xeleenet.lobbies.show_full.on" : "xeleenet.lobbies.show_full.off")) +
                        "</div><div class=\"upd\">";
        const bool looking = fetch == 0 || (lan() && rows.empty() && SDL_GetTicks() - lookFrom < kLanLookMs);
        if (looking)
            r += enc(tr("xeleenet.lobbies.looking"));
        else if (lan()) // live: no "updated"
            r += enc(rows.empty() ? std::string() : tr("xeleenet.lobbies.count", {{"n", rows.size()}}));
        else
            r += enc((rows.empty() ? std::string() : tr("xeleenet.lobbies.count", {{"n", rows.size()}}) + kDot) +
                     tr("xeleenet.lobbies.updated", {{"n", (SDL_GetTicks() - fetchedAt) / 1000}}));
        r += "</div></div><div id=\"list\">";
        // A window of kListRows that follows the cursor.
        const int n = static_cast<int>(rows.size());
        const int cur = cursorId.rfind("lb", 0) == 0 ? std::atoi(cursorId.c_str() + 2) : 0;
        if (cur < scrollTop)
            scrollTop = cur;
        if (cur >= scrollTop + kListRows)
            scrollTop = cur - kListRows + 1;
        scrollTop = std::clamp(scrollTop, 0, std::max(0, n - kListRows));
        if (blocked()) {
            if (!blockedLogged)
                pc_log_line("xeleenet pages: LAN discovery blocked here");
            blockedLogged = true;
            r += "<div class=\"empty bad\">" + enc(tr("xeleenet.lobbies.blocked.title")) +
                 "</div><div class=\"empty sub\">" + enc(tr("xeleenet.lobbies.blocked.line")) + "</div>";
        } else if (fetch < 0 && !lan()) {
            const char* reason = nullptr;
            const char* err = pc_xn_error(&reason);
            r += "<div class=\"empty bad\">" +
                 enc(err != nullptr && err[0] ? std::string(err) : tr("xeleenet.lobbies.unreachable")) + "</div>";
        } else if (rows.empty() && fetch == 1 && !looking) {
            r += "<div class=\"empty\">" + enc(tr("xeleenet.lobbies.empty")) + "</div>";
        }
        for (size_t i = 0; i < rows.size(); i++) {
            const PcXnListing& x = rows[i];
            const std::string id = "lb" + std::to_string(i);
            const bool full = x.taken >= x.seats;
            const char* mode = x.teams        ? "xeleenet.lobbies.modes.doubles"
                               : x.seats == 2 ? "xeleenet.lobbies.modes.1v1"
                                              : "xeleenet.lobbies.modes.ffa";
            const std::string base =
                std::string(x.host) + (x.tag[0] ? "#" + std::string(x.tag).substr(0, 4) : "") + kDot +
                (x.stock ? tr("xeleenet.lobbies.stocks", {{"n", x.stocks}}) : tr("xeleenet.lobbies.time")) + kDot +
                minutes_label(x.time_min) + kDot +
                (x.first_to ? tr("xeleenet.lobbies.first_to", {{"n", x.first_to_n}}) : tr("xeleenet.lobbies.endless"));
            // Then the lists, when they aren't the defaults; the stages go
            // first when the line runs out of room (Details always has them).
            std::string lists = fighters_summary(x.fighter_mask);
            const std::string stages = stages_summary(x.stage_mask);
            if (!stages.empty()) {
                const std::string both = lists.empty() ? stages : lists + kDot + stages;
                if (fits_row(base + kDot + both))
                    lists = both;
            }
            const std::string sub = enc(base) + (lists.empty() ? "" : enc(kDot) + "<span class=\"lsum\">" + enc(lists) +
                                                                         "</span>");
            // Full ones too: Details still shows them.
            Item it{id, 0, static_cast<int>(i), true};
            it.act = [this, i] { open_details(static_cast<int>(i)); };
            it.hint = tr("xeleenet.lobbies.hints.row");
            items.push_back(std::move(it));
            if (static_cast<int>(i) < scrollTop || static_cast<int>(i) >= scrollTop + kListRows)
                continue;
            r += "<div id=\"" + id + "\" class=\"lrow" + (full ? " full" : "") + "\"><div class=\"lw\"><div class=\"ln\">" +
                 enc(x.name) + "</div><div class=\"ls\">" + sub + "</div></div><div class=\"lm\">" +
                 enc(tr(mode)) + "</div><div class=\"lb\">" +
                 (x.playing ? "<span class=\"badge game\">" + enc(tr("xeleenet.lobbies.in_game")) + "</span>"
                  : x.xr    ? "<span class=\"badge\">" + enc(tr("xeleenet.lobbies.xr")) + "</span>"
                            : std::string()) +
                 "</div><div class=\"lt\">" + std::to_string(x.taken) + "/" + std::to_string(x.seats) +
                 "</div><div class=\"lc\">" + enc(x.country) + "</div></div>";
        }
        r += "</div>";
        if (n > kListRows) {
            // The scrollbar: the thumb is the window's share of the list.
            const float track = kListRows * 96.f - 12.f;
            const float h = std::max(36.f, track * kListRows / n);
            r += "<div id=\"sbar\"><div class=\"thumb\" style=\"top: " +
                 fmt((track - h) * scrollTop / (n - kListRows)) + "px; height: " + fmt(h) + "px;\"></div></div>";
        }
        if (others > 0)
            r += "<div class=\"foot\">" + enc(tr("xeleenet.lobbies.others", {{"n", others}})) + "</div>";
        set_board(r);
    }

    // ---- Details: a lobby before joining it ---------------------------------

    void open_details(int i) {
        details = rows[i];
        detailsRow = i;
        detailsGone = false;
        log_details();
        go(Page::Details, "dtop");
        if (lan() && s_preview.empty())
            browse(); // full lobbies too, while here (the service's next fetch asks)
    }

    // "RHYTHM#K7Q2", or for another pad "RHYTHM 2", as the CSS names it.
    static std::string seat_name(const PcXnListing& x, const PcXnSeat& s) {
        if (!s.extra)
            return std::string(s.name) + (s.tag[0] ? "#" + std::string(s.tag).substr(0, 4) : "");
        int n = 1;
        std::string owner;
        for (int i = 0; i < x.nplayers; i++) {
            const PcXnSeat& o = x.players[i];
            if (!o.extra && o.port == s.owner)
                owner = o.name;
            if (o.extra && o.owner == s.owner && o.port <= s.port)
                n++;
        }
        return owner.empty() ? tr("xeleenet.details.other_pad", {{"n", n}})
                             : tr("xeleenet.details.pad_seat", {{"name", owner}, {"n", n}});
    }

    void log_details() const {
        const PcXnListing& x = details;
        std::string who;
        for (int i = 0; i < x.nplayers; i++)
            who += (who.empty() ? "" : ",") + std::string("P") + std::to_string(x.players[i].port + 1) + "=" +
                   seat_name(x, x.players[i]) + (x.players[i].port == 0 ? "(host)" : "");
        pc_log_line("xeleenet pages: details %s %d/%d players=%s fighters=%08x stages=%08x%s", x.code, x.taken,
                    x.seats, who.c_str(), x.fighter_mask, x.stage_mask, x.playing ? " playing" : "");
    }

    bool details_full() const { return details.taken >= details.seats; }
    bool details_joinable() const { return !details_full() && !detailsGone; }

    // The seat a joiner would get: the lowest port nobody holds.
    int details_open_port() const {
        unsigned held = 1;
        for (int i = 0; i < details.nplayers; i++)
            held |= 1u << details.players[i].port;
        for (int q = 1; q < 4; q++)
            if (!(held & (1u << q)))
                return q;
        return details.taken;
    }

    // The place line: the region, or on LAN the host's address.
    std::string details_place() const { return lan() ? details.address : details.country; }

    // "Off: Fox, Falco" or "Only: Marth", whichever is shorter (ties go to
    // Off); every name on: `all`.
    template <typename Name>
    static std::string names_line(int total, const std::function<bool(int)>& on, Name name, const char* all) {
        std::string offs, ons;
        int n_on = 0;
        for (int i = 0; i < total; i++) {
            std::string& to = on(i) ? ons : offs;
            n_on += on(i);
            // Each name kept on one line ("Young Link"): no-break spaces.
            std::string n = name(i);
            for (size_t at; (at = n.find(' ')) != std::string::npos;)
                n.replace(at, 1, "\xc2\xa0");
            to += (to.empty() ? "" : ", ") + n;
        }
        if (n_on == total)
            return tr(all);
        return offs.size() <= ons.size() ? tr("xeleenet.details.off", {{"names", offs}})
                                         : tr("xeleenet.details.only", {{"names", ons}});
    }

    void build_details() {
        const PcXnListing& x = details;
        const bool can = details_joinable();
        // The top block: the name, seats and place; the host and the rules.
        // A (or a click on it) joins.
        std::string r = "<div id=\"dtop\"><div class=\"dl1\"><span class=\"dn\">" + enc(x.name) + "</span>";
        if (x.playing)
            r += "<span class=\"badge game\">" + enc(tr("xeleenet.lobbies.in_game")) + "</span>";
        if (detailsGone)
            r += "<span class=\"badge gone\">" + enc(tr("xeleenet.details.closed_badge")) + "</span>";
        r += "<div class=\"dplace\">" +
             (details_place().empty() ? std::string() : "<span class=\"dpl\">" + enc(details_place()) + "</span>") +
             enc(tr("xeleenet.details.seats", {{"n", x.taken}, {"m", x.seats}})) + "</div></div>";
        const std::string host = std::string(x.host) + (x.tag[0] ? "#" + std::string(x.tag).substr(0, 4) : "");
        r += "<div class=\"dl2\">" +
             enc(host + kDot + rules_line(x.teams, x.seats, x.stock, x.stocks, x.time_min, x.items) + kDot +
                 (x.first_to ? tr("xeleenet.lobbies.first_to", {{"n", x.first_to_n}})
                             : tr("xeleenet.lobbies.endless"))) +
             "</div></div>";
        {
            Item it{"dtop", 0, 0, can};
            it.act = [this] {
                joining = details;
                joiningKnown = true;
                code = details.code;
                join();
            };
            items.push_back(std::move(it));
        }
        // Players: a row per seat in port order; open seats dashed.
        r += "<div class=\"col dleft\">";
        group(r, "xeleenet.details.players");
        for (int q = 0; q < std::clamp(x.seats, 2, 4); q++) {
            const PcXnSeat* s = nullptr;
            for (int i = 0; i < x.nplayers; i++)
                if (x.players[i].port == q)
                    s = &x.players[i];
            const std::string pn = "<div class=\"pn\">P" + std::to_string(q + 1) + "</div>";
            if (s == nullptr) {
                r += "<div class=\"dseat open\">" + pn + "<div class=\"nm\">" + enc(tr("xeleenet.details.open_seat")) +
                     "</div></div>";
                continue;
            }
            r += "<div class=\"dseat\"><div class=\"pc\" style=\"background-color: " + std::string(kPortColor[q]) +
                 ";\"></div>" + pn + "<div class=\"nm\">" + enc(seat_name(x, *s)) + "</div><div class=\"tags\">" +
                 (q == 0 ? "<span class=\"badge\">" + enc(tr("xeleenet.joining.host")) + "</span>" : std::string()) +
                 (s->xr ? "<span class=\"badge\">" + enc(tr("xeleenet.lobbies.xr")) + "</span>" : std::string()) +
                 "</div></div>";
        }
        // Stages: by name, the shorter side (dimmed when it's the off one).
        r += "<div class=\"grp\"><span>" + enc(tr("xeleenet.details.stages")) + "</span><div class=\"gv\">" +
             enc(stage_list_label(x.stage_mask)) + "</div></div><div class=\"dchips\">";
        const int n_on = stage_count(x.stage_mask);
        if (n_on == kStageCount) {
            r += "<div class=\"dline\">" + enc(tr("xeleenet.details.every_stage")) + "</div>";
        } else {
            const bool offs = kStageCount - n_on <= n_on;
            if (offs)
                r += "<div class=\"dchip lbl\">" + enc(tr("xeleenet.details.off_label")) + "</div>";
            for (const Stage& s : kStages)
                if (((x.stage_mask >> s.bit) & 1) != offs)
                    r += "<div class=\"dchip" + std::string(offs ? " dim" : "") + "\">" + enc(tr(s.name)) + "</div>";
        }
        r += "</div></div>";
        // Fighters: the CSS's rows of 9, 9 and 7, off-list cells dimmed, and
        // their names in a line under them.
        r += "<div class=\"col dright\"><div class=\"grp\"><span>" + enc(tr("xeleenet.details.fighters")) +
             "</span><div class=\"gv\">" + enc(fighter_list_label(x.fighter_mask)) + "</div></div><div id=\"dgrid\">";
        for (int i = 0; i < XN_FIGHTER_ENTRIES; i++) {
            if (i % kFightersAcross == 0)
                r += std::string(i ? "</div>" : "") + "<div class=\"drow" + (i / kFightersAcross == 2 ? " short" : "") +
                     "\">";
            const bool on = (x.fighter_mask >> xn_fighter_order[i]) & 1;
            r += "<div class=\"dcell" + std::string(on ? "" : " off") + "\">" + fighter_cell(i) + "</div>";
        }
        const unsigned fm = x.fighter_mask;
        r += "</div></div><div class=\"dnames\">" +
             enc(names_line(
                 XN_FIGHTER_ENTRIES, [fm](int i) { return ((fm >> xn_fighter_order[i]) & 1) != 0; },
                 [](int i) { return fighter_name(i); }, "xeleenet.details.every_fighter")) +
             "</div></div>";
        set_board(r);
    }

    // A fighter's cell in the Details grid: its CSS portrait once decoded
    // (D5), its name until then (and for good, should that fail).
    static std::string fighter_cell(int entry) {
        s_portraits_wanted = true; // read on the game's next frame (mnxeleenet.c)
        if (!s_portraits[entry].rgba.empty())
            return "<img src=\"xnfighter://" + std::to_string(entry) + "\"/>";
        return "<div class=\"dcn\">" + enc(fighter_name(entry)) + "</div>";
    }

    std::string details_hint() const {
        if (detailsGone)
            return tr("xeleenet.details.hints.closed");
        if (details_full())
            return tr("xeleenet.details.hints.full");
        if (details.playing)
            return tr("xeleenet.details.hints.in_game");
        return tr("xeleenet.details.hints.open",
                  {{"n", details.seats - details.taken}, {"p", details_open_port() + 1}});
    }

    // Whether a row's second line fits its 600 px (.lrow .lw), measured on
    // the hidden #lsm (xeleenet.rml), which has the line's font. Until that
    // has its font (the first frame), a count of characters stands in.
    bool fits_row(const std::string& line) const {
        constexpr int kRowLinePx = 600, kRowLineChars = 62;
        auto* m = document != nullptr ? document->GetElementById("lsm") : nullptr;
        if (m != nullptr && m->GetFontFaceHandle() != 0)
            return Rml::ElementUtilities::GetStringWidth(m, line) <= kRowLinePx;
        size_t chars = 0;
        for (const char c : line)
            chars += (static_cast<unsigned char>(c) & 0xC0) != 0x80; // UTF-8 lead bytes
        return chars <= kRowLineChars;
    }

    // LAN's list with nothing at all heard, not even our own query: the
    // network blocks discovery, and joining is by address (B2).
    bool blocked() const {
        return lan() && rows.empty() &&
               (s_preview == "lan-blocked" || (s_preview.empty() && pc_xn_lan_unavailable()));
    }

    // J1's code, or on LAN the host's address after an '@'.
    bool code_ready() const {
        return lan() && !code.empty() && code[0] == '@' ? address_ok(code.substr(1)) : code.size() == 6;
    }

    // LAN's J2: the host's address, typed on a keypad of digits and a dot.
    void build_address() {
        if (code.empty() || code[0] != '@')
            code = "@";
        std::string r = "<div id=\"addr\"><div class=\"al\">" + enc(tr("xeleenet.code.address")) +
                        "</div><div class=\"av\">" + enc(code.substr(1)) + "<span class=\"caret\">|</span></div></div>" +
                        "<div id=\"keys\" class=\"addr\">";
        static const char keys[] = "123456789.0";
        for (int i = 0; i < 12; i++) {
            const bool del = i == 11;
            const std::string id = del ? "k-del" : keys[i] == '.' ? "k-dot" : std::string("k-") + keys[i];
            r += "<div id=\"" + id + "\" class=\"key" + (del ? " del" : "") + "\">" +
                 (del ? enc(tr("xeleenet.code.del")) : std::string(1, keys[i])) + "</div>";
            Item it{id, i % 3, i / 3, true};
            if (del)
                it.act = [this] {
                    if (code.size() > 1)
                        code.pop_back();
                };
            else
                it.act = [this, c = keys[i]] {
                    if (code.size() < 22) // "@" and 255.255.255.255:65535
                        code += c;
                };
            it.hint = tr("xeleenet.code.hints.type");
            items.push_back(std::move(it));
        }
        r += "</div>";
        set_board(r);
    }

    void build_code() {
        if (lan()) {
            build_address();
            return;
        }
        // Placed one by one: three tiles, a dash, three tiles.
        std::string r = "<div id=\"tiles\" class=\"six\"><div class=\"dash\" style=\"left: 318px;\">-</div>";
        for (int i = 0; i < 6; i++) {
            const bool has = i < static_cast<int>(code.size());
            r += "<div class=\"cell" + std::string(has ? "" : " empty") +
                 (i == static_cast<int>(code.size()) ? " cur" : "") + "\" style=\"left: " +
                 std::to_string(i * 106 + (i >= 3 ? 56 : 0)) + "px;\">" + (has ? std::string(1, code[i]) : "&nbsp;") +
                 "</div>";
        }
        r += "</div><div id=\"keys\" class=\"xn\">";
        static const char keys[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        const int n = static_cast<int>(sizeof keys - 1);
        for (int i = 0; i < n; i++) {
            const std::string id = std::string("k-") + keys[i];
            r += "<div id=\"" + id + "\" class=\"key\">" + std::string(1, keys[i]) + "</div>";
            Item it{id, i % 9, i / 9, true};
            it.act = [this, c = keys[i]] {
                if (code.size() < 6)
                    code += c;
                if (code.size() == 6)
                    cursorId = "k-ok";
            };
            it.hint = tr("xeleenet.code.hints.type");
            items.push_back(std::move(it));
        }
        r += "<div id=\"k-ok\" class=\"key join\">" + enc(tr("xeleenet.code.join")) + "</div></div>";
        Item it{"k-ok", 5, n / 9, true};
        it.act = [this] { join(); };
        it.hint = tr("xeleenet.code.hints.join");
        items.push_back(std::move(it));
        set_board(r);
    }

    // J2-J4 share the track and the card.
    std::string track(int at, bool failed) const {
        const char* labels[3] = {"xeleenet.joining.steps.find", "xeleenet.joining.steps.connect",
                                 "xeleenet.joining.steps.lobby"};
        std::string r = "<div id=\"track\"><div class=\"line\"></div><div class=\"line done\" style=\"width: " +
                        fmt(at <= 0 ? 0.f : 50.f * std::min(at, 2)) + "%;\"></div>";
        for (int i = 0; i < 3; i++)
            r += "<div class=\"step" + std::string(at > i ? " done" : "") +
                 (at == i ? (failed ? " bad" : " now") : "") + "\" style=\"left: " + fmt(50.f * i) +
                 "%;\"><div class=\"dot\"><div class=\"mark\"></div></div><div class=\"lbl\">" + enc(tr(labels[i])) +
                 "</div></div>";
        return r + "</div>";
    }

    std::string lobby_title() const {
        PcXnListing info;
        if (pc_xn_lobby_info(&info) && info.name[0])
            return info.name;
        if (joiningKnown && joining.name[0])
            return joining.name;
        if (!code.empty() && code[0] == '@')
            return tr("xeleenet.joining.lobby_at", {{"address", code.substr(1)}});
        return tr("xeleenet.joining.lobby",
                  {{"code", code.substr(0, 3) + " " + code.substr(std::min<size_t>(3, code.size()))}});
    }

    std::string lobby_rules() const {
        PcXnListing info;
        const PcXnListing* x = pc_xn_lobby_info(&info) ? &info : joiningKnown ? &joining : nullptr;
        if (x == nullptr || x->seats == 0)
            return "";
        return rules_line(x->teams, x->seats, x->stock, x->stocks, x->time_min, x->items);
    }

    void build_joining() {
        PcXnPunchView v;
        view(&v);
        const int at = v.phase == PC_XN_PUNCH_SERVICE ? 0 : v.phase == PC_XN_PUNCH_LOBBY ? 1 : 2;
        PcXnListing info;
        const bool playing = s_preview == "progress" ||
                             (pc_xn_lobby_info(&info) && info.playing && at == 1 && v.nplayers > 1 && v.players[0].linked);
        std::string r = track(playing ? 2 : at, false);
        r += "<div id=\"lobbycard\">";
        if (playing) {
            r += "<div class=\"head\">" + enc(tr("xeleenet.joining.in_progress")) + "</div><div class=\"rules\">" +
                 enc(lobby_title()) + "</div>";
            std::string names;
            for (int i = 0; i < v.nplayers; i++)
                if (!v.players[i].local)
                    names += (names.empty() ? "" : ", ") + std::string(v.players[i].name);
            r += "<div class=\"text\">" + enc(names) + "</div><div class=\"fine\">" +
                 enc(tr("xeleenet.joining.seated_later")) + "</div>";
        } else {
            r += "<div class=\"head\">" + enc(lobby_title()) + "</div>";
            const std::string rl = lobby_rules();
            if (!rl.empty())
                r += "<div class=\"rules\">" + enc(rl) + "</div>";
            r += "<div class=\"players\">";
            for (int i = 0; i < v.nplayers; i++) {
                const auto& p = v.players[i];
                const char* how = p.local      ? "xeleenet.joining.players.you"
                                  : p.linked   ? "xeleenet.joining.players.connected"
                                  : p.via_host ? "xeleenet.joining.players.via_host"
                                               : "xeleenet.joining.players.trying";
                r += "<div class=\"player" + std::string(p.linked ? " ok" : "") + (p.local ? " me" : "") +
                     "\"><div class=\"name\">" + enc(p.name) + "</div>" +
                     (p.port == 0 ? "<div class=\"badge\">" + enc(tr("xeleenet.joining.host")) + "</div>"
                                  : std::string()) +
                     "<div class=\"how\">" + enc(tr(how)) + "</div></div>";
            }
            r += "</div>";
            r += v.nplayers == 0 ? "<div class=\"wait\">" + enc(tr("xeleenet.joining.finding")) + "</div>"
                                 : "<div class=\"fine\">" + enc(tr("xeleenet.joining.answer")) + "</div>";
        }
        r += "</div>";
        set_board(r);
    }

    std::string fail_title() const {
        const std::string host = failHost.empty() ? tr("xeleenet.failed.host.someone") : failHost;
        return tr(("xeleenet.failed." + failWhy + ".title").c_str(), {{"host", host}});
    }

    void build_failed() {
        std::string r = track(failStep, true);
        const std::string line = failWhy != "offline" ? tr(("xeleenet.failed." + failWhy + ".line").c_str())
                                 : failErr.empty()    ? tr("xeleenet.failed.offline.line")
                                                      : tr("xeleenet.failed.offline.line_error", {{"error", failErr}});
        r += "<div id=\"lobbycard\" class=\"failed\"><div class=\"head bad\">" + enc(fail_title()) +
             "</div><div class=\"why\">" + enc(line) + "</div>";
        if (failWhy == "host")
            r += "<div class=\"fine\">" + enc(tr("xeleenet.failed.host.small")) + "</div>";
        r += "</div><div class=\"buttons fail\">";
        if (failCode) {
            r += "<button id=\"edit\" class=\"btn primary\">" + enc(tr("xeleenet.failed.edit")) + "</button>";
            Item it{"edit", 0, 0, true};
            it.act = [this] {
                pc_xn_punch_end();
                go(Page::Code);
            };
            items.push_back(std::move(it));
        } else {
            r += "<button id=\"retry\" class=\"btn primary\">" + enc(tr("xeleenet.failed.retry")) + "</button>";
            Item it{"retry", 0, 0, true};
            it.act = [this] {
                pc_xn_punch_end();
                join();
            };
            items.push_back(std::move(it));
        }
        r += "<button id=\"back\" class=\"btn\">" + enc(tr("xeleenet.failed.back")) + "</button></div>";
        Item it{"back", 1, 0, true};
        it.act = [this] {
            pc_xn_punch_end();
            to_lobbies();
        };
        items.push_back(std::move(it));
        set_board(r);
    }

    // Why a join failed, in the wireframe's words (J3's table).
    void failed() {
        const char* reason = "";
        const char* err = pc_xn_error(&reason);
        const std::string punch = pc_xn_punch_error();
        const std::string why = reason != nullptr ? reason : "";
        failCode = false;
        failHost.clear();
        failErr.clear();
        failStep = 0;
        const bool byAddress = !code.empty() && code[0] == '@';
        if (why == "not_found" || why == "bad_code") {
            // On LAN a listed lobby that's gone isn't the code's fault.
            failWhy = lan() && !byAddress ? "gone" : "not_found";
            failCode = !lan() || byAddress;
        } else if (why == "full" || why == "declined" || why == "version" || why == "banned" || why == "closed") {
            failWhy = why;
        } else if (punch.find("host") != std::string::npos || s_preview == "failed") {
            PcXnPunchView v;
            view(&v);
            failWhy = "host";
            if (v.nplayers > 0 && v.players[0].port == 0)
                failHost = v.players[0].name;
            failStep = 1;
        } else {
            failWhy = "offline";
            failCode = byAddress; // LAN: nothing answered at that address
            if (err != nullptr && !lan())
                failErr = err;
        }
        pc_log_line("xeleenet pages: join failed: %s (%s)", fail_title().c_str(), why.c_str());
        sfx(SFX_BACK);
        go(Page::Failed, failCode ? "edit" : "retry");
    }

    void join() {
        if (!code_ready())
            return;
        if (!s_preview.empty()) {
            go(Page::Joining);
            return;
        }
        const char* url = pc_xn_url();
        if ((url[0] == '\0' && !lan()) || !pc_xn_punch_start(url, pc_xn_name(), code.c_str(), 4)) {
            failed();
            return;
        }
        pc_log_line("xeleenet pages: joining %s", code.c_str());
        go(Page::Joining);
    }

    // A button or click: an idle list (kIdleMs) refreshes on the next update
    // (not here: the button may start a fetch of its own).
    void touched() {
        if (SDL_GetTicks() - touchedAt >= kIdleMs)
            fetchAsked = 0;
        touchedAt = SDL_GetTicks();
    }

    // Back to B1, and to the row a join off Details came from.
    void to_lobbies() {
        const std::string row = joiningKnown && std::strcmp(joining.code, details.code) == 0
                                    ? "lb" + std::to_string(detailsRow)
                                    : std::string();
        touchedAt = SDL_GetTicks();
        joiningKnown = false;
        code.clear();
        lookFrom = SDL_GetTicks();
        blockedLogged = false;
        go(Page::Lobbies, row.empty() ? nullptr : row.c_str());
        browse();
    }

    // ---- the legend: what the buttons do, over Melee's description box ---------

    static std::string key(const std::string& button, const char* label) { return legend_key(button, label); }

    std::string legend() const {
        const Item* at = items.empty() ? nullptr : &items[cursor];
        const std::string id = at != nullptr ? at->id : "";
        std::string r;
        if (overlay)
            return "<div class=\"keys\">" + overlay_legend(at) + "</div>";
        switch (page) {
        case Page::Host:
            r += at != nullptr && at->change
                     ? key("stick", "xeleenet.legend.change")
                     : key("a", id == "open" ? "xeleenet.legend.open" : "xeleenet.legend.select");
            if (id == "preset")
                r += key("a", "xeleenet.legend.confirm"); // on to the rules (activate)
            if (id != "open")
                r += key("start", "xeleenet.legend.open");
            r += key("b", "xeleenet.legend.back");
            break;
        case Page::More:
            r += at != nullptr && at->change ? key("stick", "xeleenet.legend.change")
                                             : key("a", "xeleenet.legend.done");
            r += key("b", "xeleenet.legend.back");
            break;
        case Page::Stages:
            r += key("a", "xeleenet.legend.toggle") + key("x", "xeleenet.legend.stage_sets") +
                 key("b", "xeleenet.legend.done");
            break;
        case Page::Fighters:
            r += key("a", "xeleenet.legend.toggle");
            if (id.rfind("fi", 0) == 0)
                r += key("y", "xeleenet.legend.only_this");
            r += key("x", "xeleenet.legend.fighter_sets") + key("b", "xeleenet.legend.done");
            break;
        case Page::Details:
            if (details_joinable())
                r += key("a", "xeleenet.legend.join");
            r += key("b", "xeleenet.legend.back");
            break;
        case Page::Lobbies:
            if (!rows.empty())
                r += key("a", "xeleenet.legend.details");
            r += lan() ? key("x", "xeleenet.legend.join_address")
                       : key("y", "xeleenet.legend.enter_code") + key("x", "xeleenet.legend.refresh");
            r += key("start", showFull ? "xeleenet.legend.hide_full" : "xeleenet.legend.show_full") +
                 key("b", "xeleenet.legend.back");
            break;
        case Page::Code:
            if (lan()) { // X deletes; B is Back
                r += key("a", id == "k-del" ? "xeleenet.legend.delete" : "xeleenet.legend.type") +
                     key("x", "xeleenet.legend.delete");
                if (code_ready())
                    r += key("start", "xeleenet.legend.join");
                r += key("b", "xeleenet.legend.back");
                break;
            }
            r += key("a", id == "k-ok" ? "xeleenet.legend.join" : "xeleenet.legend.type");
            if (code.size() == 6 && id != "k-ok")
                r += key("start", "xeleenet.legend.join");
            r += key("b", code.empty() ? "xeleenet.legend.back" : "xeleenet.legend.delete");
            break;
        case Page::Joining:
            r += key("b", "xeleenet.legend.cancel");
            break;
        case Page::Failed:
            r += key("a", "xeleenet.legend.select") + key("b", "xeleenet.legend.back");
            break;
        }
        return "<div class=\"keys\">" + r + "</div>";
    }

    // The overlay's: what the stick, A, START and B do on this page.
    std::string overlay_legend(const Item* at) const {
        const bool on = at != nullptr && at->enabled;
        std::string r;
        if (readOnly) {
            if (on && at->act)
                r += key("a", "xeleenet.legend.view");
            return r + key("b", page == Page::Host ? "xeleenet.legend.close" : "xeleenet.legend.back");
        }
        if (on && at->change)
            r += key("stick", "xeleenet.legend.change");
        else if (on && at->id == "apply")
            r += key("a", "xeleenet.legend.apply");
        else if (on && at->id == "cancel")
            r += key("a", "xeleenet.legend.cancel");
        else if (on)
            r += key("a", page == Page::Host ? "xeleenet.legend.select" : "xeleenet.legend.toggle");
        if (page == Page::Fighters && at != nullptr && at->id.rfind("fi", 0) == 0)
            r += key("y", "xeleenet.legend.only_this");
        if (page == Page::Fighters)
            r += key("x", "xeleenet.legend.fighter_sets");
        else if (page == Page::Stages)
            r += key("x", "xeleenet.legend.stage_sets");
        r += key("start", "xeleenet.legend.apply");
        return r + key("b", page == Page::Host ? "xeleenet.legend.cancel" : "xeleenet.legend.back");
    }

    void update_legend() {
        if (overlay)
            update_hint();
        const std::string r = legend();
        if (r == lastLegend)
            return;
        lastLegend = r;
        if (auto* e = document->GetElementById("legend"))
            e->SetInnerRML(r);
    }

    // ---- the board ------------------------------------------------------------

    // The overlay's own hint line (the pages use Melee's hint box).
    void update_hint() {
        const char* h = hint();
        const std::string t = h != nullptr ? h : "";
        if (t == lastHint)
            return;
        lastHint = t;
        if (auto* e = document->GetElementById("ohint"))
            e->SetInnerRML(enc(t));
    }

    void set_board(const std::string& r) {
        if (r == last) {
            mark();
            update_legend();
            return;
        }
        last = r;
        auto* board = document->GetElementById("board");
        board->SetInnerRML(r);
        for (int i = 0; i < kPageCount; i++) {
            std::string kind = page_name(static_cast<Page>(i));
            kind[0] = static_cast<char>(std::tolower(kind[0]));
            board->SetClass(kind, static_cast<int>(page) == i);
        }
        mark();
        update_legend();
    }

    // The overlay's title: LOBBY SETTINGS, the page it's on, and a guest's
    // READ ONLY.
    std::string lastTitle;
    void overlay_title() {
        std::string t = "<span class=\"tt\">" + enc(tr("xeleenet.settings.title")) + "</span>";
        if (page != Page::Host)
            t += "<span class=\"ts\">" + enc(std::string(kDot) + title()) + "</span>";
        if (readOnly)
            t += "<span class=\"badge\">" + enc(tr("xeleenet.settings.read_only")) + "</span>";
        if (t != lastTitle) {
            lastTitle = t;
            if (auto* e = document->GetElementById("otitle"))
                e->SetInnerRML(t);
        }
    }

    void build() {
        if (document == nullptr)
            return;
        if (overlay)
            overlay_title();
        items.clear();
        switch (page) {
        case Page::Host: build_host(); break;
        case Page::More: build_more(); break;
        case Page::Stages: build_stages(); break;
        case Page::Fighters: build_fighters(); break;
        case Page::Lobbies: build_lobbies(); break;
        case Page::Details: build_details(); break;
        case Page::Code: build_code(); break;
        case Page::Joining: build_joining(); break;
        case Page::Failed: build_failed(); break;
        }
    }

    void mark() {
        if (items.empty())
            return;
        if (!cursorId.empty())
            for (size_t i = 0; i < items.size(); i++)
                if (items[i].id == cursorId && items[i].enabled)
                    cursor = static_cast<int>(i);
        cursor = std::clamp(cursor, 0, static_cast<int>(items.size()) - 1);
        if (!items[cursor].enabled)
            for (size_t i = 0; i < items.size(); i++)
                if (items[i].enabled) {
                    cursor = static_cast<int>(i);
                    break;
                }
        cursorId = items[cursor].id;
        for (size_t i = 0; i < items.size(); i++)
            if (auto* e = document->GetElementById(items[i].id))
                e->SetClass("cursor", static_cast<int>(i) == cursor && items[i].enabled);
    }

    void refresh() {
        last.clear();
        build();
    }

    bool host_like() const { return page == Page::Host || page == Page::More; }

    void move(int dir) {
        if (items.empty())
            return;
        int best = -1;
        if (host_like() && (dir == PC_SCREEN_UP || dir == PC_SCREEN_DOWN)) {
            // Up and down: the preset, the left column, the right one.
            const int n = static_cast<int>(items.size());
            for (int k = 1; k < n && best < 0; k++) {
                const int i = (cursor + (dir == PC_SCREEN_DOWN ? k : -k) + n) % n;
                if (items[i].enabled)
                    best = i;
            }
        } else if (host_like()) {
            // Left and right change a value; on a link they cross columns.
            if (items[cursor].change) {
                items[cursor].change(dir == PC_SCREEN_LEFT ? -1 : 1);
                sfx(SFX_MOVE);
                refresh();
                return;
            }
            const int want = dir == PC_SCREEN_LEFT ? 0 : 1, row = items[cursor].row;
            int bestDist = 1000;
            for (size_t i = 0; i < items.size(); i++)
                if (items[i].enabled && items[i].col == want && items[i].id != "preset" &&
                    std::abs(items[i].row - row) < bestDist)
                    bestDist = std::abs(items[i].row - row), best = static_cast<int>(i);
        } else {
            const Item& at = items[cursor];
            float bestScore = 1e9f;
            for (size_t i = 0; i < items.size(); i++) {
                const Item& it = items[i];
                if (!it.enabled || static_cast<int>(i) == cursor)
                    continue;
                const int dr = it.row - at.row, dc = it.col - at.col;
                bool ok = false;
                float score = 0;
                switch (dir) {
                case PC_SCREEN_UP: ok = dr < 0, score = -dr * 10.f + std::abs(dc); break;
                case PC_SCREEN_DOWN: ok = dr > 0, score = dr * 10.f + std::abs(dc); break;
                case PC_SCREEN_LEFT: ok = dr == 0 && dc < 0, score = -dc; break;
                case PC_SCREEN_RIGHT: ok = dr == 0 && dc > 0, score = dc; break;
                }
                if (ok && score < bestScore)
                    bestScore = score, best = static_cast<int>(i);
            }
        }
        if (best >= 0 && best != cursor) {
            cursor = best;
            cursorId = items[cursor].id;
            sfx(SFX_MOVE);
            if (page == Page::Lobbies)
                refresh(); // the window may scroll
            else
                mark();
            update_legend();
        }
    }

    void activate(int i) {
        if (!items[i].enabled)
            return;
        if (items[i].act) {
            sfx(SFX_CONFIRM);
            auto act = items[i].act; // the page may rebuild under it
            act();
            if (s_result == PC_XN_MENU_STAY)
                refresh();
        } else if (items[i].id == "preset") {
            // A confirms the preset and goes on to its rules. Stepping to
            // the next one, as A does on a value, turned a chosen Doubles
            // into Free-for-all (Time, 2:00) just before Open lobby.
            move(PC_SCREEN_DOWN);
        } else if (items[i].change) {
            items[i].change(1);
            sfx(SFX_MOVE);
            refresh();
        }
    }

    void back() {
        sfx(SFX_BACK);
        switch (page) {
        case Page::Host:
            if (overlay) {
                settings_done(false); // B: cancel, nothing changes
                break;
            }
            s_result = PC_XN_MENU_LEAVE;
            break;
        case Page::Lobbies:
            s_result = PC_XN_MENU_LEAVE;
            break;
        case Page::More:
            go(Page::Host, "more");
            break;
        case Page::Stages:
            go(Page::Host, "slist");
            break;
        case Page::Fighters:
            go(Page::Host, "flist");
            break;
        case Page::Details: {
            // The same row, with no fetch (the list stays as it was).
            const std::string row = "lb" + std::to_string(detailsRow);
            go(Page::Lobbies, row.c_str());
            break;
        }
        case Page::Code:
            if (!code.empty() && !lan()) {
                code.pop_back();
                refresh();
            } else {
                to_lobbies();
            }
            break;
        case Page::Joining:
        case Page::Failed:
            pc_xn_punch_end();
            to_lobbies();
            break;
        }
    }

    void input(int what) {
        if (!shown || presence < 1.f)
            return;
        touched();
        switch (what) {
        case PC_SCREEN_UP:
        case PC_SCREEN_DOWN:
        case PC_SCREEN_LEFT:
        case PC_SCREEN_RIGHT:
            move(what);
            break;
        case PC_SCREEN_CONFIRM:
            if (!items.empty())
                activate(cursor);
            break;
        case PC_SCREEN_BACK:
            back();
            break;
        case PC_XN_IN_START:
            if (overlay) {
                if (!readOnly)
                    settings_done(true); // START: apply, from any of its pages
            } else if (page == Page::Host || page == Page::More || page == Page::Stages || page == Page::Fighters) {
                open_lobby();
                refresh();
            } else if (page == Page::Code) {
                join();
            } else if (page == Page::Lobbies) {
                showFull = !showFull;
                sfx(SFX_CONFIRM);
                browse();
                refresh();
            }
            break;
        case PC_XN_IN_X:
            if (page == Page::Lobbies && lan()) {
                sfx(SFX_CONFIRM);
                code = "@";
                go(Page::Code);
            } else if (page == Page::Lobbies) {
                sfx(SFX_CONFIRM);
                browse();
                refresh();
            } else if (page == Page::Code && lan()) {
                if (code.size() > 1) {
                    code.pop_back();
                    sfx(SFX_BACK);
                    refresh();
                }
            } else if (page == Page::Stages && !readOnly) {
                s_edit->stages = s_edit->stages == kCompetitive ? kAllStages : kCompetitive;
                s_edit->preset = kCustom;
                sfx(SFX_MOVE);
                refresh();
            } else if (page == Page::Fighters && !readOnly) {
                // All, High tiers, Low tiers, round again (a custom list goes to All).
                const unsigned f = s_edit->fighters;
                s_edit->fighters = f == XN_FIGHTERS_ALL  ? XN_FIGHTERS_HIGH
                                  : f == XN_FIGHTERS_HIGH ? XN_FIGHTERS_LOW
                                                          : XN_FIGHTERS_ALL;
                s_edit->preset = kCustom;
                sfx(SFX_MOVE);
                refresh();
            }
            break;
        case PC_XN_IN_Y:
            if (fighters_only_this()) {
                sfx(SFX_MOVE);
                refresh();
                break;
            }
            if (page == Page::Lobbies && !lan()) {
                sfx(SFX_CONFIRM);
                code.clear();
                go(Page::Code);
            }
            break;
        }
    }

    const char* hint() {
        if (!shown)
            return nullptr;
        if (overlay) {
            hintText = items.empty() || !items[cursor].enabled ? std::string() : items[cursor].hint;
            if (readOnly && page == Page::Host)
                hintText = tr("xeleenet.settings.hints.read_only");
            return hintText.c_str();
        }
        if (page == Page::Joining) {
            PcXnPunchView v;
            view(&v);
            const int n = std::max(0, v.nplayers - 1);
            hintText = n > 0 ? tr("xeleenet.joining.hints.connecting", {{"n", n}})
                             : tr("xeleenet.joining.hints.finding");
        } else if (page == Page::Failed) {
            hintText = tr("xeleenet.failed.hint");
        } else if (page == Page::Details) {
            hintText = details_hint();
        } else if (!hostNote.empty() && (page == Page::Host || page == Page::Lobbies)) {
            hintText = hostNote;
        } else if (page == Page::Lobbies && items.empty()) {
            hintText = tr(blocked() ? "xeleenet.lobbies.hints.blocked" : "xeleenet.lobbies.hints.empty");
        } else if (page == Page::Code) {
            hintText = tr(code_ready() ? "xeleenet.code.hints.join" : "xeleenet.code.hints.type");
        } else {
            hintText = items.empty() ? std::string() : items[cursor].hint;
        }
        return hintText.c_str();
    }

    const char* title() const {
        switch (page) {
        case Page::Host: return pc_tr("xeleenet.host.title");
        case Page::More: return pc_tr("xeleenet.rules.title");
        case Page::Stages: return pc_tr("xeleenet.stages.title");
        case Page::Fighters: return pc_tr("xeleenet.fighters.title");
        case Page::Lobbies: return pc_tr("xeleenet.lobbies.title");
        case Page::Details: return pc_tr("xeleenet.details.title");
        case Page::Code: return pc_tr("xeleenet.code.title");
        case Page::Joining:
        case Page::Failed: return pc_tr("xeleenet.joining.title");
        }
        return "";
    }

    void show(bool on) {
        if (on) {
            if (shown || !load())
                return;
            shown = true;
            if (overlay)
                s_settings_up = true;
            presence = 0.f;
            placedPresence = -1.f;
            document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            refresh();
            layout();
        } else if (shown) {
            shown = false;
            if (overlay)
                s_settings_up = false;
            document->Hide();
        }
    }

    // The screen scaled into the frame's inside, with the page's fade and rise.
    // The overlay's: centred over the whole (dimmed) CSS.
    void layout() {
        auto* context = aurora::rmlui::get_context();
        auto* screen = document->GetElementById("screen");
        if (context == nullptr || screen == nullptr)
            return;
        const Rml::Vector2i d = context->GetDimensions();
        dims = d;
        if (d == placedDims && presence == placedPresence)
            return;
        placedDims = d;
        placedPresence = presence;
        if (overlay) {
            // The legend where the CSS's sits; the frame centred in the room
            // above it.
            const float p = std::clamp(presence, 0.f, 1.f);
            const LegendPlace lp = css_legend_place(d);
            const float above = 0.025f * d.y, below = lp.top - 0.02f * d.y;
            const float scale = std::min(0.94f * d.x / kOverlayW, (below - above) / kOverlayH);
            screen->SetProperty("transform", "scale(" + fmt(scale) + ")");
            screen->SetProperty("left", fmt((d.x - kOverlayW * scale) / 2) + "px");
            screen->SetProperty("top",
                fmt(above + (below - above - kOverlayH * scale) / 2 + (1.f - p) * 10.f * scale) + "px");
            screen->SetProperty("opacity", fmt(p));
            if (auto* legend = document->GetElementById("legend")) {
                legend->SetProperty("transform", "scale(" + fmt(lp.scale) + ")");
                legend->SetProperty("left", fmt(lp.left) + "px");
                legend->SetProperty("top", fmt(lp.top) + "px");
                legend->SetProperty("opacity", fmt(p));
            }
            if (auto* dim = document->GetElementById("dim"))
                dim->SetProperty("opacity", fmt(p));
            return;
        }
        const float x0 = 0.123f * d.x, y0 = 0.170f * d.y;
        const float w = 0.755f * d.x, h = 0.672f * d.y;
        const float p = std::clamp(presence, 0.f, 1.f);
        const float ease = 1.f - (1.f - p) * (1.f - p);
        const float scale = std::min(w / kScreenW, h / kScreenH);
        screen->SetProperty("transform", "scale(" + fmt(scale) + ")");
        screen->SetProperty("left", fmt(x0 + (w - kScreenW * scale) / 2) + "px");
        screen->SetProperty("top", fmt(y0 + (h - kScreenH * scale) / 2 + (1.f - ease) * 10.f * scale) + "px");
        screen->SetProperty("opacity", fmt(ease));
    }

    static bool id_of(Rml::Element* e, std::string& out) {
        for (; e != nullptr; e = e->GetParentNode()) {
            const Rml::String& id = e->GetId();
            if (!id.empty() && id != "board" && id != "screen") {
                out = id;
                return true;
            }
        }
        return false;
    }

    void ProcessEvent(Rml::Event& event) override {
        if (!shown || presence < 1.f)
            return;
        std::string id;
        if (!id_of(event.GetTargetElement(), id))
            return;
        if (id == "full" && event.GetId() == Rml::EventId::Click && !pointerWas) {
            input(PC_XN_IN_START);
            return;
        }
        // The overlay over a session's CSS: the headset's laser gives no A
        // there (xr_pointer.c), so its clicks are the overlay's own.
        const bool click_ok = !pointerWas || (overlay && pc_net_active());
        int i = -1;
        for (size_t k = 0; k < items.size(); k++)
            if (items[k].id == id)
                i = static_cast<int>(k);
        if (i < 0 || !items[i].enabled)
            return;
        if (event.GetId() == Rml::EventId::Click && click_ok) {
            cursor = i;
            cursorId = id;
            activate(i);
        } else if (event.GetId() == Rml::EventId::Mouseover && i != cursor) {
            cursor = i;
            cursorId = id;
            sfx(SFX_MOVE);
            mark();
        }
    }

    void update() {
        if (page == Page::Joining && s_preview.empty()) {
            const int r = pc_xn_punch_poll();
            if (r < 0)
                failed();
            else if (r == 1 && s_result == PC_XN_MENU_STAY)
                s_result = PC_XN_MENU_JOINED;
        }
        poll_browse();
        // B1 refreshes every 10 s while it's open and someone's using it: a
        // game left on the list stops asking the service after kIdleMs (LAN
        // asks no service).
        if (browsing() && fetch != 0 && SDL_GetTicks() - fetchAsked > 10000 &&
            (lan() || SDL_GetTicks() - touchedAt < kIdleMs))
            browse();
        if (!shown)
            return;
        build();
        layout();
        if (overlay)
            update_hint();
    }
};

// The headset's laser as the RmlUi context's mouse, once a frame while any
// XeleeNet document is up (the pages, the CSS's banner and Lobby Settings,
// the stage select's): its press is a click. Whether it's on is told to
// those that care (`was`), and a press starts a page's idle clock again.
struct Pointer {
    bool was = false, pressed = false;
    void feed(bool any, Rml::Vector2i dims, const std::function<void()>& touched) {
#ifdef AURORA_ENABLE_OPENXR
        float px = 0.f, py = 0.f;
        bool down = false;
        auto* context = aurora::rmlui::get_context();
        if (any && context != nullptr && aurora_xr_screen_pointer(&px, &py, &down)) {
            context->ProcessMouseMove(static_cast<int>(px * dims.x), static_cast<int>(py * dims.y), 0);
            if (down && !pressed) {
                context->ProcessMouseButtonDown(0, 0);
                touched();
            } else if (!down && pressed)
                context->ProcessMouseButtonUp(0, 0);
            pressed = down;
            was = true;
        } else if (was && context != nullptr) {
            if (pressed)
                context->ProcessMouseButtonUp(0, 0);
            context->ProcessMouseLeave();
            was = pressed = false;
        }
#else
        (void)any, (void)dims, (void)touched;
#endif
    }
};
Pointer s_pointer;

// ---- C1: the lobby's banner in the CSS --------------------------------------------

// The CSS's Ready, as mncharsel.c last told it (pc_xn_css_frame), and when.
PcXnCssState s_css{};
uint64_t s_css_at = 0;
// Who left the lobby last (pc_xn_css_left), and when: the notice slot says so
// for a while, and for that long again once the CSS has reloaded without them
// (the next session takes a few seconds to agree).
std::string s_left_name;
uint64_t s_left_at = 0;
bool s_left_reload = false; // the reload without them hasn't come yet
constexpr uint64_t kLeftNoticeMs = 5000, kLeftReloadMs = 15000;
// The hold's notice shows from this many frames of it on.
constexpr int kHoldNotice = 10;

// This machine's other pads (docs/lan-multipad-plan.md): the notice a seat
// change gives (one at a time, for a few seconds), the door whose controller
// tag shows new, and what the last frame had, to tell what changed.
std::string s_seat_note, s_seat_note_kind;
uint64_t s_seat_note_at = 0;
int s_seat_new_port = -1;
uint64_t s_seat_new_at = 0;
unsigned s_prev_local = 0, s_prev_others = 0, s_prev_claimed = 0;
constexpr uint64_t kSeatNoteMs = 4000;
constexpr uint64_t kOffListNoteMs = 2000; // A on an off-list portrait
// F3: each port whose pick a reload dropped (off the new fighter list), the
// fighter, until it picks again; -1 none.
std::array<int, 4> s_dropped_ckind = {-1, -1, -1, -1};
uint64_t s_dropped_at = 0;
constexpr uint64_t kDroppedNoticeMs = 8000;

// The lobby's rules in force, as the CSS was last entered with them
// (pc_xn_css_rules), and the set before: C4's "Host changed" notice and the
// banner's bright value. Forgotten whenever the XeleeNet pages come up.
bool s_rules_known = false;
HostSettings s_rules_now, s_rules_prev;
uint64_t s_rules_changed_at = 0;
std::string s_rules_changed; // the notice's text
constexpr uint64_t kChangedNoticeMs = 6000, kLitMs = 4000;
// C3, on a guest's game: the host has Lobby Settings open (REL_XN_EDITING).
bool s_host_editing = false;
// A settings session on its way (the switch named, then its handshake), until
// the CSS reloads with it: "Applying the host's settings".
bool s_applying = false;
// The service refused the last Apply's seats: its line, for a while.
std::string s_refused;
uint64_t s_refused_at = 0;
constexpr uint64_t kRefusedMs = 6000;

bool settings_up();
void settings_open(bool host);
void settings_pad(unsigned buttons, int stick_x, int stick_y);
// The overlay's draft: what the host changes, or what a guest reads.
HostSettings s_draft;
void s_draft_set(bool host) {
    s_draft = host || !s_rules_known ? s_host : s_rules_now;
    if (!host) {
        PcXnListing info{};
        if (pc_xn_lobby_info(&info) && info.name[0])
            s_draft.name = info.name;
    }
    s_edit = &s_draft;
}

// The rules RULES put in force (gmonlinemode.c's PcXnRules) as the Host
// page's settings, for a guest's read-only overlay and the diffs.
HostSettings from_rules(const PcXnRules& r) {
    HostSettings h = s_host;
    h.seats = r.seats, h.teams = r.teams, h.stock = r.stock, h.stocks = r.stocks, h.minutes = r.minutes;
    h.items = r.items, h.friendly_fire = r.friendly_fire, h.handicap = r.handicap, h.damage = r.damage_ratio;
    h.pause = r.pause, h.stages = r.stage_mask, h.fighters = r.fighter_mask, h.frozen = r.frozen_stadium;
    return h;
}

// What changed from a to b, a line each ("Stocks 4 → 3"); `listing` adds
// the rows only the lobby list shows.
std::vector<std::string> rule_changes(const HostSettings& a, const HostSettings& b, bool listing) {
    std::vector<std::string> out;
    const auto add = [&](const char* row, const std::string& from, const std::string& to) {
        if (from != to)
            out.push_back(tr("xeleenet.settings.change", {{"row", tr(row)}, {"from", from}, {"to", to}}));
    };
    const auto dmg = [](int d) {
        char b[16];
        std::snprintf(b, sizeof b, "%.1f", d / 10.f);
        return tr("xeleenet.rules.values.damage", {{"n", b}});
    };
    add("xeleenet.host.rows.teams", on_off(a.teams), on_off(b.teams));
    if (a.stock != b.stock)
        add("xeleenet.rules.rows.win_by", tr(a.stock ? "xeleenet.rules.values.stock" : "xeleenet.rules.values.time"),
            tr(b.stock ? "xeleenet.rules.values.stock" : "xeleenet.rules.values.time"));
    else if (b.stock)
        add("xeleenet.host.rows.stocks", std::to_string(a.stocks), std::to_string(b.stocks));
    add("xeleenet.host.rows.time", minutes_label(a.minutes), minutes_label(b.minutes));
    add("xeleenet.host.rows.items", tr(kItemNames[a.items + 1]), tr(kItemNames[b.items + 1]));
    add("xeleenet.host.rows.ff", on_off(a.teams && a.friendly_fire), on_off(b.teams && b.friendly_fire));
    add("xeleenet.rules.rows.handicap", tr(kHandicapNames[a.handicap]), tr(kHandicapNames[b.handicap]));
    add("xeleenet.rules.rows.damage", dmg(a.damage), dmg(b.damage));
    add("xeleenet.rules.rows.pause", on_off(a.pause), on_off(b.pause));
    add("xeleenet.host.rows.fighter_list", fighter_list_label(a.fighters), fighter_list_label(b.fighters));
    add("xeleenet.host.rows.stage_list", stage_list_label(a.stages), stage_list_label(b.stages));
    add("xeleenet.host.rows.frozen", on_off(a.frozen), on_off(b.frozen));
    add("xeleenet.host.rows.seats", std::to_string(a.seats), std::to_string(b.seats));
    if (listing && !lan()) {
        add("xeleenet.host.rows.visibility", tr(a.open ? "xeleenet.host.values.public" : "xeleenet.host.values.private"),
            tr(b.open ? "xeleenet.host.values.public" : "xeleenet.host.values.private"));
        add("xeleenet.host.rows.hide_code", on_off(a.hide_code), on_off(b.hide_code));
    }
    return out;
}

// A fighter's name by CharacterKind (Sheik as Zelda / Sheik's entry).
std::string fighter_name_of(int ckind) {
    const int k = ckind == XN_FIGHTER_SHEIK ? XN_FIGHTER_ZELDA : ckind;
    for (int i = 0; i < XN_FIGHTER_ENTRIES; i++)
        if (xn_fighter_order[i] == k)
            return fighter_name(i);
    return "";
}

// The name port q's player goes by on the doors, "" when nobody plays it.
std::string port_name(int q) {
    PcXnPunchView v;
    view(&v);
    for (int i = 0; i < v.nplayers; i++)
        if (v.players[i].port == q) {
            const std::string n = v.players[i].name;
            const size_t hash = n.find('#');
            return hash == std::string::npos ? n : n.substr(0, hash);
        }
    return "";
}

// The CSS's doors: the fighter port q holds (a CharacterKind), -1 for none.
int held_by(int q) {
    if (q < 0 || q > 3 || !(s_css.humans & s_css.picked & (1u << q)) || s_css.ckind[q] >= 0x1A)
        return -1;
    return s_css.ckind[q];
}

void seat_note(const std::string& kind, const std::string& text) {
    s_seat_note_kind = kind;
    s_seat_note = text;
    s_seat_note_at = SDL_GetTicks();
}

// The name a player goes by on the doors: "NAME" of "NAME#TAG".
std::string short_name(const char* name) {
    std::string n = name;
    const size_t hash = n.find('#');
    return hash == std::string::npos ? n : n.substr(0, hash);
}

// What changed in this machine's seats and the others' since the last frame.
void seat_changes(const PcXnPunchView& v, int seats) {
    const unsigned local = unsigned(pc_net_local_mask());
    unsigned others = 0;
    for (int i = 0; i < v.nplayers; i++)
        if (v.players[i].owner != v.players[i].port && !v.players[i].local)
            others |= 1u << v.players[i].port;
    if (pc_xn_punch_claim_refused() && s_prev_claimed != 0)
        pc_xn_css_seat_full(__builtin_ctz(s_prev_claimed));
    s_prev_claimed = s_css.claimed;
    if (local == 0)
        return; // between sessions
    if (s_prev_local != 0) {
        for (int q = 0; q < 4; q++) {
            const unsigned bit = 1u << q;
            if ((local & ~s_prev_local & bit) && q != pc_net_local_player()) {
                seat_note("gold", tr("xeleenet.css.seat_mine", {{"n", pc_net_port_phys(q) + 1}, {"p", q + 1}}));
                s_seat_new_port = q;
                s_seat_new_at = SDL_GetTicks();
            }
            // Another machine's new seat: named after its machine, as the
            // lobby tells it (the engine may not have switched yet).
            for (int i = 0; i < v.nplayers && (others & ~s_prev_others & bit); i++)
                if (v.players[i].port == q)
                    for (int j = 0; j < v.nplayers; j++)
                        if (v.players[j].port == v.players[i].owner)
                            seat_note("info", tr("xeleenet.css.seat_added",
                                                 {{"name", short_name(v.players[j].name)}, {"p", q + 1}}));
        }
    }
    (void)seats;
    s_prev_local = local;
    s_prev_others = others;
}


// The banner's rules line, its parts lit (bright) where `lit` has their bit:
// 1 the mode, 2 stocks or time, 4 the time limit, 8 items, 16 the fighters.
std::string rules_line_rml(const HostSettings& h, unsigned lit) {
    const auto part = [&](unsigned bit, const std::string& text) {
        return (lit & bit) ? "<span class=\"lit\">" + enc(text) + "</span>" : enc(text);
    };
    std::string r = part(1, tr(h.teams        ? "xeleenet.rules_line.doubles"
                               : h.seats == 2 ? "xeleenet.rules_line.1v1"
                                              : "xeleenet.rules_line.ffa"));
    r += enc(kDot) + part(2, h.stock ? tr("xeleenet.rules_line.stocks", {{"n", h.stocks}}) : tr("xeleenet.rules_line.time"));
    r += enc(kDot) + part(4, minutes_label(h.minutes));
    if (h.items >= 0)
        r += enc(kDot) + part(8, tr("xeleenet.rules_line.items"));
    const std::string f = fighter_list_short(fighter_list_id(h.fighters));
    if (!f.empty() || (lit & 16))
        r += enc(kDot) + part(16, f.empty() ? fighter_list_label(h.fighters) : f);
    return r;
}

// Which of the rules line's parts differ between two sets (rules_line_rml).
unsigned rules_line_diff(const HostSettings& a, const HostSettings& b) {
    unsigned m = 0;
    if (a.teams != b.teams || (a.seats == 2) != (b.seats == 2))
        m |= 1;
    if (a.stock != b.stock || (b.stock && a.stocks != b.stocks))
        m |= 2;
    if (a.minutes != b.minutes)
        m |= 4;
    if ((a.items >= 0) != (b.items >= 0))
        m |= 8;
    if (a.fighters != b.fighters)
        m |= 16;
    return m;
}

struct Banner : Rml::EventListener {
    Rml::ElementDocument* document = nullptr;
    bool shown = false;
    std::string last, lastDoors;
    bool pointerWas = false;

    void show(bool on) {
        if (on && document == nullptr) {
            auto* context = aurora::rmlui::get_context();
            if (context == nullptr)
                return;
            load_fonts();
            document = context->LoadDocument(std::string(pc_resources_dir()) + "xeleenet-banner.rml");
            if (document == nullptr) {
                pc_log_line("xeleenet: could not load xeleenet-banner.rml");
                return;
            }
            document->AddEventListener(Rml::EventId::Click, this);
        }
        if (document == nullptr || on == shown)
            return;
        shown = on;
        last.clear();
        if (on)
            document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        else
            document->Hide();
    }

    // A click on the bar (a mouse, or the headset's laser in a session, where
    // it gives no A) opens Lobby Settings, as A on it does: the host's to
    // change, a guest's to read. Not while Ready or counting down.
    void ProcessEvent(Rml::Event& event) override {
        if (!shown || event.GetId() != Rml::EventId::Click || settings_up() || (pointerWas && !pc_net_active()))
            return;
        bool on_bar = false;
        for (Rml::Element* e = event.GetTargetElement(); e != nullptr && !on_bar; e = e->GetParentNode())
            on_bar = e->GetId() == "bar";
        const int me = pc_net_active() ? pc_net_local_player() : 0;
        const bool css = s_css_at != 0 && SDL_GetTicks() - s_css_at < 500;
        if (!on_bar || !css || me < 0 || me > 3 || (s_css.ready & (1u << me)) || s_css.countdown > 0)
            return;
        pc_log_line("xeleenet: the banner clicked");
        settings_open(me == 0);
    }

    void update() {
        if (!shown)
            return;
        auto* context = aurora::rmlui::get_context();
        auto* bar = document->GetElementById("bar");
        if (context == nullptr || bar == nullptr)
            return;
        // Over the CSS's top strip: Melee's logo and its rules bar.
        const Rml::Vector2i d = context->GetDimensions();
        const float scale = 0.072f * d.y / 72.f;
        bar->SetProperty("transform", "scale(" + fmt(scale) + ")");
        bar->SetProperty("left", fmt(0.07f * d.x) + "px");
        bar->SetProperty("top", fmt(0.062f * d.y) + "px");
        bar->SetProperty("width", fmt(0.86f * d.x / scale) + "px");
        PcXnPunchView v;
        view(&v);
        PcXnListing info{};
        const bool known = pc_xn_lobby_info(&info);
        const bool host = v.host;
        const std::string name = host ? lobby_name() : known ? std::string(info.name) : tr("xeleenet.banner.lobby");
        // The rules in force (RULES, every player's the same), the parts a
        // change just moved lit for a few seconds; before any, the host's own
        // or a guest's listing.
        std::string rules_all;
        if (s_rules_known) {
            const bool lit = s_rules_changed_at != 0 && SDL_GetTicks() - s_rules_changed_at < kLitMs;
            rules_all = rules_line_rml(s_rules_now, lit ? rules_line_diff(s_rules_prev, s_rules_now) : 0u);
        } else {
            const std::string rules =
                host ? rules_line(s_host.teams, s_host.seats, s_host.stock, s_host.stocks, s_host.minutes,
                                  kItemIds[s_host.items + 1])
                     : known ? rules_line(info.teams, info.seats, info.stock, info.stocks, info.time_min, info.items) : "";
            // The fighter list, when it isn't All.
            const std::string fighters =
                fighter_list_short(host ? fighter_list_id(s_host.fighters) : known ? info.fighter_list : "");
            rules_all = enc(fighters.empty() || rules.empty() ? rules : rules + kDot + fighters);
        }
        std::string code = v.code[0] ? std::string(v.code, 3) + " " + std::string(v.code + 3, 3) : "";
        char addr[64];
        if (lan()) // the host's address, for Join by address, where the code sits
            code = s_preview == "lan-banner" ? "192.168.0.57" : pc_xn_lan_address(addr, sizeof addr) ? addr : "";
        else if (host && s_host.hide_code)
            code = "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 \xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2";
        const int seats = lobby_seats(host);
        // The CSS's Ready, while it keeps telling it.
        const bool css = s_css_at != 0 && SDL_GetTicks() - s_css_at < 500;
        const auto bits = [](unsigned m) { return __builtin_popcount(m & 15u); };
        std::string count = "<div class=\"seats\">" + std::to_string(std::max(1, v.nplayers)) + "/" +
                            std::to_string(seats) + "</div>";
        if (css && s_css.humans != 0) {
            const bool all = (s_css.ready & s_css.humans) == s_css.humans;
            count = "<div class=\"rpill" + std::string(all ? " all" : "") + "\">" +
                    enc(tr("xeleenet.css.ready_count",
                           {{"n", bits(s_css.ready & s_css.humans)}, {"m", bits(s_css.humans)}})) +
                    "</div>";
        }
        // The notice slot, under the banner: our own hold toward leaving, the
        // countdown, who's waited on, or who left.
        std::string notice;
        if (css)
            seat_changes(v, seats);
        const unsigned local = css ? s_css.local : 0u;
        const int me = pc_net_local_player();
        if (css && s_css.hold >= kHoldNotice) {
            // Another seat of ours gives up only itself; our own takes them all.
            std::string text = tr("xeleenet.css.holding");
            const int hp = s_css.hold_port;
            if (hp >= 0 && hp < 4 && hp != me && (local & (1u << hp))) {
                text = tr("xeleenet.css.holding_seat", {{"p", hp + 1}, {"n", pc_net_port_phys(hp) + 1}});
            } else if (local & ~(1u << me) & 15u) {
                std::string names;
                for (int i = 0; i < v.nplayers; i++)
                    if (v.players[i].local && v.players[i].port != me)
                        names += (names.empty() ? "" : ", ") + short_name(v.players[i].name);
                text = tr("xeleenet.css.holding_all", {{"names", names}});
            }
            notice = "<div class=\"notice red\">" + enc(text) + "</div>";
        } else if (css && s_css.countdown > 0) {
            const int secs = (s_css.countdown + 59) / 60;
            const float left = s_css.countdown_total > 0 ? float(s_css.countdown) / s_css.countdown_total : 0.f;
            notice = "<div class=\"notice gold\">" + enc(tr("xeleenet.css.countdown", {{"n", secs}})) +
                     "<div class=\"drain\" style=\"width: " + fmt(100.f * left) + "%;\"></div></div>";
        } else if (const int lost = css ? dropped_mine() : -1; lost >= 0) {
            notice = "<div class=\"notice red\">" +
                     enc(tr("xeleenet.css.dropped", {{"name", fighter_name_of(s_dropped_ckind[lost])}})) + "</div>";
        } else if (css && s_css.waiting != 0) {
            std::string who;
            for (int i = 0; i < v.nplayers; i++)
                if (s_css.waiting & (1u << v.players[i].port))
                    who = v.players[i].name;
            const size_t hash = who.find('#');
            notice = "<div class=\"notice warn\">" +
                     enc(tr("xeleenet.css.waiting_on", {{"name", hash == std::string::npos ? who : who.substr(0, hash)}})) +
                     "</div>";
        } else if (!s_refused.empty() && SDL_GetTicks() - s_refused_at < kRefusedMs) {
            notice = "<div class=\"notice warn\">" + enc(tr("xeleenet.css.refused", {{"why", s_refused}})) + "</div>";
        } else if (s_applying) {
            notice = "<div class=\"notice info\">" + enc(tr("xeleenet.css.applying")) + "</div>";
        } else if (s_rules_changed_at != 0 && SDL_GetTicks() - s_rules_changed_at < kChangedNoticeMs) {
            notice = "<div class=\"notice info\">" + enc(s_rules_changed) + "</div>";
        } else if (s_host_editing && !host) {
            notice = "<div class=\"notice info\">" + enc(tr("xeleenet.css.editing")) + "</div>";
        } else if (s_seat_note_at != 0 && SDL_GetTicks() - s_seat_note_at < kSeatNoteMs) {
            notice = "<div class=\"notice " + s_seat_note_kind + "\">" + enc(s_seat_note) + "</div>";
        } else if (s_left_at != 0 && SDL_GetTicks() - s_left_at < kLeftNoticeMs) {
            notice = "<div class=\"notice info\">" + enc(tr("xeleenet.css.left", {{"name", s_left_name}})) + "</div>";
        }
        const std::string r =
            "<div class=\"brand\"><span>" + enc(tr("xeleenet.banner.brand")) +
            "</span></div><div class=\"pill" + std::string(css && s_css.hot ? " hot" : "") +
            "\"><div class=\"names\"><div class=\"bn\">" + enc(name) + "</div><div class=\"br\">" + rules_all +
            "</div></div>" + count +
            (code.empty() ? "" : "<div class=\"code\">" + enc(tr("xeleenet.banner.code", {{"code", code}})) + "</div>") +
            "</div><div class=\"nslot\">" + notice + "</div>";
        if (r != last) {
            last = r;
            bar->SetInnerRML(r);
            bar->SetClass("lan", lan()); // a shorter brand
        }
        update_doors(d, css);
    }

    // F3: a port whose pick a reload dropped and that hasn't picked again.
    static bool dropped_open(int q) {
        if (q < 0 || q > 3 || s_dropped_ckind[q] < 0)
            return false;
        if (s_css_at <= s_dropped_at)
            return true; // the reloaded CSS hasn't said who picked yet
        if ((s_css.picked & (1u << q)) || !(s_css.humans & (1u << q))) {
            s_dropped_ckind[q] = -1; // picked again (or gone)
            return false;
        }
        return true;
    }

    // One of ours of those, for the red notice (for a while), else -1.
    static int dropped_mine() {
        if (s_dropped_at == 0 || SDL_GetTicks() - s_dropped_at > kDroppedNoticeMs)
            return -1;
        for (int q = 0; q < 4; q++)
            if ((s_css.local & (1u << q)) && dropped_open(q))
                return q;
        return -1;
    }

    // Each door's tag, and the legend above the doors, in window pixels (laid
    // out at 720 tall and scaled).
    void update_doors(const Rml::Vector2i d, bool css) {
        auto* doors = document->GetElementById("doors");
        if (doors == nullptr)
            return;
        std::string r;
        // Lobby Settings covers the doors: their tags and the legend wait.
        if (css && !s_settings_up) {
            const float k = d.y / 720.f;
            // The lobby's seats' doors stand centred (mncharsel.c moves them).
            const int seats = s_css.seats >= 2 && s_css.seats <= 4 ? s_css.seats : 4;
            const float shift = (4 - seats) * kDoorPitch / 2;
            // The first open seat a spare pad of ours can take (docs/lan-multipad-plan.md).
            int open = -1;
            for (int i = 0; i < seats && open < 0; i++)
                if (!(s_css.humans & (1u << i)))
                    open = i;
            // Our doors carry their controller's number once we play more
            // than one, or a spare pad could join.
            const bool tags = __builtin_popcount(s_css.local & 15u) > 1 || s_css.spare >= 0;
            for (int i = 0; i < 4; i++) {
                const unsigned bit = 1u << i;
                const float x = css_zoom(kDoorX0 + i * kDoorPitch + shift) * d.x, w = kDoorW * PC_XN_CSS_ZOOM * d.x;
                const float y = css_zoom(kDoorY0) * d.y, h = kDoorH * PC_XN_CSS_ZOOM * d.y;
                if (i == open && (s_css.spare >= 0 || s_css.claimed != 0)) {
                    const bool adding = s_css.claimed != 0;
                    const int pad = (adding ? __builtin_ctz(s_css.claimed) : s_css.spare) + 1;
                    r += "<div class=\"door spare\" style=\"left: " + fmt(x) + "px; top: " + fmt(y) + "px; width: " +
                         fmt(w) + "px; height: " + fmt(h) + "px; font-size: " + fmt(15.f * k) + "px;\">" +
                         "<div class=\"st\" style=\"margin-top: " + fmt(0.38f * h) + "px;\">" +
                         enc(tr("xeleenet.css.spare_title", {{"n", pad}})) + "</div><div class=\"sl\" style=\"font-size: " +
                         fmt(12.f * k) + "px;\">" +
                         enc(tr(adding ? "xeleenet.css.spare_adding" : "xeleenet.css.spare_line")) + "</div>" +
                         (adding ? "" : "<div class=\"sk\" style=\"font-size: " + fmt(9.f * k) + "px;\">START</div>") +
                         "</div>";
                    continue;
                }
                if (!(s_css.humans & bit))
                    continue;
                std::string tag;
                // Another machine's other pad: whose machine it plays from.
                const int owner = pc_net_port_owner(i);
                if (owner >= 0 && owner != i && !(s_css.local & bit)) {
                    PcXnPunchView pv;
                    view(&pv);
                    for (int j = 0; j < pv.nplayers; j++)
                        if (pv.players[j].port == owner)
                            tag += "<div class=\"owner\" style=\"top: " + fmt(0.62f * h) + "px; font-size: " +
                                   fmt(11.f * k) + "px;\">" +
                                   enc(tr("xeleenet.css.tag_owner", {{"name", short_name(pv.players[j].name)}})) +
                                   "</div>";
                }
                if (tags && (s_css.local & bit)) {
                    const bool fresh = i == s_seat_new_port && SDL_GetTicks() - s_seat_new_at < kSeatNoteMs;
                    tag += "<div class=\"ctag" + std::string(fresh ? " new" : "") + "\" style=\"top: " + fmt(0.16f * h) +
                           "px; left: " + fmt(0.066f * w) + "px; font-size: " + fmt(10.f * k) + "px; height: " +
                           fmt(18.f * k) + "px; line-height: " + fmt(16.f * k) + "px;\">" +
                           enc(tr("xeleenet.css.tag_controller", {{"n", pc_net_port_phys(i) + 1}})) + "</div>";
                }
                if (s_css.ready & bit) {
                    // At the card's foot, inside its frame (3 px, as at its sides).
                    tag += "<div class=\"ribbon\" style=\"bottom: 3px; height: " + fmt(0.10f * h) +
                          "px; line-height: " + fmt(0.10f * h) + "px; font-size: " + fmt(18.f * k) + "px;\">" +
                          enc(tr("xeleenet.css.tag_ready")) + "</div>";
                } else if (s_css.waiting & bit) {
                    tag += "<div class=\"dash\"></div><div class=\"tag warn\" style=\"bottom: " + fmt(0.1f * w) +
                          "px; font-size: " + fmt(11.f * k) + "px; height: " + fmt(22.f * k) + "px; line-height: " +
                          fmt(20.f * k) + "px;\">" + enc(tr("xeleenet.css.tag_waiting")) + "</div>";
                } else if (i == 0 && s_host_editing && !(s_css.local & bit)) {
                    // C3: the host has Lobby Settings open.
                    tag += "<div class=\"tag info\" style=\"bottom: " + fmt(0.1f * w) + "px; font-size: " + fmt(11.f * k) +
                           "px; height: " + fmt(22.f * k) + "px; line-height: " + fmt(20.f * k) + "px;\">" +
                           enc(tr("xeleenet.css.tag_editing")) + "</div>";
                } else if (dropped_open(i)) {
                    // F3: their fighter came off the list; they pick again.
                    tag += "<div class=\"tag warn\" style=\"bottom: " + fmt(0.1f * w) + "px; font-size: " + fmt(11.f * k) +
                           "px; height: " + fmt(22.f * k) + "px; line-height: " + fmt(20.f * k) + "px;\">" +
                           enc(tr("xeleenet.css.tag_choosing")) + "</div>";
                } else if (s_css.picked & bit) {
                    // Off the foot as far as off the sides (10% of the card's width).
                    tag += "<div class=\"tag\" style=\"bottom: " + fmt(0.1f * w) + "px; font-size: " + fmt(11.f * k) +
                          "px; height: " + fmt(22.f * k) + "px; line-height: " + fmt(20.f * k) + "px;\">" +
                          enc(tr("xeleenet.css.tag_start")) + "</div>";
                }
                r += "<div class=\"door\" style=\"left: " + fmt(x) + "px; top: " + fmt(y) + "px; width: " + fmt(w) +
                     "px; height: " + fmt(h) + "px;\">" + tag + "</div>";
            }
            // The legend: what this player's buttons do now.
            const int me = pc_net_local_player();
            const unsigned mine = me >= 0 && me < 4 ? 1u << me : 0u;
            (void)open;
            std::string keys;
            if (s_css.countdown > 0)
                keys = legend_key("b", "xeleenet.legend.cancel");
            else if (s_css.ready & mine)
                keys = legend_key("b", "xeleenet.legend.not_ready") + legend_key("b", "xeleenet.legend.hold_leave");
            else
                keys = legend_key("start", "xeleenet.legend.ready") + legend_key("z", "xeleenet.legend.settings") +
                       legend_key("b", "xeleenet.legend.hold_leave");
            // The ring around this player's hand while they hold B.
            if (s_css.hold > 0 && s_css.hold_total > 0 && s_css.hand_ok) {
                constexpr int kDots = 24;
                const float cx = s_css.hand_x * d.x, cy = s_css.hand_y * d.y;
                const float rad = 46.f * k, dot = 8.f * k;
                const int on = kDots * s_css.hold / s_css.hold_total;
                for (int j = 0; j < kDots; j++) {
                    const float a = 6.2831853f * j / kDots;
                    r += "<div class=\"rdot" + std::string(j < on ? " on" : "") + "\" style=\"left: " +
                         fmt(cx + rad * std::sin(a) - dot / 2) + "px; top: " + fmt(cy - rad * std::cos(a) - dot / 2) +
                         "px; width: " + fmt(dot) + "px; height: " + fmt(dot) + "px; border-radius: " + fmt(dot / 2) +
                         "px;\"></div>";
                }
            }
            // Below the doors, centred in the room the zoom leaves.
            const LegendPlace lp = css_legend_place(d);
            r += "<div id=\"clegend\" style=\"transform: scale(" + fmt(lp.scale) + "); left: " + fmt(lp.left) +
                     "px; top: " + fmt(lp.top) + "px;\"><div class=\"keys\">" + keys + "</div></div>";
        }
        if (r != lastDoors) {
            lastDoors = r;
            doors->SetInnerRML(r);
        }
    }
};

// ---- the stage select's overlay: the list's name, the legend, the picks
// card, the picks outlined on the grid, and the draw (P5)

// The icons' pictures (pc_xn_sss_thumb), decoded once and served to RmlUi as
// xnstage://<cell>.
using Thumb = Picture;
std::array<Thumb, 32> s_thumbs;

// A pick's ring round its icon (xnring://W_H_T_C_BEVELS_RRGGBB): the frame's
// shape, a rectangle with the corners in BEVELS (bit 0 top left, then
// clockwise) cut at 45 degrees by C px, as a line T px wide, antialiased.
// RmlUi has no polygons; a texture per size is what keeps it crisp.
struct Ring {
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
};
std::vector<std::pair<std::string, Ring>> s_rings;

std::optional<aurora::rmlui::RuntimeTexture> ring_texture(std::string_view source) {
    const std::string key(source);
    for (const auto& [k, r] : s_rings)
        if (k == key) {
            aurora::rmlui::RuntimeTexture t;
            t.width = r.w, t.height = r.h, t.rgba8 = std::as_bytes(std::span(r.rgba));
            return t;
        }
    int w = 0, h = 0, bevels = 0;
    float th = 0, c = 0;
    unsigned rgb = 0;
    const size_t at = key.find("://");
    if (at == std::string::npos ||
        std::sscanf(key.c_str() + at + 3, "%d_%d_%f_%f_%d_%x", &w, &h, &th, &c, &bevels, &rgb) != 6 || w <= 0 ||
        h <= 0 || w > 2048 || h > 2048)
        return std::nullopt;
    Ring r;
    r.w = uint32_t(w), r.h = uint32_t(h);
    r.rgba.assign(size_t(w) * h * 4, 0);
    // Signed distance outside a convex shape, inset by `in`: the furthest of
    // its edges' half-planes (the corner cuts' normals are diagonal).
    const float hw = w / 2.f, hh = h / 2.f, s2 = 0.70710678f;
    const auto dist = [&](float x, float y, float in) {
        float d = std::max(std::fabs(x) - (hw - in), std::fabs(y) - (hh - in));
        for (int k = 0; k < 4; k++) {
            if (!(bevels & (1 << k)))
                continue;
            const float sx = k == 0 || k == 3 ? -1.f : 1.f, sy = k < 2 ? -1.f : 1.f;
            d = std::max(d, (sx * x + sy * y - (hw + hh - c)) * s2 + in);
        }
        return d;
    };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float px = x + 0.5f - hw, py = y + 0.5f - hh;
            const float outer = std::clamp(0.5f - dist(px, py, 0.f), 0.f, 1.f);
            const float inner = std::clamp(0.5f + dist(px, py, th), 0.f, 1.f);
            uint8_t* o = &r.rgba[(size_t(y) * w + x) * 4];
            o[0] = uint8_t(rgb >> 16), o[1] = uint8_t(rgb >> 8), o[2] = uint8_t(rgb);
            o[3] = uint8_t(255.f * outer * inner + 0.5f);
        }
    if (s_rings.size() > 64)
        s_rings.erase(s_rings.begin());
    s_rings.emplace_back(key, std::move(r));
    const Ring& kept = s_rings.back().second;
    aurora::rmlui::RuntimeTexture t;
    t.width = kept.w, t.height = kept.h, t.rgba8 = std::as_bytes(std::span(kept.rgba));
    return t;
}

// A GX palette entry (big-endian): IA8, RGB565 or RGB5A3.
std::array<uint8_t, 4> tlut_rgba(const uint8_t* e, int fmt) {
    const unsigned v = unsigned(e[0]) << 8 | e[1];
    switch (fmt) {
    case 0: // IA8: alpha, then intensity
        return {e[1], e[1], e[1], e[0]};
    case 1: // RGB565
        return {uint8_t((v >> 11 & 31) * 255 / 31), uint8_t((v >> 5 & 63) * 255 / 63), uint8_t((v & 31) * 255 / 31),
                255};
    default: // RGB5A3
        if (v & 0x8000)
            return {uint8_t((v >> 10 & 31) * 255 / 31), uint8_t((v >> 5 & 31) * 255 / 31),
                    uint8_t((v & 31) * 255 / 31), 255};
        return {uint8_t((v >> 8 & 15) * 17), uint8_t((v >> 4 & 15) * 17), uint8_t((v & 15) * 17),
                uint8_t((v >> 12 & 7) * 255 / 7)};
    }
}

// A GX texture into RGBA: the formats Melee's menus use (GXTexFmt: I4 0, I8
// 1, IA4 2, IA8 3, RGB565 4, RGB5A3 5, RGBA8 6, C4 8, C8 9, CMPR 14), each
// laid out in blocks of 32 bytes (RGBA8's 64), row by row. The palette is
// for C4 and C8. False for any other format.
bool decode_gx(const uint8_t* src, int fmt, int w, int h, const uint8_t* lut, int lut_fmt, int entries,
               Picture& out) {
    const int bw = fmt == 0 || fmt == 8 || fmt == 14 ? 8 : fmt == 1 || fmt == 2 || fmt == 9 ? 8 : 4;
    const int bh = fmt == 0 || fmt == 8 || fmt == 14 ? 8 : 4;
    if (src == nullptr || w <= 0 || h <= 0 || fmt == 7 || (fmt > 9 && fmt != 14) ||
        ((fmt == 8 || fmt == 9) && (lut == nullptr || entries <= 0)))
        return false;
    out.w = uint32_t(w), out.h = uint32_t(h);
    out.rgba.assign(size_t(w) * h * 4, 0);
    const auto put = [&](int x, int y, std::array<uint8_t, 4> c) {
        if (x < w && y < h)
            std::memcpy(&out.rgba[(size_t(y) * w + x) * 4], c.data(), 4);
    };
    const auto pal = [&](int e) {
        return e < entries ? tlut_rgba(lut + 2 * e, lut_fmt) : std::array<uint8_t, 4>{0, 0, 0, 0};
    };
    const uint8_t* p = src;
    for (int by = 0; by < h; by += bh)
        for (int bx = 0; bx < w; bx += bw) {
            if (fmt == 14) { // four DXT1 blocks of 4x4, in Z order, big-endian
                for (int s = 0; s < 4; s++, p += 8) {
                    const unsigned c0 = unsigned(p[0]) << 8 | p[1], c1 = unsigned(p[2]) << 8 | p[3];
                    std::array<std::array<uint8_t, 4>, 4> c;
                    c[0] = tlut_rgba(p, 1), c[1] = tlut_rgba(p + 2, 1);
                    for (int k = 0; k < 3; k++) {
                        c[2][k] = uint8_t(c0 > c1 ? (2 * c[0][k] + c[1][k]) / 3 : (c[0][k] + c[1][k]) / 2);
                        c[3][k] = uint8_t(c0 > c1 ? (c[0][k] + 2 * c[1][k]) / 3 : 0);
                    }
                    c[2][3] = 255, c[3][3] = c0 > c1 ? 255 : 0;
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++)
                            put(bx + (s & 1) * 4 + x, by + (s >> 1) * 4 + y, c[(p[4 + y] >> (6 - 2 * x)) & 3]);
                }
                continue;
            }
            for (int i = 0; i < bw * bh; i++) {
                const int x = bx + i % bw, y = by + i / bw;
                const uint8_t nib = i & 1 ? p[i / 2] & 15 : p[i / 2] >> 4;
                switch (fmt) {
                case 0: put(x, y, {uint8_t(nib * 17), uint8_t(nib * 17), uint8_t(nib * 17), 255}); break;
                case 1: put(x, y, {p[i], p[i], p[i], 255}); break;
                case 2:
                    put(x, y, {uint8_t((p[i] & 15) * 17), uint8_t((p[i] & 15) * 17), uint8_t((p[i] & 15) * 17),
                               uint8_t((p[i] >> 4) * 17)});
                    break;
                case 3: put(x, y, tlut_rgba(p + 2 * i, 0)); break;
                case 4: put(x, y, tlut_rgba(p + 2 * i, 1)); break;
                case 5: put(x, y, tlut_rgba(p + 2 * i, 2)); break;
                case 6: put(x, y, {p[2 * i + 1], p[32 + 2 * i], p[33 + 2 * i], p[2 * i]}); break;
                case 8: put(x, y, pal(nib)); break;
                case 9: put(x, y, pal(p[i])); break;
                }
            }
            p += fmt == 6 ? 64 : 32;
        }
    return true;
}

std::string stage_name_key(int id) {
    for (const Stage& st : kStages)
        if (st.bit == id)
            return st.name;
    return "";
}

// The picks card's bottom-left corner, as fractions of the game's picture
// (the stage select drawn at PC_XN_SSS_ZOOM): as far in from the frame's
// inner lines on the left as on the bottom. Measured with Brinstar Depths'
// icon, whose bottom a card of three or four rows passes: those are narrow
// enough to end left of it. Rows are kRowH apart and grid tiles kTileW by
// kTileH, at 720 tall.
constexpr float kCardLeft = 0.13f, kCardBottom = 0.8365f;
constexpr float kRowH = 58.f, kTileW = 84.f, kTileH = 73.f;
// An icon frame's corner cut, as a fraction of its width (measured).
constexpr float kFrameCut = 0.07f;

std::string short_name(const std::string& n) {
    const size_t hash = n.find('#');
    return hash == std::string::npos ? n : n.substr(0, hash);
}

void register_sss_textures();

// ---- Lobby Settings: the overlay over the CSS (docs/xeleenet-settings-overlay-plan.md)

Screen s_over;
uint64_t s_over_at = 0; // when it opened, for its fade

bool settings_up() { return s_over.shown; }

// The fewest seats the lobby can go down to: the ports in use, our pads
// waiting for one, and those still linking.
int settings_seats_min() {
    PcXnPunchView v;
    view(&v);
    int claims[2];
    const int n = v.nplayers + pc_xn_punch_claims(claims);
    return std::clamp(n, 2, 4);
}

// LINKS: which pairs of guests go through the host's game ("All direct").
std::vector<std::string> links_lines() {
    PcXnPunchView v;
    view(&v);
    std::vector<std::string> out;
    for (int a = 1; a < 4; a++)
        for (int b = a + 1; b < 4; b++) {
            const int da = pc_xn_punch_direct(a);
            if (da < 0 || pc_xn_punch_direct(b) < 0 || (da & (1 << b)))
                continue;
            out.push_back(tr(v.host ? "xeleenet.settings.links.via_you" : "xeleenet.settings.links.via_host",
                             {{"a", port_name(a)}, {"b", port_name(b)}}));
        }
    if (out.empty())
        out.push_back(tr(v.nplayers > 1 ? "xeleenet.settings.links.direct" : "xeleenet.settings.links.alone"));
    return out;
}

// What Apply would do: the changes, then what they do to the players.
std::string changes_box(const HostSettings& was, const HostSettings& now) {
    const std::vector<std::string> lines = rule_changes(was, now, true);
    std::string r = "<div class=\"ch\">" + enc(tr("xeleenet.settings.changes")) + "</div>";
    if (lines.empty())
        return r + "<div class=\"cl none\">" + enc(tr("xeleenet.settings.effects.none")) + "</div>";
    std::string list;
    for (size_t i = 0; i < lines.size(); i++)
        list += (i ? "; " : "") + lines[i];
    r += "<div class=\"cl\">" + enc(list) + "</div>";
    if (plays_same(was, now))
        return r + "<div class=\"ce\">" + enc(tr("xeleenet.settings.effects.listing")) + "</div>";
    for (int q = 0; q < 4; q++) {
        const int ck = held_by(q);
        if (ck >= 0 && ((was.fighters >> ck) & 1) && !((now.fighters >> ck) & 1))
            r += "<div class=\"ce red\">" +
                 enc(tr("xeleenet.settings.effects.off_list", {{"name", port_name(q)}, {"fighter", fighter_name_of(ck)}})) +
                 "</div>";
    }
    if (now.teams && !was.teams)
        r += "<div class=\"ce\">" + enc(tr("xeleenet.settings.effects.teams")) + "</div>";
    return r + "<div class=\"ce\">" + enc(tr("xeleenet.settings.effects.next_game")) + "</div>";
}

// C2: the Host page's rows in two columns, minus the preset and the name; a
// changed row shows its old value; CHANGES, then APPLY and CANCEL.
void Screen::build_overlay_host() {
    HostSettings& h = *s_edit;
    const HostSettings& o = s_host; // what's in force (or on its way), the host's
    const bool host = !readOnly;
    const auto was = [&](const std::string& now, const std::string& before) { return host && now != before ? before : ""; };
    std::string r = "<div class=\"col left\">";
    group(r, "xeleenet.host.groups.rules");
    choice(r, "teams", 0, 0, "xeleenet.host.rows.teams", on_off(h.teams), [](int) { s_edit->teams = !s_edit->teams; },
           "xeleenet.host.hints.teams", true, nullptr, was(on_off(h.teams), on_off(o.teams)));
    if (h.stock)
        choice(r, "stocks", 0, 1, "xeleenet.host.rows.stocks", std::to_string(h.stocks),
               [](int d) { s_edit->stocks = step(kStocks, s_edit->stocks, d); }, "xeleenet.host.hints.stocks", true,
               nullptr, o.stock ? was(std::to_string(h.stocks), std::to_string(o.stocks)) : tr("xeleenet.host.values.time"));
    else
        choice(r, "stocks", 0, 1, "xeleenet.host.rows.win_by", tr("xeleenet.host.values.time"),
               [](int) { s_edit->stock = true; }, "xeleenet.host.hints.win_by", true, nullptr,
               o.stock ? tr("xeleenet.rules.values.stock") : "");
    choice(r, "time", 0, 2, "xeleenet.host.rows.time", minutes_label(h.minutes),
           [](int d) { s_edit->minutes = step(kMinutes, s_edit->minutes, d); }, "xeleenet.host.hints.time", true, nullptr,
           was(minutes_label(h.minutes), minutes_label(o.minutes)));
    choice(r, "items", 0, 3, "xeleenet.host.rows.items", tr(kItemNames[h.items + 1]),
           [](int d) { s_edit->items = (s_edit->items + 1 + d + 6) % 6 - 1; }, "xeleenet.host.hints.items", true, nullptr,
           was(tr(kItemNames[h.items + 1]), tr(kItemNames[o.items + 1])));
    if (h.teams)
        choice(r, "ff", 0, 4, "xeleenet.host.rows.ff", on_off(h.friendly_fire),
               [](int) { s_edit->friendly_fire = !s_edit->friendly_fire; }, "xeleenet.host.hints.ff", true, nullptr,
               was(on_off(h.friendly_fire), on_off(o.teams && o.friendly_fire)));
    const bool more_changed = h.stock != o.stock || h.pause != o.pause || h.handicap != o.handicap || h.damage != o.damage;
    link(r, "more", 0, 5, "xeleenet.host.rows.more", tr("xeleenet.host.values.more"), [this] { go(Page::More); },
         "xeleenet.host.hints.more", host && more_changed ? tr("xeleenet.settings.changed") : "");
    choice(r, "place", 0, 6, "xeleenet.host.rows.placement", tr("xeleenet.host.values.placement"), [](int) {},
           "xeleenet.host.hints.placement", false, "xeleenet.host.soon");
    group(r, "xeleenet.settings.groups.links");
    r += "<div class=\"links\">";
    for (const std::string& line : links_lines())
        r += "<div class=\"ll\">" + enc(line) + "</div>";
    r += "</div>";
    // CHANGES under the links, where the left column has the room for it.
    if (host)
        r += "<div id=\"changes\">" + changes_box(o, h) + "</div>";
    else
        r += "<div id=\"changes\" class=\"ro\"><div class=\"cl none\">" + enc(tr("xeleenet.settings.host_only")) +
             "</div></div>";
    r += "</div><div class=\"col right\">";
    group(r, "xeleenet.settings.groups.right");
    link(r, "flist", 1, 0, "xeleenet.host.rows.fighter_list", fighter_list_label(h.fighters), [this] { go(Page::Fighters); },
         "xeleenet.host.hints.fighter_list", was(fighter_list_label(h.fighters), fighter_list_label(o.fighters)));
    link(r, "slist", 1, 1, "xeleenet.host.rows.stage_list", stage_list_label(h.stages), [this] { go(Page::Stages); },
         "xeleenet.host.hints.stage_list", was(stage_list_label(h.stages), stage_list_label(o.stages)));
    choice(r, "pick", 1, 2, "xeleenet.host.rows.stage_pick", tr("xeleenet.host.values.stage_pick"), [](int) {},
           "xeleenet.host.hints.stage_pick", false, "xeleenet.host.soon");
    choice(r, "frozen", 1, 3, "xeleenet.host.rows.frozen", on_off(h.frozen), [](int) { s_edit->frozen = !s_edit->frozen; },
           "xeleenet.host.hints.frozen", true, nullptr, was(on_off(h.frozen), on_off(o.frozen)));
    choice(r, "format", 1, 4, "xeleenet.host.rows.format", tr("xeleenet.host.values.format"), [](int) {},
           "xeleenet.host.hints.format", false, "xeleenet.host.soon");
    const int least = settings_seats_min();
    choice(r, "seats", 1, 5, "xeleenet.host.rows.seats", std::to_string(h.seats),
           [least](int d) { s_edit->seats = std::clamp(s_edit->seats + d, least, 4); },
           h.seats <= least && host ? "xeleenet.settings.hints.seats_limit" : "xeleenet.host.hints.seats", true, nullptr,
           was(std::to_string(h.seats), std::to_string(o.seats)), host && h.seats <= least);
    if (!lan() && host) { // the listing's own: a guest has no say, and LAN lists every lobby
        choice(r, "vis", 1, 6, "xeleenet.host.rows.visibility",
               tr(h.open ? "xeleenet.host.values.public" : "xeleenet.host.values.private"),
               [](int) { s_edit->open = !s_edit->open; }, "xeleenet.host.hints.visibility", true, nullptr,
               h.open != o.open ? tr(o.open ? "xeleenet.host.values.public" : "xeleenet.host.values.private") : "");
        choice(r, "hide", 1, 7, "xeleenet.host.rows.hide_code", on_off(h.hide_code),
               [](int) { s_edit->hide_code = !s_edit->hide_code; }, "xeleenet.host.hints.hide_code", true, nullptr,
               was(on_off(h.hide_code), on_off(o.hide_code)));
    }
    if (host) {
        r += "<div class=\"obtns\"><button id=\"apply\" class=\"gold\">" + enc(tr("xeleenet.settings.apply")) +
             "</button><button id=\"cancel\" class=\"plain\">" + enc(tr("xeleenet.settings.cancel")) + "</button></div>";
        Item ap{"apply", 1, 8, true};
        ap.act = [] { settings_done(true); };
        ap.hint = tr("xeleenet.settings.hints.apply");
        items.push_back(std::move(ap));
        Item ca{"cancel", 1, 9, true};
        ca.act = [] { settings_done(false); };
        ca.hint = tr("xeleenet.settings.hints.cancel");
        items.push_back(std::move(ca));
    }
    r += "</div>";
    set_board(r);
}

std::string Screen::held_chips(int ckind) const {
    std::string r;
    int n = 0;
    for (int q = 0; q < 4; q++) {
        const int ck = held_by(q);
        if (ck < 0 || (ck == XN_FIGHTER_SHEIK ? XN_FIGHTER_ZELDA : ck) != ckind)
            continue;
        r += "<div class=\"held\" style=\"right: " + std::to_string(4 + 30 * n++) + "px; background-color: " +
             kPortColor[q] + ";\">P" + std::to_string(q + 1) + "</div>";
    }
    return r;
}

std::string Screen::held_off_note() const {
    if (items.empty() || items[cursor].id.rfind("fi", 0) != 0)
        return "";
    const int ckind = xn_fighter_order[std::atoi(items[cursor].id.c_str() + 2)];
    if ((s_edit->fighters >> ckind) & 1)
        return "";
    for (int q = 0; q < 4; q++) {
        const int ck = held_by(q);
        if (ck >= 0 && (ck == XN_FIGHTER_SHEIK ? XN_FIGHTER_ZELDA : ck) == ckind)
            return tr("xeleenet.settings.held_off",
                      {{"fighter", fighter_name_of(ckind)}, {"name", port_name(q)}, {"p", q + 1}});
    }
    return "";
}

// The port this machine's overlay rests in a session (its primary), -1 none:
// taken up again should a session start while it's open, and let go when
// it closes, whether a session still runs or not.
int s_hold_port = -1;
void settings_hold(bool on) {
    const int want = on && pc_net_active() ? pc_net_local_player() : -1;
    if (want == s_hold_port)
        return;
    if (s_hold_port >= 0)
        pc_net_hold_local(s_hold_port, false);
    if (want >= 0)
        pc_net_hold_local(want, true);
    s_hold_port = want;
}

// C3: the host tells the guests it has Lobby Settings open, or closed it.
void editing_send(bool on) {
    if (!pc_net_active() || pc_net_local_player() != 0)
        return;
    const uint8_t b = on ? 1 : 0;
    pc_net_send_reliable(PC_NET_MSG_XN_EDITING, &b, 1);
}

void settings_open(bool host) {
    if (s_over.shown)
        return;
    s_over.overlay = true;
    s_draft_set(host);
    s_over.readOnly = !host;
    s_over.page = Page::Host;
    s_over.cursorId.clear();
    s_over.cursor = 0;
    s_over.show(true);
    s_over_at = SDL_GetTicks();
    settings_hold(true);
    editing_send(true);
    sfx(SFX_CONFIRM);
    pc_log_line("xeleenet: Lobby Settings open (%s)", host ? "host" : "read-only");
}

void settings_done(bool apply) {
    if (!s_over.shown)
        return;
    const bool changed = !plays_same(s_draft, s_host) || s_draft.open != s_host.open || s_draft.hide_code != s_host.hide_code;
    if (apply && !s_over.readOnly && changed) {
        pc_log_line("xeleenet: Lobby Settings applied: %s", [] {
            std::string t;
            for (const std::string& c : rule_changes(s_host, s_draft, true))
                t += (t.empty() ? "" : "; ") + c;
            return t;
        }().c_str());
        apply_host(s_draft);
    } else {
        pc_log_line("xeleenet: Lobby Settings closed%s", apply ? " (nothing changed)" : "");
    }
    if (!apply)
        sfx(SFX_BACK);
    s_over.show(false);
    s_edit = &s_host;
    settings_hold(false);
    editing_send(false);
}

// The overlay's pad, read off the physical pad once a CSS frame (the port
// itself rests): presses and the stick's direction, with a repeat, as the
// menus' input.
void settings_pad(unsigned buttons, int stick_x, int stick_y) {
    static unsigned last;
    static int dir_last = -1, held = 0;
    static uint64_t opened = 0;
    int dir = -1;
    const int ax = std::abs(stick_x), ay = std::abs(stick_y);
    if ((buttons & PAD_BUTTON_UP) || (stick_y > 50 && ay >= ax))
        dir = PC_SCREEN_UP;
    else if ((buttons & PAD_BUTTON_DOWN) || (stick_y < -50 && ay >= ax))
        dir = PC_SCREEN_DOWN;
    else if ((buttons & PAD_BUTTON_LEFT) || stick_x < -50)
        dir = PC_SCREEN_LEFT;
    else if ((buttons & PAD_BUTTON_RIGHT) || stick_x > 50)
        dir = PC_SCREEN_RIGHT;
    if (opened != s_over_at) {
        // Its first frame: what's held already (the Z or the A that opened
        // it) is no press.
        opened = s_over_at;
        last = buttons;
        dir_last = dir;
        held = 0;
        return;
    }
    if (dir != dir_last) {
        held = 0;
        if (dir >= 0)
            s_over.input(dir);
    } else if (dir >= 0 && ++held >= 20 && (held - 20) % 5 == 0) {
        s_over.input(dir);
    }
    dir_last = dir;
    const unsigned press = buttons & ~last;
    last = buttons;
    if (press & PAD_BUTTON_START)
        s_over.input(PC_XN_IN_START);
    else if (press & PAD_BUTTON_A)
        s_over.input(PC_SCREEN_CONFIRM);
    else if (press & PAD_BUTTON_B)
        s_over.input(PC_SCREEN_BACK);
    if (press & PAD_BUTTON_X)
        s_over.input(PC_XN_IN_X);
    else if (press & PAD_BUTTON_Y)
        s_over.input(PC_XN_IN_Y);
}

struct SssOverlay {
    Rml::ElementDocument* document = nullptr;
    bool shown = false;
    std::string last;
    unsigned mask = 0;
    PcXnSssState st{};
    uint64_t st_at = 0; // when mnstagesel.c last told it

    void show(bool on) {
        if (on && document == nullptr) {
            auto* context = aurora::rmlui::get_context();
            if (context == nullptr)
                return;
            load_fonts();
            register_sss_textures(); // before anything asks for a ring or a picture
            document = context->LoadDocument(std::string(pc_resources_dir()) + "xeleenet-sss.rml");
            if (document == nullptr) {
                pc_log_line("xeleenet: could not load xeleenet-sss.rml");
                return;
            }
        }
        if (document == nullptr || on == shown)
            return;
        shown = on;
        last.clear();
        if (!on)
            st_at = 0;
        if (on)
            document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        else
            document->Hide();
    }

    // A card's picture: the icon's, a Random die, or a dashed box while
    // they choose.
    static std::string thumb(int cell, bool random, const char* cls) {
        if (random)
            return std::string("<div class=\"") + cls + " rnd\">?</div>";
        if (cell < 0)
            return std::string("<div class=\"") + cls + " pend\"></div>";
        if (cell < int(s_thumbs.size()) && !s_thumbs[cell].rgba.empty())
            return std::string("<img class=\"") + cls + "\" src=\"xnstage://" + std::to_string(cell) + "\"/>";
        return std::string("<div class=\"") + cls + "\"></div>";
    }

    void update() {
        if (!shown)
            return;
        auto* context = aurora::rmlui::get_context();
        auto* root = document->GetElementById("root");
        if (context == nullptr || root == nullptr)
            return;
        const Rml::Vector2i d = context->GetDimensions();
        std::string r;
        // The list's name, top right; the legend, bottom centre. Laid out at
        // 720 px tall, scaled to the window as the banner is.
        const float scale = d.y / 720.f;
        r += "<div id=\"top\" style=\"transform: scale(" + fmt(scale) + "); left: " + fmt(d.x - 624.f * scale) +
             "px; top: " + fmt(22.f * scale) + "px;\"><div class=\"pill\">" +
             enc(tr("xeleenet.sss.list", {{"list", stage_list_label(mask & kAllStages)}})) + "</div></div>";
        const bool picks = st_at != 0 && SDL_GetTicks() - st_at < 500;
        const bool sample = s_preview.rfind("sss-", 0) == 0; // tools/xn_preview.py: we are P2
        const int me = sample ? 1 : pc_net_local_player();
        PcXnPunchView v;
        view(&v);
        std::array<std::string, 4> names;
        if (sample)
            names = {"RHYTHM", "PIXEL", "WAVEDASH KINGPN", "KOOPA"};
        else
            for (int i = 0; i < v.nplayers; i++)
                if (v.players[i].port >= 0 && v.players[i].port < 4)
                    names[v.players[i].port] = short_name(v.players[i].name);
        for (int i = 0; i < 4; i++)
            if (names[i].empty())
                names[i] = "P" + std::to_string(i + 1);
        if (picks && !st.final)
            r += outlines(d);
        if (picks && !st.final)
            r += card(d, names, me);
        if (picks && st.final)
            r += draw(d, names);
        // Only the host takes everyone back to the CSS (mnstagesel.c); once
        // the draw is made, nobody has anything to press.
        if (!(picks && st.final)) {
            // With a pick, B takes it back (the only way to change it); the
            // host's B with none takes everyone back to the CSS.
            const bool mine = picks && me >= 0 && me < 4 && st.pick[me] >= 0;
            std::string keys = mine ? legend_key("b", "xeleenet.legend.change_pick")
                                    : legend_key("stick", "xeleenet.legend.move") +
                                          legend_key("a", "xeleenet.legend.pick");
            if (me == 0 && !mine)
                keys += legend_key("b", "xeleenet.legend.everyone_back");
            r += "<div id=\"bottom\" style=\"transform: scale(" + fmt(scale) + "); left: " +
                 fmt(d.x / 2 - 560.f * scale) + "px; top: " + fmt(d.y - 62.f * scale) +
                 "px;\"><div class=\"keys\">" + keys + "</div></div>";
        }
        if (r != last) {
            last = r;
            root->SetInnerRML(r);
        }
    }

    // Each pick ringed on the grid in its player's colour; a stage two
    // picked gets two rings, one outside the other.
    std::string outlines(const Rml::Vector2i d) const {
        std::string r;
        const float k = d.y / 720.f;
        for (int i = 0; i < 4; i++) {
            if (!(st.ports & (1u << i)) || !st.outline_ok[i])
                continue;
            int j = 0;
            for (int o = 0; o < i; o++)
                j += (st.ports & (1u << o)) && st.outline_ok[o] && st.pick[o] == st.pick[i];
            // Just outside the frame, its corner cuts kept parallel (a cut
            // grows by (sqrt 2 - 1) of the outset).
            const float pad = (1.5f + 5.f * j) * k, bw = 3.5f * k;
            const float fw = st.outline[i][2] * d.x;
            const int x = int(std::lround(st.outline[i][0] * d.x - pad)), y = int(std::lround(st.outline[i][1] * d.y - pad));
            const int w = int(std::lround(fw + 2 * pad)), h = int(std::lround(st.outline[i][3] * d.y + 2 * pad));
            const float cut = kFrameCut * fw + 0.4142f * pad;
            char src[96];
            std::snprintf(src, sizeof src, "xnring://%d_%d_%.1f_%.1f_%d_%s", w, h, bw, cut, st.outline_bevels[i],
                          kPortColor[i] + 1);
            r += "<img class=\"ol\" src=\"" + std::string(src) + "\" style=\"left: " + std::to_string(x) +
                 "px; top: " + std::to_string(y) + "px; width: " + std::to_string(w) + "px; height: " +
                 std::to_string(h) + "px;\"/>";
        }
        return r;
    }

    // The picks card (wireframe S2), in the corner under RANDOM where Melee
    // shows the hovered stage's name and series (hidden in a lobby,
    // mnstagesel.c): a row per player, or for four a 2x2 grid of pictures.
    // Our own entry stands in for the hidden name: the stage the cursor is
    // on, ghosted, until we pick. Anything not locked in is faint, its
    // colour bar too.
    std::string card(const Rml::Vector2i d, const std::array<std::string, 4>& names, int me) const {
        if (st.ports == 0)
            return "";
        int n = 0;
        for (int i = 0; i < 4; i++)
            n += (st.ports >> i) & 1;
        const float k = d.y / 720.f;
        const bool grid = n >= 4;
        // Taller than Brinstar Depths' bottom, the card must end left of it.
        const float w = n <= 2 ? 250.f : 210.f;
        const float h = grid ? 40.f + 2 * kTileH + 8.f : 40.f + n * kRowH + 10.f;
        // Names cut short to fit three rows' width (the "(you)" kept whole).
        const size_t chars = n == 3 ? 12 : 15;
        std::string r = "<div id=\"card\" style=\"transform: scale(" + fmt(k) + "); width: " + fmt(w) +
                        "px; height: " + fmt(h) + "px; left: " + fmt(kCardLeft * d.x) + "px; top: " +
                        fmt(kCardBottom * d.y - h * k) + "px;\"><div class=\"ctitle\">" +
                        enc(tr("xeleenet.sss.picks_title"));
        if (st.timer >= 0) {
            const int secs = (st.timer + 59) / 60;
            r += "<div class=\"ctimer" + std::string(secs <= 5 ? " low" : "") + "\">" +
                 enc(tr("xeleenet.sss.timer", {{"n", secs}})) + "</div>";
        }
        r += "</div><div class=\"cdrain\">";
        if (st.timer >= 0 && st.timer_total > 0)
            r += "<div style=\"width: " + fmt(100.f * st.timer / st.timer_total) + "%;\"></div>";
        r += "</div>";
        int at = 0;
        for (int i = 0; i < 4; i++) {
            if (!(st.ports & (1u << i)))
                continue;
            const int p = st.pick[i];
            const bool ghost = p < 0 && i == me && st.hover >= 0;
            std::string pic;
            if (p >= 0)
                pic = "<div class=\"cpic\" style=\"border-color: " + std::string(kPortColor[i]) + ";\">" +
                      thumb(p < 30 ? p : -1, p >= 29, "cthumb") + "</div>";
            else if (ghost)
                pic = "<div class=\"cpic pend\"><div class=\"ghost\">" + thumb(st.hover, false, "cthumb") + "</div></div>";
            else
                pic = "<div class=\"cpic pend\"></div>";
            const std::string bar = "<div class=\"cbar" + std::string(p < 0 ? " pend" : "") +
                                    "\" style=\"background-color: " + std::string(kPortColor[i]) + ";\"></div>";
            if (grid) {
                const float x = 14.f + (at % 2) * (kTileW + 12.f), y = 40.f + (at / 2) * kTileH;
                at++;
                // No names: which tile is ours is its port, as our cursor's
                // token reads, bright among the others.
                r += "<div class=\"ctile\" style=\"left: " + fmt(x) + "px; top: " + fmt(y) + "px;\">" + bar + pic +
                     "<div class=\"ctag" + std::string(i == me ? " me" : "") + "\">P" + std::to_string(i + 1) +
                     "</div></div>";
                continue;
            }
            std::string stage;
            if (p >= 0)
                stage = "<div class=\"cstage\">" +
                        enc(st.pick_id[i] >= 0 ? tr(stage_name_key(st.pick_id[i]).c_str()) : tr("xeleenet.sss.random")) +
                        "</div>";
            else if (ghost)
                stage = "<div class=\"cstage pend\">" + enc(tr(stage_name_key(st.hover_id).c_str())) + "</div>";
            else
                stage = "<div class=\"cstage pend\">" + enc(tr("xeleenet.sss.choosing")) + "</div>";
            const std::string you = tr("xeleenet.sss.you", {{"name", ""}}); // " (you)" around nothing
            const std::string name =
                i == me ? tr("xeleenet.sss.you", {{"name", clip(names[i], chars > you.size() ? chars - you.size() : 1)}})
                        : clip(names[i], chars);
            r += "<div class=\"crow\" style=\"top: " + fmt(40.f + at++ * kRowH) + "px;\">" + bar + pic +
                 "<div class=\"cname\">" + enc(name) + "</div>" + stage + "</div>";
        }
        return r + "</div>";
    }

    // A name cut to `n` characters, its last an ellipsis (names are ASCII).
    static std::string clip(const std::string& s, size_t n) {
        return s.size() <= n ? s : s.substr(0, n - 1) + "\xe2\x80\xa6";
    }

    // The draw (wireframe S3, S4): the cards side by side, the highlight
    // running across them, then resting on the winner with the rest faded.
    // When every card is the same stage, just that one.
    std::string draw(const Rml::Vector2i d, const std::array<std::string, 4>& names) const {
        const float k = d.y / 720.f;
        const bool same = st.draw == 0;
        const bool landed = st.t >= st.draw;
        std::vector<int> ports;
        for (int i = 0; i < 4; i++)
            if ((st.ports & (1u << i)) && (!same || i == st.chosen))
                ports.push_back(i);
        const int n = int(ports.size());
        const float cw = 150.f, gap = 18.f;
        const float pw = std::max(380.f, n * cw + (n - 1) * gap + 56.f), ph = 300.f;
        const float padx = (pw - n * cw - (n - 1) * gap) / 2;
        std::string r = "<div id=\"dim\"></div><div id=\"draw\" style=\"transform: scale(" + fmt(k) +
                        "); width: " + fmt(pw) + "px; left: " + fmt(d.x / 2 - pw * k / 2) +
                        "px; top: " + fmt(d.y / 2 - ph * k / 2) + "px;\"><div class=\"dtitle\">" +
                        enc(landed ? tr("xeleenet.sss.landed") : tr("xeleenet.sss.drawing", {{"n", n}})) +
                        "</div><div class=\"dcards\">";
        for (int c = 0; c < n; c++) {
            const int i = ports[c];
            const bool lit = st.t >= 0 && st.lit == i;
            const int cell = st.cell[i];
            std::string tag;
            if (st.timed_out & (1u << i))
                tag = "<div class=\"dtag\">" + enc(tr("xeleenet.sss.time_tag")) + "</div>";
            else if (st.random & (1u << i))
                tag = "<div class=\"dtag\">" + enc(tr("xeleenet.sss.random_tag")) + "</div>";
            r += "<div class=\"dcard" + std::string(lit ? " lit" : "") +
                 std::string(landed && !lit ? " out" : "") + "\" style=\"left: " + fmt(padx + c * (cw + gap)) +
                 "px;\">" + thumb(cell, false, "dthumb") + tag + "<div class=\"dstage\">" +
                 enc(st.cell_id[i] >= 0 ? tr(stage_name_key(st.cell_id[i]).c_str()) : "") +
                 "</div><div class=\"dwho\" style=\"background-color: " + kPortColor[i] + ";\">" +
                 enc(same ? tr("xeleenet.sss.everyone") : names[i]) + "</div></div>";
        }
        r += "</div>";
        std::string caption;
        if (landed && st.chosen >= 0 && st.chosen < 4) {
            const std::string stage =
                st.cell_id[st.chosen] >= 0 ? tr(stage_name_key(st.cell_id[st.chosen]).c_str()) : "";
            caption = same ? tr("xeleenet.sss.whose_all", {{"stage", stage}})
                           : tr("xeleenet.sss.whose", {{"stage", stage}, {"name", names[st.chosen]}});
        }
        r += "<div class=\"dcap\">" + enc(caption) + "</div></div>";
        return r;
    }
};

Screen s_screen;
Banner s_banner;
SssOverlay s_sss;

bool s_active = false;
bool s_shown = false;
bool s_f5_was = false;

// F5: every document's stylesheets again from disk, and the text
// (resources/lang), for working on the look and the words. The pages pick the
// text up as they next build.
void reload_styles() {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool down = keys != nullptr && keys[SDL_SCANCODE_F5];
    if (down && !s_f5_was) {
        pc_lang_reload();
        if (auto* context = aurora::rmlui::get_context()) {
            Rml::Factory::ClearStyleSheetCache();
            for (int i = 0; i < context->GetNumDocuments(); i++)
                context->GetDocument(i)->ReloadStyleSheet();
            pc_log_line("rmlui: stylesheets reloaded (F5)");
        }
    }
    s_f5_was = down;
}

// The lobby's answer to the last Apply: refused seats (fewer than the
// players in it, a joiner having come meanwhile) go back, with its reason.
void settings_answer() {
    char why[160] = "";
    if (pc_xn_settings_result(why, sizeof why) >= 0)
        return;
    s_refused = why[0] != '\0' ? why : tr("xeleenet.css.refused_seats");
    s_refused_at = SDL_GetTicks();
    pc_log_line("xeleenet: the lobby refused the new settings: %s", s_refused.c_str());
    if (s_seats_before >= 2 && s_seats_before != s_host.seats) {
        HostSettings back = s_host;
        back.seats = s_seats_before;
        apply_host(back);
    }
}

// tools/xn_preview.py's Lobby Settings states, once the banner is up:
// css-settings (the host, Stocks and the fighter list changed),
// css-settings-fighters (its fighter page, on a held fighter that's off),
// css-settings-guest (read-only), css-settings-seats (at the seats' limit),
// css-changed (C4), css-offlist (F3) and css-editing (C3, a guest's).
void preview_settings() {
    static bool done;
    if (done || SDL_GetTicks() < 7000)
        return;
    done = true;
    const std::string pv = s_preview;
    PcXnRules r{};
    pc_xn_menu_rules(&r);
    if (pv.rfind("css-settings", 0) == 0) {
        s_rules_now = s_host;
        s_rules_known = true;
        s_draft_set(pv != "css-settings-guest");
        settings_open(pv != "css-settings-guest");
        if (pv == "css-settings" || pv == "css-settings-fighters") {
            s_draft.stocks = 3;
            s_draft.fighters = XN_FIGHTERS_LOW;
        }
        if (pv == "css-settings-seats")
            s_draft.seats = 3;
        if (pv == "css-settings-fighters") {
            s_over.go(Page::Fighters, "fi10"); // Fox
        } else {
            s_over.cursorId = pv == "css-settings-seats" ? "seats" : "stocks";
            s_over.refresh();
        }
    } else if (pv == "css-changed") {
        s_rules_now = s_host;
        s_rules_known = true;
        r.stocks = 3;
        r.fighter_mask = XN_FIGHTERS_LOW;
        pc_xn_css_rules(&r);
    } else if (pv == "css-offlist") {
        pc_xn_css_dropped(2, 0x02); // our Fox (we're P3 in these)
    } else if (pv == "css-editing") {
        s_host_editing = true;
    }
}

} // namespace

extern "C" void pc_xn_menu_note(const char* key) { s_note = key != nullptr ? key : ""; }

extern "C" void pc_xn_menu_hide_lobby(const char* code) {
    s_hidden_code.assign(code, strnlen(code, 6));
    s_hidden_at = SDL_GetTicks();
}

extern "C" void pc_xn_menu_begin(int what) {
    pc_log_line("xeleenet pages: begin (%d)", what);
    host_load();
    const char* preview = std::getenv("MELEE_XN_PREVIEW");
    s_preview = preview != nullptr ? preview : "";
    if (s_preview.rfind("lan-", 0) == 0)
        pc_xn_set_network(PC_XN_NET_LAN);
    s_what = what;
    s_result = PC_XN_MENU_STAY;
    s_active = true;
    s_screen.hostNote = s_note.empty() ? std::string() : tr(s_note.c_str());
    s_note.clear();
    // A lobby's state ends with it: the next one's CSS starts afresh.
    settings_done(false);
    s_rules_known = s_host_editing = s_applying = false;
    s_rules_changed_at = 0;
    s_dropped_ckind = {-1, -1, -1, -1};
    // The player's name comes first: asked for when there is none, or when
    // Name was picked.
    if (s_preview.empty() && (what == PC_XN_MENU_NAME || pc_xn_name()[0] == '\0'))
        ask_keyboard(Typing::Player, pc_xn_name(),
                     tr(pc_xn_name()[0] == '\0' ? "xeleenet.name.ask_first" : "xeleenet.name.ask"),
                     12);
    if (s_preview == "stages") {
        s_screen.go(Page::Stages);
    } else if (s_preview == "fighters") {
        s_host.fighters = XN_FIGHTERS_LOW;
        s_screen.go(Page::Fighters);
    } else if (s_preview == "more") {
        s_screen.go(Page::More);
    } else if (s_preview == "code" || s_preview == "lan-address") {
        s_screen.code = lan() ? "@192.168.0.5" : "K7M";
        s_screen.go(Page::Code);
    } else if (s_preview == "joining" || s_preview == "progress") {
        s_screen.joining = sample("K7MQ2D", "Friday netplay", "PIXEL", "3MPA", "US", 3, 4, true, true, 4, 8, true,
                                  false, false);
        s_screen.joiningKnown = true;
        s_screen.code = "K7MQ2D";
        s_screen.go(Page::Joining);
    } else if (s_preview == "failed") {
        s_screen.code = "K7MQ2D";
        s_screen.failed();
    } else if (s_preview == "details" || s_preview == "lan-details") {
        s_screen.to_lobbies();
        s_screen.details = details_sample(lan());
        s_screen.go(Page::Details, "dtop");
    } else if (what == PC_XN_MENU_JOIN || s_preview == "lobbies" || s_preview == "lan-lobbies" ||
               s_preview == "lan-empty" || s_preview == "lan-blocked") {
        s_screen.to_lobbies();
    } else {
        s_screen.go(Page::Host);
    }
    if (!s_preview.empty())
        pc_log_line("xeleenet pages: preview %s", s_preview.c_str());
}

extern "C" void pc_xn_menu_end(void) {
    pc_xn_menu_show(false);
    s_active = false;
}

extern "C" void pc_xn_menu_show(bool show) {
    s_shown = show && s_what != PC_XN_MENU_NAME;
    s_screen.show(s_shown);
}

extern "C" void pc_xn_menu_presence(float presence) { s_screen.presence = presence; }

extern "C" void pc_xn_menu_input(int what) { s_screen.input(what); }

extern "C" bool pc_xn_menu_keyboard(char initial[32], int* max_len, const char** prompt) {
    if (!s_kb.wanted)
        return false;
    s_kb.wanted = false;
    std::snprintf(initial, 32, "%s", s_kb.initial.c_str());
    *max_len = s_kb.max_len;
    *prompt = s_kb.prompt.c_str();
    return true;
}

extern "C" void pc_xn_menu_keyboard_done(bool confirmed, const char* text) {
    const Typing what = s_kb.what;
    s_kb.what = Typing::None;
    if (what == Typing::Lobby) {
        if (confirmed && text != nullptr)
            s_host.name = std::string(text).substr(0, 20);
        s_screen.refresh();
        return;
    }
    if (confirmed && text != nullptr && text[0] != '\0')
        pc_xn_set_name(text);
    // Name alone ends here; Host or Join without a name can't go on.
    if (s_what == PC_XN_MENU_NAME || pc_xn_name()[0] == '\0')
        s_result = PC_XN_MENU_LEAVE;
}

extern "C" int pc_xn_menu_result(void) {
    const int r = s_result;
    if (r != PC_XN_MENU_STAY)
        s_result = PC_XN_MENU_STAY;
    return r;
}

extern "C" void pc_xn_menu_rules(PcXnRules* out) {
    const HostSettings& h = s_host;
    out->seats = h.seats;
    out->teams = h.teams;
    out->stock = h.stock;
    out->stocks = h.stocks;
    out->minutes = h.minutes;
    out->items = h.items;
    out->friendly_fire = h.teams && h.friendly_fire;
    out->handicap = h.handicap;
    out->damage_ratio = h.damage;
    out->pause = h.pause;
    out->stage_mask = h.stages;
    out->fighter_mask = h.fighters;
    out->frozen_stadium = h.frozen;
}

extern "C" const char* pc_xn_menu_title(void) {
    return s_what == PC_XN_MENU_NAME ? pc_tr("xeleenet.name.title") : s_screen.title();
}

extern "C" const char* pc_xn_menu_hint(void) { return s_shown ? s_screen.hint() : nullptr; }

extern "C" void pc_xn_menu_update(void) {
    static bool read_env;
    if (!read_env) {
        read_env = true;
        if (const char* p = std::getenv("MELEE_XN_PREVIEW"))
            s_preview = p;
        if (s_preview == "lan-banner")
            pc_xn_set_network(PC_XN_NET_LAN);
    }
    reload_styles();
    if (s_active)
        s_screen.update();
    const bool css_preview = s_preview == "banner" || s_preview == "lan-banner" || s_preview.rfind("css-", 0) == 0;
    if (s_banner.shown || css_preview) {
        if (css_preview && !s_banner.shown) {
            const char* boot = std::getenv("MELEE_BOOT_SCENE");
            if (boot != nullptr && std::strcmp(boot, "css") == 0 && SDL_GetTicks() > 6000) {
                // Three players of four; css-N: 1v1, three free for all, Doubles.
                const int full = preview_full_seats();
                preset(full == 2 ? kOneVsOne : full == 3 ? kFreeForAll : kDoubles);
                if (full == 3)
                    s_host.preset = kCustom, s_host.seats = 3;
                s_banner.show(true);
            }
        }
        s_banner.update();
        if (css_preview && s_banner.shown)
            preview_settings();
    }
    // Lobby Settings: its fade, its board, and what's on its way.
    if (s_over.shown) {
        s_over.presence = std::min(1.f, float(SDL_GetTicks() - s_over_at) / 150.f);
        s_over.update();
    }
    if (pc_xn_punch_switch_pending() == PC_XN_SWITCH_SETTINGS)
        s_applying = true;
    settings_answer();
    s_sss.update();
    // The headset's laser, for whichever of them is up.
    auto* context = aurora::rmlui::get_context();
    const bool any = s_screen.shown || s_banner.shown || s_over.shown || s_sss.shown;
    s_pointer.feed(any, context != nullptr ? context->GetDimensions() : Rml::Vector2i{0, 0},
                   [] { s_screen.touched(); });
    s_screen.pointerWas = s_over.pointerWas = s_banner.pointerWas = s_pointer.was;
}

// While the CSS's or the stage select's overlay is up, the game's frame is as
// tall as the picture it's shown as (vi.c), so the overlay drawn inside it
// (aurora_rmlui_draw_here, under the hands and cursors) isn't resampled down
// to the render scale's EFB.
extern "C" bool pc_xn_overlay_full_size(void) { return s_banner.shown || s_sss.shown || s_over.shown; }

extern "C" void pc_xn_banner_show(bool show) {
    if (show && s_left_reload && SDL_GetTicks() - s_left_at < kLeftReloadMs)
        s_left_at = SDL_GetTicks(); /* the CSS is back, without them */
    // A seat change comes with the session, before the CSS reloads with its
    // door: say it again once the door is there.
    if (show && s_seat_note_at != 0 && SDL_GetTicks() - s_seat_note_at < kLeftReloadMs)
        s_seat_note_at = s_seat_new_at = SDL_GetTicks();
    if (show)
        s_left_reload = false;
    if (show && s_over.shown)
        editing_send(true); // through a reload: the guests heard nothing yet this CSS
    s_banner.show(show);
}

extern "C" void pc_xn_css_frame(const PcXnCssState* state) {
    s_css = *state;
    s_css_at = SDL_GetTicks();
    test_apply_frame();
    // tools/xn_preview.py's CSS states: three players, as the wireframe's R1-R3
    // and L1 (P1 holding B), with the real hand.
    const auto sample = [](uint8_t humans, uint8_t ready, uint8_t picked, uint8_t waiting, int countdown, int hold) {
        s_css.humans = humans, s_css.ready = ready, s_css.picked = picked, s_css.waiting = waiting;
        s_css.countdown = countdown, s_css.countdown_total = 120;
        s_css.hold = hold, s_css.hold_total = 60;
    };
    if (s_preview == "css-ready")
        sample(0x7, 0x1, 0x3, 0, -1, 0);
    else if (s_preview == "css-countdown")
        sample(0x7, 0x7, 0x7, 0, 70, 0);
    else if (s_preview == "css-waiting")
        sample(0x7, 0x5, 0x7, 0x2, -1, 0);
    else if (s_preview == "css-leaving")
        sample(0x7, 0x2, 0x7, 0, -1, 40);
    else if (s_preview == "css-left") {
        sample(0x3, 0, 0x3, 0, -1, 0);
        pc_xn_css_left("KOOPA");
    } else if (const int full = preview_full_seats()) {
        const uint8_t all = uint8_t((1u << full) - 1);
        sample(all, 0x2, all, 0, -1, 0);
    } else if (s_preview.rfind("css-settings", 0) == 0 || s_preview == "css-changed" || s_preview == "css-offlist" ||
               s_preview == "css-editing") {
        // Three players: P1 Luigi, P2 Fox, P3 (us) Pichu, or still choosing
        // once our Fox came off the list.
        const bool lost = s_preview == "css-offlist";
        sample(0x7, 0, lost ? 0x3 : 0x7, 0, -1, 0);
        s_css.ckind[0] = 0x07, s_css.ckind[1] = 0x02, s_css.ckind[2] = lost ? 0xFF : 0x18, s_css.ckind[3] = 0xFF;
        s_css.local = 0x4;
    }
}

extern "C" int pc_xn_css_seats(void) {
    // tools/xn_preview.py: css-N has N seats, the other CSS states Doubles'.
    if (std::getenv("MELEE_XN_PREVIEW") != nullptr)
        return preview_full_seats() ? preview_full_seats() : 4;
    PcXnPunchView v;
    view(&v);
    return lobby_seats(v.host);
}

extern "C" unsigned pc_xn_css_names(char names[4][16], int* local) {
    PcXnPunchView v;
    view(&v);
    unsigned named = 0;
    *local = -1;
    for (int i = 0; i < v.nplayers && i < 4; i++) {
        const int port = v.players[i].port;
        if (port < 0 || port > 3)
            continue;
        const char* hash = std::strchr(v.players[i].name, '#');
        const int len = hash != nullptr ? int(hash - v.players[i].name) : int(std::strlen(v.players[i].name));
        std::snprintf(names[port], 16, "%.*s", len, v.players[i].name);
        if (names[port][0] != '\0')
            named |= 1u << port;
        if (v.players[i].local)
            *local = port;
    }
    return named;
}

extern "C" void pc_xn_css_off_list(int port, int ckind) {
    if (pc_net_active() && !(pc_net_local_mask() & (1 << port)))
        return; // another machine's player: their notice, not ours
    for (int i = 0; i < XN_FIGHTER_ENTRIES; i++)
        if (xn_fighter_order[i] == ckind)
            seat_note("warn", tr("xeleenet.css.off_list", {{"name", fighter_name(i)}}));
    s_seat_note_at = SDL_GetTicks() - (kSeatNoteMs - kOffListNoteMs); // a shorter note
}

extern "C" void pc_xn_settings_open(bool host) { settings_open(host); }

extern "C" bool pc_xn_settings_up(void) { return settings_up(); }

extern "C" void pc_xn_settings_cancel(void) { settings_done(false); }

extern "C" void pc_xn_settings_pad(unsigned buttons, int stick_x, int stick_y) {
    if (!settings_up())
        return;
    settings_hold(true); // a session that started meanwhile
    settings_pad(buttons, stick_x, stick_y);
}

extern "C" void pc_xn_editing_receive(const void* payload, int len) {
    if (len < 1 || (pc_net_active() && pc_net_local_player() == 0))
        return;
    s_host_editing = static_cast<const uint8_t*>(payload)[0] != 0;
    pc_log_line("xeleenet: the host %s Lobby Settings", s_host_editing ? "opened" : "closed");
}

extern "C" void pc_xn_css_rules(const PcXnRules* r) {
    const HostSettings now = from_rules(*r);
    // C4: what the host changed, for everyone, once the CSS has it.
    if (s_rules_known) {
        const std::vector<std::string> lines = rule_changes(s_rules_now, now, false);
        if (!lines.empty()) {
            s_rules_prev = s_rules_now;
            s_rules_changed_at = SDL_GetTicks();
            s_rules_changed = lines.size() == 1
                                  ? tr("xeleenet.css.changed", {{"what", lines[0]}})
                                  : tr("xeleenet.css.changed_more", {{"what", lines[0]}, {"n", lines.size() - 1}});
            pc_log_line("xeleenet: the CSS has new rules: %s%s", lines[0].c_str(), lines.size() > 1 ? " (and more)" : "");
        }
    }
    s_rules_now = now;
    s_rules_known = true;
    s_applying = false;
    s_applying_at = 0;
    s_host_editing = false; // the host says so again if it still is (pc_xn_banner_show)
}

extern "C" void pc_xn_css_dropped(int port, int ckind) {
    if (port < 0 || port > 3)
        return;
    s_dropped_ckind[port] = ckind;
    s_dropped_at = SDL_GetTicks();
}

extern "C" void pc_xn_css_seat_full(int phys) {
    const int seats = pc_xn_css_seats();
    seat_note("warn", tr("xeleenet.css.seat_full", {{"n", phys + 1}, {"m", seats}}));
}

extern "C" void pc_xn_css_left(const char* name) {
    s_left_name = name != nullptr ? name : "";
    const size_t hash = s_left_name.find('#');
    if (hash != std::string::npos)
        s_left_name.resize(hash);
    s_left_at = SDL_GetTicks();
    s_left_reload = true;
}

extern "C" void pc_xn_sss_frame(uint32_t mask) {
    s_sss.mask = mask;
    s_sss.show(true);
}

extern "C" void pc_xn_sss_show(bool show) {
    s_sss.show(show);
}

extern "C" void pc_xn_sss_picks(const PcXnSssState* state) {
    s_sss.st = *state;
    s_sss.st_at = SDL_GetTicks();
}

extern "C" bool pc_xn_sss_has_thumb(int cell) {
    return cell >= 0 && cell < int(s_thumbs.size()) && !s_thumbs[cell].rgba.empty();
}

namespace {
// xnstage://<cell> or xnfighter://<entry>: the picture at the number in
// `pics`.
template <size_t N>
std::optional<aurora::rmlui::RuntimeTexture> picture_texture(std::string_view source,
                                                             const std::array<Picture, N>& pics) {
    const size_t at = source.find("://");
    if (at == std::string_view::npos)
        return std::nullopt;
    const int c = std::atoi(std::string(source.substr(at + 3)).c_str());
    if (c < 0 || c >= int(N) || pics[c].rgba.empty())
        return std::nullopt;
    aurora::rmlui::RuntimeTexture t;
    t.width = pics[c].w;
    t.height = pics[c].h;
    t.rgba8 = std::as_bytes(std::span(pics[c].rgba));
    t.generateMipmaps = true;
    return t;
}

void register_sss_textures() {
    static bool registered;
    if (!registered) {
        registered = true;
        aurora::rmlui::register_texture_provider("xnring", ring_texture);
        aurora::rmlui::register_texture_provider("xnstage", [](std::string_view source) {
            return picture_texture(source, s_thumbs);
        });
        aurora::rmlui::register_texture_provider("xnfighter", [](std::string_view source) {
            return picture_texture(source, s_portraits);
        });
    }
}
} // namespace

extern "C" void pc_xn_sss_thumb(int cell, const void* c8, int w, int h, const void* tlut, int tlut_fmt, int entries) {
    if (cell < 0 || cell >= int(s_thumbs.size()) || tlut == nullptr || w % 8 != 0 || h % 4 != 0)
        return;
    register_sss_textures();
    decode_gx(static_cast<const uint8_t*>(c8), 9, w, h, static_cast<const uint8_t*>(tlut), tlut_fmt, entries,
              s_thumbs[cell]);
}

extern "C" int pc_xn_fighter_thumb(int ckind, const void* image, int w, int h, int fmt, const void* tlut,
                                   int tlut_fmt, int entries) {
    for (int i = 0; i < XN_FIGHTER_ENTRIES; i++) {
        if (xn_fighter_order[i] != ckind)
            continue;
        register_sss_textures();
        return decode_gx(static_cast<const uint8_t*>(image), fmt, w, h, static_cast<const uint8_t*>(tlut), tlut_fmt,
                         entries, s_portraits[i])
                   ? 1
                   : 0;
    }
    return 0;
}

extern "C" bool pc_xn_menu_portraits_wanted(void) {
    if (!s_portraits_wanted || s_portraits_asked)
        return false;
    s_portraits_asked = true;
    return true;
}
