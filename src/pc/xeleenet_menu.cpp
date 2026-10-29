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

#include <RmlUi/Core.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_timer.h>
#include <aurora/rmlui.hpp>
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
#include <string>
#include <vector>

extern "C" void lbAudioAx_80024030(int);

namespace {

enum { SFX_BACK = 0, SFX_CONFIRM = 1, SFX_MOVE = 2 };

// The layout, in px, scaled to fit the frame's inside, as the Slippi pages'.
constexpr float kScreenW = 1120.f, kScreenH = 660.f;

const char* const kLeft = "\xe2\x80\xb9"; // the value arrows
const char* const kRight = "\xe2\x80\xba";
const char* const kDot = " \xc2\xb7 ";
const int kListRows = 5; // B1's rows in view

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

std::string enc(const std::string& s) { return Rml::StringUtilities::EncodeRml(s); }

void sfx(int which) { lbAudioAx_80024030(which); }

// MELEE_XN_PREVIEW: the page shown with sample data, else "".
std::string s_preview;

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

int stage_count(unsigned mask) {
    int n = 0;
    for (const Stage& s : kStages)
        n += (mask >> s.bit) & 1;
    return n;
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
    bool frozen = true;
    bool hide_code = false;
};
HostSettings s_host;
bool s_host_loaded;

void host_save() {
    char path[1024];
    FILE* f = pc_xn_profile_file("xeleenet-lobby.txt", path, sizeof path) ? std::fopen(path, "w") : nullptr;
    if (f == nullptr)
        return;
    const HostSettings& h = s_host;
    std::fprintf(f,
                 "preset=%d\nname=%s\nopen=%d\nseats=%d\nteams=%d\nstock=%d\nstocks=%d\nminutes=%d\nitems=%d\n"
                 "ff=%d\nhandicap=%d\ndamage=%d\npause=%d\nstages=%u\nfrozen=%d\nhide=%d\n",
                 h.preset, h.name.c_str(), h.open, h.seats, h.teams, h.stock, h.stocks, h.minutes, h.items,
                 h.friendly_fire, h.handicap, h.damage, h.pause, h.stages, h.frozen, h.hide_code);
    std::fclose(f);
}

void host_load() {
    if (s_host_loaded)
        return;
    s_host_loaded = true;
    char path[1024], line[96];
    FILE* f = pc_xn_profile_file("xeleenet-lobby.txt", path, sizeof path) ? std::fopen(path, "r") : nullptr;
    if (f == nullptr)
        return;
    HostSettings& h = s_host;
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
        else if (k == "frozen")
            h.frozen = n != 0;
        else if (k == "hide")
            h.hide_code = n != 0;
    }
    std::fclose(f);
    if (h.stages == 0)
        h.stages = kCompetitive;
}

void preset(int p) {
    HostSettings& h = s_host;
    h.preset = p;
    switch (p) {
    case kOneVsOne:
        h.seats = 2, h.teams = false, h.stock = true, h.stocks = 4, h.minutes = 8, h.items = -1;
        h.friendly_fire = false, h.stages = kCompetitive, h.frozen = true;
        break;
    case kDoubles:
        h.seats = 4, h.teams = true, h.stock = true, h.stocks = 4, h.minutes = 8, h.items = -1;
        h.friendly_fire = true, h.stages = kCompetitive, h.frozen = true;
        break;
    case kFreeForAll:
        h.seats = 4, h.teams = false, h.stock = false, h.stocks = 4, h.minutes = 2, h.items = 2;
        h.friendly_fire = false, h.stages = kAllStages, h.frozen = false;
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

std::string settings_json() {
    const HostSettings& h = s_host;
    char b[512];
    std::snprintf(b, sizeof b,
                  "{\"preset\":\"%s\",\"seats\":%d,\"mode\":\"%s\",\"win\":\"%s\",\"stocks\":%d,\"time_min\":%d,"
                  "\"items\":\"%s\",\"stage_list\":\"%s\",\"pick\":\"everyone\",\"format\":\"endless\","
                  "\"first_to\":2,\"place_secs\":15}",
                  kPresetIds[h.preset], h.seats, h.teams ? "teams" : "ffa", h.stock ? "stock" : "time", h.stocks,
                  h.minutes, kItemIds[h.items + 1],
                  h.stages == kCompetitive ? "competitive" : h.stages == kAllStages ? "all" : "custom");
    return b;
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
    return x;
}

// The punch's view, or the preview's: three players, one still connecting.
void view(PcXnPunchView* v) {
    if (s_preview.empty()) {
        pc_xn_punch_view(v);
        return;
    }
    std::memset(v, 0, sizeof *v);
    v->phase = PC_XN_PUNCH_LOBBY;
    v->host = s_preview == "banner";
    std::snprintf(v->code, sizeof v->code, "K7MQ2D");
    const char* names[3] = {"PIXEL#3MPA", "KOOPA#Z2RW", "RHYTHM#K7Q2"};
    for (int i = 0; i < 3; i++) {
        v->players[i].port = i;
        std::snprintf(v->players[i].name, sizeof v->players[i].name, "%s", names[i]);
        v->players[i].local = i == 2;
        v->players[i].linked = i != 1;
    }
    v->nplayers = 3;
}

// ---- the screens --------------------------------------------------------------

enum class Page { Host, Stages, More, Lobbies, Code, Joining, Failed };

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

    std::string hostNote; // H1's error line
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

    std::string hintText;
    std::string lastLegend;
    int scrollTop = 0; // B1's first row in view

    bool load() {
        if (document != nullptr)
            return true;
        auto* context = aurora::rmlui::get_context();
        if (context == nullptr)
            return false;
        load_fonts();
        document = context->LoadDocument(std::string(pc_resources_dir()) + "xeleenet.rml");
        if (document == nullptr) {
            pc_log_line("xeleenet pages: could not load xeleenet.rml");
            return false;
        }
        document->AddEventListener(Rml::EventId::Click, this);
        document->AddEventListener(Rml::EventId::Mouseover, this);
        return true;
    }

    void go(Page p, const char* focus = nullptr) {
        page = p;
        cursorId = focus != nullptr ? focus : "";
        cursor = 0;
        refresh();
    }

    // ---- H1 and its pages ----------------------------------------------------

    // name, hint and note are keys.
    void choice(std::string& r, const std::string& id, int col, int row, const char* name, const std::string& value,
                std::function<void(int)> change, const char* hint, bool enabled = true, const char* note = nullptr) {
        r += "<div id=\"" + id + "\" class=\"xrow" + (enabled ? "" : " off") + "\"><div class=\"n\">" + enc(tr(name)) +
             (note != nullptr ? "<span class=\"note\">" + enc(tr(note)) + "</span>" : std::string()) + "</div>";
        r += "<div class=\"v\"><span class=\"arr\">" + std::string(kLeft) + "</span>" + enc(value) +
             "<span class=\"arr\">" + kRight + "</span></div></div>";
        Item it{id, col, row, enabled};
        it.change = std::move(change);
        it.hint = tr(hint);
        items.push_back(std::move(it));
    }

    void link(std::string& r, const std::string& id, int col, int row, const char* name, const std::string& value,
              std::function<void()> act, const char* hint) {
        r += "<div id=\"" + id + "\" class=\"xrow link\"><div class=\"n\">" + enc(tr(name)) + "</div><div class=\"v\">" +
             enc(value) + "<span class=\"go\">" + kRight + "</span></div></div>";
        Item it{id, col, row, true};
        it.act = std::move(act);
        it.hint = tr(hint);
        items.push_back(std::move(it));
    }

    static void group(std::string& r, const char* name) {
        r += "<div class=\"grp\"><span>" + enc(tr(name)) + "</span></div>";
    }

    void build_host() {
        HostSettings& h = s_host;
        auto custom = [] { s_host.preset = kCustom; };
        std::string r;
        r += "<div id=\"preset\" class=\"pbar\"><div class=\"k\">" + enc(tr("xeleenet.host.preset")) +
             "</div><div class=\"pv\"><span class=\"arr\">" + kLeft + "</span>" + enc(tr(kPresetNames[h.preset])) +
             "<span class=\"arr\">" + kRight + "</span></div><div class=\"pk\">";
        for (int i = 0; i < kPresetCount; i++)
            r += (i > 0 ? kDot : "") + enc(tr(kPresetNames[i]));
        r += "</div></div>";
        {
            Item it{"preset", 0, 0, true};
            it.change = [](int d) { preset((s_host.preset + d + kPresetCount) % kPresetCount); };
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
                ask_keyboard(Typing::Lobby, s_host.name.empty() ? lobby_name() : s_host.name,
                             tr("xeleenet.host.ask_name"), 20);
            };
            it.hint = tr("xeleenet.host.hints.name");
            items.push_back(std::move(it));
        }
        choice(r, "vis", 0, 2, "xeleenet.host.rows.visibility",
               tr(h.open ? "xeleenet.host.values.public" : "xeleenet.host.values.private"),
               [](int) { s_host.open = !s_host.open; }, "xeleenet.host.hints.visibility");
        choice(r, "seats", 0, 3, "xeleenet.host.rows.seats", std::to_string(h.seats),
               [custom](int d) { s_host.seats = 2 + (s_host.seats - 2 + d + 3) % 3, custom(); },
               "xeleenet.host.hints.seats");
        group(r, "xeleenet.host.groups.rules");
        choice(r, "teams", 0, 4, "xeleenet.host.rows.teams", on_off(h.teams),
               [custom](int) { s_host.teams = !s_host.teams, custom(); }, "xeleenet.host.hints.teams");
        if (h.stock)
            choice(r, "stocks", 0, 5, "xeleenet.host.rows.stocks", std::to_string(h.stocks),
                   [custom](int d) { s_host.stocks = step(kStocks, s_host.stocks, d), custom(); },
                   "xeleenet.host.hints.stocks");
        else
            choice(r, "stocks", 0, 5, "xeleenet.host.rows.win_by", tr("xeleenet.host.values.time"),
                   [custom](int) { s_host.stock = true, custom(); }, "xeleenet.host.hints.win_by");
        choice(r, "time", 0, 6, "xeleenet.host.rows.time", minutes_label(h.minutes),
               [custom](int d) { s_host.minutes = step(kMinutes, s_host.minutes, d), custom(); },
               "xeleenet.host.hints.time");
        choice(r, "items", 0, 7, "xeleenet.host.rows.items", tr(kItemNames[h.items + 1]),
               [custom](int d) { s_host.items = (s_host.items + 1 + d + 6) % 6 - 1, custom(); },
               "xeleenet.host.hints.items");
        if (h.teams)
            choice(r, "ff", 0, 8, "xeleenet.host.rows.ff", on_off(h.friendly_fire),
                   [custom](int) { s_host.friendly_fire = !s_host.friendly_fire, custom(); },
                   "xeleenet.host.hints.ff");
        link(r, "more", 0, 9, "xeleenet.host.rows.more", tr("xeleenet.host.values.more"), [this] { go(Page::More); },
             "xeleenet.host.hints.more");
        r += "</div><div class=\"col right\">";
        group(r, "xeleenet.host.groups.stages");
        link(r, "slist", 1, 1, "xeleenet.host.rows.stage_list", stage_list_label(h.stages),
             [this] { go(Page::Stages); }, "xeleenet.host.hints.stage_list");
        choice(r, "pick", 1, 2, "xeleenet.host.rows.stage_pick", tr("xeleenet.host.values.stage_pick"), [](int) {},
               "xeleenet.host.hints.stage_pick", false, "xeleenet.host.soon");
        choice(r, "frozen", 1, 3, "xeleenet.host.rows.frozen", on_off(h.frozen),
               [custom](int) { s_host.frozen = !s_host.frozen, custom(); }, "xeleenet.host.hints.frozen");
        choice(r, "place", 1, 4, "xeleenet.host.rows.placement", tr("xeleenet.host.values.placement"), [](int) {},
               "xeleenet.host.hints.placement", false, "xeleenet.host.soon");
        group(r, "xeleenet.host.groups.set");
        choice(r, "format", 1, 5, "xeleenet.host.rows.format", tr("xeleenet.host.values.format"), [](int) {},
               "xeleenet.host.hints.format", false, "xeleenet.host.soon");
        group(r, "xeleenet.host.groups.joining");
        choice(r, "ask", 1, 6, "xeleenet.host.rows.ask", on_off(false), [](int) {}, "xeleenet.host.hints.ask", false,
               "xeleenet.host.soon");
        choice(r, "hide", 1, 7, "xeleenet.host.rows.hide_code", on_off(h.hide_code),
               [](int) { s_host.hide_code = !s_host.hide_code; }, "xeleenet.host.hints.hide_code");
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
        HostSettings& h = s_host;
        auto custom = [] { s_host.preset = kCustom; };
        std::string r = "<div class=\"col single\">";
        group(r, "xeleenet.rules.group");
        choice(r, "win", 0, 0, "xeleenet.rules.rows.win_by",
               tr(h.stock ? "xeleenet.rules.values.stock" : "xeleenet.rules.values.time"),
               [custom](int) { s_host.stock = !s_host.stock, custom(); }, "xeleenet.rules.hints.win_by");
        choice(r, "pause", 0, 1, "xeleenet.rules.rows.pause", on_off(h.pause),
               [custom](int) { s_host.pause = !s_host.pause, custom(); }, "xeleenet.rules.hints.pause");
        choice(r, "handicap", 0, 2, "xeleenet.rules.rows.handicap", tr(kHandicapNames[h.handicap]),
               [custom](int d) { s_host.handicap = (s_host.handicap + d + 3) % 3, custom(); },
               "xeleenet.rules.hints.handicap");
        char dmg[16];
        std::snprintf(dmg, sizeof dmg, "%.1f", h.damage / 10.f);
        choice(r, "damage", 0, 3, "xeleenet.rules.rows.damage", tr("xeleenet.rules.values.damage", {{"n", dmg}}),
               [custom](int d) { s_host.damage = 5 + (s_host.damage - 5 + d + 16) % 16, custom(); },
               "xeleenet.rules.hints.damage");
        r += "<button id=\"done\" class=\"gold\">" + enc(tr("xeleenet.rules.done")) + "</button></div>";
        Item it{"done", 0, 4, true};
        it.act = [this] { go(Page::Host, "more"); };
        it.hint = tr("xeleenet.rules.hints.done");
        items.push_back(std::move(it));
        set_board(r);
    }

    void build_stages() {
        const unsigned m = s_host.stages;
        std::string r = "<div class=\"chips\">";
        const char* chips[3] = {"xeleenet.stages.chips.competitive", "xeleenet.stages.chips.all",
                                "xeleenet.stages.chips.custom"};
        const bool on[3] = {m == kCompetitive, m == kAllStages, m != kCompetitive && m != kAllStages};
        for (int i = 0; i < 3; i++) {
            const std::string id = "chip" + std::to_string(i);
            r += "<div id=\"" + id + "\" class=\"chipx" + (on[i] ? " on" : "") + (i == 2 ? " dash" : "") + "\">" +
                 enc(tr(chips[i])) + "</div>";
            Item it{id, i, 0, i < 2};
            it.act = [i] {
                s_host.stages = i == 0 ? kCompetitive : kAllStages;
                s_host.preset = kCustom;
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
            Item it{id, i % kGridAcross, 1 + i / kGridAcross, true};
            it.act = [b = s.bit] {
                const unsigned want = s_host.stages ^ (1u << b);
                if (want != 0) // one stage at least stays on
                    s_host.stages = want;
                s_host.preset = kCustom;
            };
            it.hint = tr("xeleenet.stages.hint");
            items.push_back(std::move(it));
        }
        r += "</div>";
        set_board(r);
    }

    void open_lobby() {
        if (!s_preview.empty())
            return;
        const char* url = pc_xn_url();
        if (url[0] == '\0') {
            hostNote = tr("xeleenet.host.no_server");
            sfx(SFX_BACK);
            return;
        }
        pc_xn_host_options(lobby_name().c_str(), s_host.open, settings_json().c_str());
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
                sample("S4NQE7", "Sunday crew", "STAR", "Q4NE", "GB", 1, 4, true, true, 3, 7, true, false, false),
                sample("M2KD7F", "Marth dittos", "LUNA", "8KQD", "US", 1, 2, false, true, 4, 8, false, false, false),
                sample("B8HC3N", "Brinstar beginners", "MOSS", "T2LA", "DE", 2, 4, false, true, 4, 8, false, false,
                       false),
                sample("R6VW9J", "Quest crew", "ORBIT", "2WXZ", "US", 3, 4, true, true, 4, 8, true, false, true),
                sample("C5PL2Q", "Late night FFA", "NOVA", "C3RE", "JP", 2, 4, false, false, 4, 4, false, true,
                       false),
            };
            others = 2, fetch = 1, fetchedAt = SDL_GetTicks() - 4000;
            return;
        }
        PcNetIdentity id;
        const char* url = pc_xn_url();
        if (url[0] == '\0' || !pc_xn_identity(&id)) {
            fetch = -1;
            return;
        }
        fetch = pc_xn_browse(url, &id, showFull) ? 0 : -1;
        std::memset(&id, 0, sizeof id);
    }

    void poll_browse() {
        if (page != Page::Lobbies || fetch != 0 || !s_preview.empty())
            return;
        PcXnListing out[PC_XN_LIST_MAX];
        int n = 0, o = 0;
        const int st = pc_xn_browse_poll(out, PC_XN_LIST_MAX, &n, &o);
        if (st == 1) {
            rows.assign(out, out + n);
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
        if (fetch == 0)
            r += enc(tr("xeleenet.lobbies.looking"));
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
        if (fetch < 0) {
            const char* reason = nullptr;
            const char* err = pc_xn_error(&reason);
            r += "<div class=\"empty bad\">" +
                 enc(err != nullptr && err[0] ? std::string(err) : tr("xeleenet.lobbies.unreachable")) + "</div>";
        } else if (rows.empty() && fetch == 1) {
            r += "<div class=\"empty\">" + enc(tr("xeleenet.lobbies.empty")) + "</div>";
        }
        for (size_t i = 0; i < rows.size(); i++) {
            const PcXnListing& x = rows[i];
            const std::string id = "lb" + std::to_string(i);
            const bool full = x.taken >= x.seats;
            const char* mode = x.teams        ? "xeleenet.lobbies.modes.doubles"
                               : x.seats == 2 ? "xeleenet.lobbies.modes.1v1"
                                              : "xeleenet.lobbies.modes.ffa";
            const std::string sub =
                std::string(x.host) + (x.tag[0] ? "#" + std::string(x.tag).substr(0, 4) : "") + kDot +
                (x.stock ? tr("xeleenet.lobbies.stocks", {{"n", x.stocks}}) : tr("xeleenet.lobbies.time")) + kDot +
                minutes_label(x.time_min) + kDot +
                (x.first_to ? tr("xeleenet.lobbies.first_to", {{"n", x.first_to_n}}) : tr("xeleenet.lobbies.endless"));
            Item it{id, 0, static_cast<int>(i), !full};
            it.act = [this, i] {
                joining = rows[i];
                joiningKnown = true;
                code = rows[i].code;
                join();
            };
            it.hint = tr("xeleenet.lobbies.hints.row");
            items.push_back(std::move(it));
            if (static_cast<int>(i) < scrollTop || static_cast<int>(i) >= scrollTop + kListRows)
                continue;
            r += "<div id=\"" + id + "\" class=\"lrow" + (full ? " full" : "") + "\"><div class=\"lw\"><div class=\"ln\">" +
                 enc(x.name) + "</div><div class=\"ls\">" + enc(sub) + "</div></div><div class=\"lm\">" +
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

    void build_code() {
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
        if (why == "not_found" || why == "bad_code") {
            failWhy = "not_found";
            failCode = true;
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
            if (err != nullptr)
                failErr = err;
        }
        pc_log_line("xeleenet pages: join failed: %s (%s)", fail_title().c_str(), why.c_str());
        sfx(SFX_BACK);
        go(Page::Failed, failCode ? "edit" : "retry");
    }

    void join() {
        if (code.size() != 6)
            return;
        if (!s_preview.empty()) {
            go(Page::Joining);
            return;
        }
        const char* url = pc_xn_url();
        if (url[0] == '\0' || !pc_xn_punch_start(url, pc_xn_name(), code.c_str(), 4)) {
            failed();
            return;
        }
        pc_log_line("xeleenet pages: joining %s", code.c_str());
        go(Page::Joining);
    }

    void to_lobbies() {
        joiningKnown = false;
        code.clear();
        go(Page::Lobbies);
        browse();
    }

    // ---- the legend: what the buttons do, over Melee's description box ---------

    static std::string key(const std::string& button, const char* label) { return legend_key(button, label); }

    std::string legend() const {
        const Item* at = items.empty() ? nullptr : &items[cursor];
        const std::string id = at != nullptr ? at->id : "";
        std::string r;
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
        case Page::Lobbies:
            if (!rows.empty())
                r += key("a", "xeleenet.legend.join");
            r += key("y", "xeleenet.legend.enter_code") + key("x", "xeleenet.legend.refresh") +
                 key("start", showFull ? "xeleenet.legend.hide_full" : "xeleenet.legend.show_full") +
                 key("b", "xeleenet.legend.back");
            break;
        case Page::Code:
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

    void update_legend() {
        const std::string r = legend();
        if (r == lastLegend)
            return;
        lastLegend = r;
        if (auto* e = document->GetElementById("legend"))
            e->SetInnerRML(r);
    }

    // ---- the board ------------------------------------------------------------

    void set_board(const std::string& r) {
        if (r == last) {
            mark();
            update_legend();
            return;
        }
        last = r;
        auto* board = document->GetElementById("board");
        board->SetInnerRML(r);
        const char* kinds[] = {"host", "stages", "more", "lobbies", "code", "joining", "failed"};
        for (int i = 0; i < 7; i++)
            board->SetClass(kinds[i], static_cast<int>(page) == i);
        mark();
        update_legend();
    }

    void build() {
        if (document == nullptr)
            return;
        items.clear();
        switch (page) {
        case Page::Host: build_host(); break;
        case Page::More: build_more(); break;
        case Page::Stages: build_stages(); break;
        case Page::Lobbies: build_lobbies(); break;
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
        case Page::Lobbies:
            s_result = PC_XN_MENU_LEAVE;
            break;
        case Page::More:
            go(Page::Host, "more");
            break;
        case Page::Stages:
            go(Page::Host, "slist");
            break;
        case Page::Code:
            if (!code.empty()) {
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
            if (page == Page::Host || page == Page::More || page == Page::Stages) {
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
            if (page == Page::Lobbies) {
                sfx(SFX_CONFIRM);
                browse();
                refresh();
            } else if (page == Page::Stages) {
                s_host.stages = s_host.stages == kCompetitive ? kAllStages : kCompetitive;
                s_host.preset = kCustom;
                sfx(SFX_MOVE);
                refresh();
            }
            break;
        case PC_XN_IN_Y:
            if (page == Page::Lobbies) {
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
        if (page == Page::Joining) {
            PcXnPunchView v;
            view(&v);
            const int n = std::max(0, v.nplayers - 1);
            hintText = n > 0 ? tr("xeleenet.joining.hints.connecting", {{"n", n}})
                             : tr("xeleenet.joining.hints.finding");
        } else if (page == Page::Failed) {
            hintText = tr("xeleenet.failed.hint");
        } else if (page == Page::Lobbies && items.empty()) {
            hintText = tr("xeleenet.lobbies.hints.empty");
        } else if (page == Page::Code) {
            hintText = tr(code.size() == 6 ? "xeleenet.code.hints.join" : "xeleenet.code.hints.type");
        } else if (!hostNote.empty() && page == Page::Host) {
            hintText = hostNote;
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
        case Page::Lobbies: return pc_tr("xeleenet.lobbies.title");
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
            presence = 0.f;
            placedPresence = -1.f;
            document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
            refresh();
            layout();
        } else if (shown) {
            shown = false;
            document->Hide();
        }
    }

    // The screen scaled into the frame's inside, with the page's fade and rise.
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
        int i = -1;
        for (size_t k = 0; k < items.size(); k++)
            if (items[k].id == id)
                i = static_cast<int>(k);
        if (i < 0 || !items[i].enabled)
            return;
        if (event.GetId() == Rml::EventId::Click && !pointerWas) {
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
        if (page == Page::Lobbies && fetch != 0 && SDL_GetTicks() - fetchAsked > 10000)
            browse(); // B1 refreshes every 10 s while it's open
        if (!shown)
            return;
        build();
        layout();
#ifdef AURORA_ENABLE_OPENXR
        float px = 0.f, py = 0.f;
        bool down = false;
        auto* context = aurora::rmlui::get_context();
        if (context != nullptr && aurora_xr_screen_pointer(&px, &py, &down)) {
            context->ProcessMouseMove(static_cast<int>(px * dims.x), static_cast<int>(py * dims.y), 0);
            if (down && !pointerPressed)
                context->ProcessMouseButtonDown(0, 0);
            else if (!down && pointerPressed)
                context->ProcessMouseButtonUp(0, 0);
            pointerPressed = down;
            pointerWas = true;
        } else if (pointerWas && context != nullptr) {
            if (pointerPressed)
                context->ProcessMouseButtonUp(0, 0);
            context->ProcessMouseLeave();
            pointerWas = pointerPressed = false;
        }
#endif
    }
};

// ---- C1: the lobby's banner in the CSS --------------------------------------------

struct Banner {
    Rml::ElementDocument* document = nullptr;
    bool shown = false;
    std::string last;

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
        const std::string rules =
            host ? rules_line(s_host.teams, s_host.seats, s_host.stock, s_host.stocks, s_host.minutes,
                              kItemIds[s_host.items + 1])
                 : known ? rules_line(info.teams, info.seats, info.stock, info.stocks, info.time_min, info.items) : "";
        std::string code = v.code[0] ? std::string(v.code, 3) + " " + std::string(v.code + 3, 3) : "";
        if (host && s_host.hide_code)
            code = "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 \xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2";
        const int seats = host ? s_host.seats : known && info.seats ? info.seats : 4;
        const std::string r =
            "<div class=\"brand\"><span>" + enc(tr("xeleenet.banner.brand")) +
            "</span></div><div class=\"pill\"><div class=\"names\"><div class=\"bn\">" + enc(name) +
            "</div><div class=\"br\">" + enc(rules) + "</div></div><div class=\"seats\">" +
            std::to_string(std::max(1, v.nplayers)) + "/" + std::to_string(seats) + "</div>" +
            (code.empty() ? "" : "<div class=\"code\">" + enc(tr("xeleenet.banner.code", {{"code", code}})) + "</div>") +
            "</div>";
        if (r != last) {
            last = r;
            bar->SetInnerRML(r);
        }
    }
};

// ---- the stage select's overlay: the list's name, the legend

struct SssOverlay {
    Rml::ElementDocument* document = nullptr;
    bool shown = false;
    std::string last;
    unsigned mask = 0;

    void show(bool on) {
        if (on && document == nullptr) {
            auto* context = aurora::rmlui::get_context();
            if (context == nullptr)
                return;
            load_fonts();
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
        if (on)
            document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        else
            document->Hide();
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
        // Only the host takes everyone back to the CSS (mnstagesel.c).
        std::string keys = legend_key("stick", "xeleenet.legend.move") + legend_key("a", "xeleenet.legend.pick");
        if (pc_net_local_player() == 0)
            keys += legend_key("b", "xeleenet.legend.everyone_back");
        r += "<div id=\"bottom\" style=\"transform: scale(" + fmt(scale) + "); left: " + fmt(d.x / 2 - 560.f * scale) +
             "px; top: " + fmt(d.y - 70.f * scale) + "px;\"><div class=\"keys\">" + keys + "</div></div>";
        if (r != last) {
            last = r;
            root->SetInnerRML(r);
        }
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

} // namespace

extern "C" void pc_xn_menu_begin(int what) {
    host_load();
    const char* preview = std::getenv("MELEE_XN_PREVIEW");
    s_preview = preview != nullptr ? preview : "";
    s_what = what;
    s_result = PC_XN_MENU_STAY;
    s_active = true;
    s_screen.hostNote.clear();
    // The player's name comes first: asked for when there is none, or when
    // Name was picked.
    if (s_preview.empty() && (what == PC_XN_MENU_NAME || pc_xn_name()[0] == '\0'))
        ask_keyboard(Typing::Player, pc_xn_name(),
                     tr(pc_xn_name()[0] == '\0' ? "xeleenet.name.ask_first" : "xeleenet.name.ask"),
                     12);
    if (s_preview == "stages") {
        s_screen.go(Page::Stages);
    } else if (s_preview == "more") {
        s_screen.go(Page::More);
    } else if (s_preview == "code") {
        s_screen.code = "K7M";
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
    } else if (what == PC_XN_MENU_JOIN || s_preview == "lobbies") {
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
    }
    reload_styles();
    if (s_active)
        s_screen.update();
    if (s_banner.shown || s_preview == "banner") {
        if (s_preview == "banner" && !s_banner.shown) {
            const char* boot = std::getenv("MELEE_BOOT_SCENE");
            if (boot != nullptr && std::strcmp(boot, "css") == 0 && SDL_GetTicks() > 6000) {
                preset(kDoubles); // three players of four
                s_banner.show(true);
            }
        }
        s_banner.update();
    }
    s_sss.update();
}

extern "C" void pc_xn_banner_show(bool show) { s_banner.show(show); }

extern "C" void pc_xn_sss_frame(uint32_t mask) {
    s_sss.mask = mask;
    s_sss.show(true);
}

extern "C" void pc_xn_sss_show(bool show) { s_sss.show(show); }
