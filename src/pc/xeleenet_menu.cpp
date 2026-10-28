/* SPDX-License-Identifier: GPL-3.0-or-later */
/* XeleeNet's pages (xeleenet_menu.h), drawn in the Slippi pages' line work
 * (slippi-setup.rcss, with xeleenet.rcss on top), and the CSS banner.
 *
 * For working on their look: MELEE_XN_PREVIEW=<page> shows a page with
 * sample data and no network (tools/xn_preview.py boots and screenshots
 * it), and F5 reloads every RmlUi stylesheet in the running game. */
#include "xeleenet_menu.h"

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
    const char* name;
};
const Stage kStages[] = {
    {24, "Battlefield"}, {25, "Final Dest."}, {26, "Dream Land"}, {4, "Yoshi's Story"}, {5, "Fountain"},
    {7, "Pok\xc3\xa9mon Std."}, {0, "Peach's Castle"}, {12, "Rainbow Cruise"}, {1, "Kongo Jungle"},
    {13, "Jungle Japes"}, {2, "Great Bay"}, {14, "Temple"}, {3, "Brinstar"}, {15, "Br. Depths"},
    {16, "Yoshi's Island"}, {17, "Green Greens"}, {6, "Corneria"}, {18, "Venom"}, {19, "Pok\xc3\xa9 Floats"},
    {8, "Mute City"}, {20, "Big Blue"}, {9, "Onett"}, {21, "Fourside"}, {10, "Icicle Mtn."},
    {11, "Mushroom K."}, {22, "Mushroom K. II"}, {23, "Flat Zone"}, {28, "Kongo 64"}, {27, "Yoshi's Is. 64"},
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
const char* const kPresetNames[kPresetCount] = {"1v1", "Doubles", "Free-for-all", "Custom"};
const char* const kPresetIds[kPresetCount] = {"1v1", "doubles", "ffa", "custom"};
const int kStocks[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 99};
const int kMinutes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 30};
const char* const kItemNames[] = {"Off", "Very low", "Low", "Medium", "High", "Very high"};
const char* const kItemIds[] = {"off", "very_low", "low", "medium", "high", "very_high"};
const char* const kHandicapNames[] = {"Off", "Auto", "On"};

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

std::string minutes_label(int m) { return m == 0 ? std::string("No limit") : std::to_string(m) + ":00"; }

std::string lobby_name() {
    return s_host.name.empty() ? std::string(pc_xn_name()) + "'s lobby" : s_host.name;
}

std::string stage_list_label(unsigned mask) {
    const char* kind = mask == kCompetitive ? "Competitive" : mask == kAllStages ? "All" : "Custom";
    return std::string(kind) + " (" + std::to_string(stage_count(mask)) + ")";
}

// "Doubles · 4 stocks · 8:00": a lobby in a line.
std::string rules_line(bool teams, int seats, bool stock, int stocks, int minutes, const char* items) {
    const std::string mode = teams ? "Doubles" : seats == 2 ? "1v1" : "Free-for-all";
    std::string r = mode + kDot +
                    (stock ? std::to_string(stocks) + (stocks == 1 ? " stock" : " stocks") : std::string("Time"));
    r += kDot + minutes_label(minutes);
    if (items != nullptr && items[0] != '\0' && std::strcmp(items, "off") != 0)
        r += std::string(kDot) + "Items";
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
    const char* hint = "";
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
    std::string failTitle, failLine, failSmall;
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

    void choice(std::string& r, const std::string& id, int col, int row, const char* name, const std::string& value,
                std::function<void(int)> change, const char* hint, bool enabled = true, const char* note = nullptr) {
        r += "<div id=\"" + id + "\" class=\"xrow" + (enabled ? "" : " off") + "\"><div class=\"n\">" + name +
             (note != nullptr ? "<span class=\"note\">" + std::string(note) + "</span>" : std::string()) + "</div>";
        r += "<div class=\"v\"><span class=\"arr\">" + std::string(kLeft) + "</span>" + enc(value) +
             "<span class=\"arr\">" + kRight + "</span></div></div>";
        Item it{id, col, row, enabled};
        it.change = std::move(change);
        it.hint = hint;
        items.push_back(std::move(it));
    }

    void link(std::string& r, const std::string& id, int col, int row, const char* name, const std::string& value,
              std::function<void()> act, const char* hint) {
        r += "<div id=\"" + id + "\" class=\"xrow link\"><div class=\"n\">" + name + "</div><div class=\"v\">" +
             enc(value) + "<span class=\"go\">" + kRight + "</span></div></div>";
        Item it{id, col, row, true};
        it.act = std::move(act);
        it.hint = hint;
        items.push_back(std::move(it));
    }

    static void group(std::string& r, const char* name) {
        r += "<div class=\"grp\"><span>" + std::string(name) + "</span></div>";
    }

    void build_host() {
        HostSettings& h = s_host;
        auto custom = [] { s_host.preset = kCustom; };
        std::string r;
        r += "<div id=\"preset\" class=\"pbar\"><div class=\"k\">PRESET</div><div class=\"pv\"><span class=\"arr\">" +
             std::string(kLeft) + "</span>" + kPresetNames[h.preset] + "<span class=\"arr\">" + kRight +
             "</span></div><div class=\"pk\">1v1" + kDot + "Doubles" + kDot + "Free-for-all" + kDot +
             "Custom</div></div>";
        {
            Item it{"preset", 0, 0, true};
            it.change = [](int d) { preset((s_host.preset + d + kPresetCount) % kPresetCount); };
            it.hint = "1v1, Doubles or Free-for-all set every rule at once.";
            items.push_back(std::move(it));
        }
        r += "<div class=\"col left\">";
        group(r, "LOBBY");
        r += "<div id=\"lname\" class=\"xrow link\"><div class=\"n\">Name</div><div class=\"v\">" + enc(lobby_name()) +
             "<span class=\"type\">TYPE</span></div></div>";
        {
            Item it{"lname", 0, 1, true};
            it.act = [] {
                ask_keyboard(Typing::Lobby, s_host.name.empty() ? lobby_name() : s_host.name,
                             "Name your lobby, up to 20 characters.", 20);
            };
            it.hint = "The lobby's name in Join's list.";
            items.push_back(std::move(it));
        }
        choice(r, "vis", 0, 2, "Visibility", h.open ? "Public" : "Private", [](int) { s_host.open = !s_host.open; },
               "Public lobbies are listed in Join. Private ones open only with their code.");
        choice(r, "seats", 0, 3, "Seats", std::to_string(h.seats),
               [custom](int d) { s_host.seats = 2 + (s_host.seats - 2 + d + 3) % 3, custom(); },
               "How many players the lobby takes, you included.");
        group(r, "RULES");
        choice(r, "teams", 0, 4, "Teams", h.teams ? "On" : "Off",
               [custom](int) { s_host.teams = !s_host.teams, custom(); },
               "Teams on: everyone picks a team on their door in the CSS.");
        if (h.stock)
            choice(r, "stocks", 0, 5, "Stocks", std::to_string(h.stocks),
                   [custom](int d) { s_host.stocks = step(kStocks, s_host.stocks, d), custom(); },
                   "Stocks per player. START: open lobby");
        else
            choice(r, "stocks", 0, 5, "Win by", "Time", [custom](int) { s_host.stock = true, custom(); },
                   "Time: most KOs when the clock runs out.");
        choice(r, "time", 0, 6, "Time limit", minutes_label(h.minutes),
               [custom](int d) { s_host.minutes = step(kMinutes, s_host.minutes, d), custom(); },
               "How long a game lasts.");
        choice(r, "items", 0, 7, "Items", kItemNames[h.items + 1],
               [custom](int d) { s_host.items = (s_host.items + 1 + d + 6) % 6 - 1, custom(); },
               "How often items appear.");
        if (h.teams)
            choice(r, "ff", 0, 8, "Friendly fire", h.friendly_fire ? "On" : "Off",
                   [custom](int) { s_host.friendly_fire = !s_host.friendly_fire, custom(); },
                   "Teammates' attacks hurt each other.");
        link(r, "more", 0, 9, "More rules", "Time mode, pause, handicap", [this] { go(Page::More); },
             "Stock or time, pause, handicap and damage ratio.");
        r += "</div><div class=\"col right\">";
        group(r, "STAGES");
        link(r, "slist", 1, 1, "Stage list", stage_list_label(h.stages), [this] { go(Page::Stages); },
             "The stages random picks draw from.");
        choice(r, "pick", 1, 2, "Stage pick", "Everyone picks", [](int) {},
               "Everyone picks, then one of the picks is drawn.", false, "SOON");
        choice(r, "frozen", 1, 3, "Frozen Stadium", h.frozen ? "On" : "Off",
               [custom](int) { s_host.frozen = !s_host.frozen, custom(); },
               "Pok\xc3\xa9mon Stadium stays in its neutral form.");
        choice(r, "place", 1, 4, "Placement time", "15 s", [](int) {},
               "How long headset players get to place the stage before each match.", false, "SOON");
        group(r, "SET");
        choice(r, "format", 1, 5, "Format", "Endless", [](int) {}, "First to N wins, or endless.", false, "SOON");
        group(r, "JOINING");
        choice(r, "ask", 1, 6, "Ask before joining", "Off", [](int) {}, "Each join waits for your OK in the CSS.",
               false, "SOON");
        choice(r, "hide", 1, 7, "Hide code", h.hide_code ? "On" : "Off",
               [](int) { s_host.hide_code = !s_host.hide_code; }, "The CSS banner masks the code, for streaming.");
        r += "<button id=\"open\" class=\"gold\">OPEN LOBBY</button></div>";
        {
            Item it{"open", 1, 9, true};
            it.act = [this] { open_lobby(); };
            it.hint = "Open the lobby. You go to the CSS; friends join with its code.";
            items.push_back(std::move(it));
        }
        set_board(r);
    }

    void build_more() {
        HostSettings& h = s_host;
        auto custom = [] { s_host.preset = kCustom; };
        std::string r = "<div class=\"col single\">";
        group(r, "MORE RULES");
        choice(r, "win", 0, 0, "Win by", h.stock ? "Stock" : "Time",
               [custom](int) { s_host.stock = !s_host.stock, custom(); },
               "Stock: last one standing. Time: most KOs when the clock runs out.");
        choice(r, "pause", 0, 1, "Pause", h.pause ? "On" : "Off",
               [custom](int) { s_host.pause = !s_host.pause, custom(); }, "Whether START pauses a match.");
        choice(r, "handicap", 0, 2, "Handicap", kHandicapNames[h.handicap],
               [custom](int d) { s_host.handicap = (s_host.handicap + d + 3) % 3, custom(); },
               "Melee's handicap: more knockback resistance for whoever is behind.");
        char dmg[16];
        std::snprintf(dmg, sizeof dmg, "%.1f\xc3\x97", h.damage / 10.f);
        choice(r, "damage", 0, 3, "Damage ratio", dmg,
               [custom](int d) { s_host.damage = 5 + (s_host.damage - 5 + d + 16) % 16, custom(); },
               "How much damage every hit does.");
        r += "<button id=\"done\" class=\"gold\">DONE</button></div>";
        Item it{"done", 0, 4, true};
        it.act = [this] { go(Page::Host, "more"); };
        it.hint = "Back to the lobby's settings.";
        items.push_back(std::move(it));
        set_board(r);
    }

    void build_stages() {
        const unsigned m = s_host.stages;
        std::string r = "<div class=\"chips\">";
        const char* chips[3] = {"COMPETITIVE", "ALL", "CUSTOM"};
        const bool on[3] = {m == kCompetitive, m == kAllStages, m != kCompetitive && m != kAllStages};
        for (int i = 0; i < 3; i++) {
            const std::string id = "chip" + std::to_string(i);
            r += "<div id=\"" + id + "\" class=\"chipx" + (on[i] ? " on" : "") + (i == 2 ? " dash" : "") + "\">" +
                 chips[i] + "</div>";
            Item it{id, i, 0, i < 2};
            it.act = [i] {
                s_host.stages = i == 0 ? kCompetitive : kAllStages;
                s_host.preset = kCustom;
            };
            it.hint = "A: on / off   X: next preset   B: done";
            items.push_back(std::move(it));
        }
        r += "<div class=\"count\">" + std::to_string(stage_count(m)) + " of " + std::to_string(kStageCount) +
             " on</div></div><div id=\"grid\">";
        for (int i = 0; i < kStageCount; i++) {
            const Stage& s = kStages[i];
            const std::string id = "st" + std::to_string(i);
            r += "<div id=\"" + id + "\" class=\"tile" + ((m >> s.bit) & 1 ? " on" : "") + "\">" + enc(s.name) +
                 "</div>";
            Item it{id, i % kGridAcross, 1 + i / kGridAcross, true};
            it.act = [b = s.bit] {
                const unsigned want = s_host.stages ^ (1u << b);
                if (want != 0) // one stage at least stays on
                    s_host.stages = want;
                s_host.preset = kCustom;
            };
            it.hint = "A: on / off   X: next preset   B: done";
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
            hostNote = "This build has no XeleeNet server set.";
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
        PcXnListing out[16];
        int n = 0, o = 0;
        const int st = pc_xn_browse_poll(out, 16, &n, &o);
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
                        "\">SHOW FULL: " + (showFull ? "ON" : "OFF") + "</div><div class=\"upd\">";
        if (fetch == 0)
            r += "Looking for lobbies...";
        else
            r += "Updated " + std::to_string((SDL_GetTicks() - fetchedAt) / 1000) + " s ago" + kDot + "X refresh";
        r += "</div></div><div id=\"list\">";
        if (fetch < 0) {
            const char* reason = nullptr;
            const char* err = pc_xn_error(&reason);
            r += "<div class=\"empty bad\">" + enc(err != nullptr && err[0] ? err : "Couldn't reach XeleeNet") +
                 "</div>";
        } else if (rows.empty() && fetch == 1) {
            r += "<div class=\"empty\">No public lobbies right now. Host one, or join a friend's with its code.</div>";
        }
        for (size_t i = 0; i < rows.size(); i++) {
            const PcXnListing& x = rows[i];
            const std::string id = "lb" + std::to_string(i);
            const bool full = x.taken >= x.seats;
            const char* mode = x.teams ? "DOUBLES" : x.seats == 2 ? "1V1" : "FFA";
            const std::string sub = std::string(x.host) + (x.tag[0] ? "#" + std::string(x.tag).substr(0, 4) : "") +
                                    kDot + (x.stock ? std::to_string(x.stocks) + " stocks" : std::string("Time")) + kDot +
                                    minutes_label(x.time_min) + kDot +
                                    (x.first_to ? "First to " + std::to_string(x.first_to_n) : std::string("Endless"));
            r += "<div id=\"" + id + "\" class=\"lrow" + (full ? " full" : "") + "\"><div class=\"lw\"><div class=\"ln\">" +
                 enc(x.name) + "</div><div class=\"ls\">" + enc(sub) + "</div></div><div class=\"lm\">" + mode +
                 "</div><div class=\"lb\">" +
                 (x.playing ? "<span class=\"badge game\">IN GAME</span>"
                            : x.xr ? "<span class=\"badge\">XR</span>" : "") +
                 "</div><div class=\"lt\">" + std::to_string(x.taken) + "/" + std::to_string(x.seats) +
                 "</div><div class=\"lc\">" + enc(x.country) + "</div></div>";
            Item it{id, 0, static_cast<int>(i), !full};
            it.act = [this, i] {
                joining = rows[i];
                joiningKnown = true;
                code = rows[i].code;
                join();
            };
            it.hint = "A: join   Y: join with code   B: back";
            items.push_back(std::move(it));
        }
        r += "</div>";
        if (others > 0)
            r += "<div class=\"foot\">" + std::to_string(others) +
                 (others == 1 ? " more lobby is" : " more lobbies are") +
                 " on another Xelee version. Update to see them.</div>";
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
            it.hint = "Type the 6-letter code the host shared.";
            items.push_back(std::move(it));
        }
        r += "<div id=\"k-ok\" class=\"key join\">JOIN</div></div>";
        Item it{"k-ok", 5, n / 9, true};
        it.act = [this] { join(); };
        it.hint = "START: join this lobby.";
        items.push_back(std::move(it));
        set_board(r);
    }

    // J2-J4 share the track and the card.
    std::string track(int at, bool failed) const {
        const char* labels[3] = {"FIND", "CONNECT", "LOBBY"};
        std::string r = "<div id=\"track\"><div class=\"line\"></div><div class=\"line done\" style=\"width: " +
                        fmt(at <= 0 ? 0.f : 50.f * std::min(at, 2)) + "%;\"></div>";
        for (int i = 0; i < 3; i++)
            r += "<div class=\"step" + std::string(at > i ? " done" : "") +
                 (at == i ? (failed ? " bad" : " now") : "") + "\" style=\"left: " + fmt(50.f * i) +
                 "%;\"><div class=\"dot\"><div class=\"mark\"></div></div><div class=\"lbl\">" + labels[i] +
                 "</div></div>";
        return r + "</div>";
    }

    std::string lobby_title() const {
        PcXnListing info;
        if (pc_xn_lobby_info(&info) && info.name[0])
            return info.name;
        if (joiningKnown && joining.name[0])
            return joining.name;
        return "Lobby " + code.substr(0, 3) + " " + code.substr(std::min<size_t>(3, code.size()));
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
            r += "<div class=\"head\">Game in progress</div><div class=\"rules\">" + enc(lobby_title()) + "</div>";
            std::string names;
            for (int i = 0; i < v.nplayers; i++)
                if (!v.players[i].local)
                    names += (names.empty() ? "" : ", ") + std::string(v.players[i].name);
            r += "<div class=\"text\">" + enc(names) +
                 "</div><div class=\"fine\">You'll be seated when they're back on the CSS.</div>";
        } else {
            r += "<div class=\"head\">" + enc(lobby_title()) + "</div>";
            const std::string rl = lobby_rules();
            if (!rl.empty())
                r += "<div class=\"rules\">" + enc(rl) + "</div>";
            r += "<div class=\"players\">";
            for (int i = 0; i < v.nplayers; i++) {
                const auto& p = v.players[i];
                const char* how = p.local      ? "you"
                                  : p.linked   ? "connected"
                                  : p.via_host ? "via host"
                                               : "trying direct...";
                r += "<div class=\"player" + std::string(p.linked ? " ok" : "") + (p.local ? " me" : "") +
                     "\"><div class=\"name\">" + enc(p.name) + "</div>" +
                     (p.port == 0 ? "<div class=\"badge\">HOST</div>" : "") + "<div class=\"how\">" + how +
                     "</div></div>";
            }
            r += "</div>";
            r += v.nplayers == 0 ? "<div class=\"wait\">Finding the lobby...</div>"
                                 : "<div class=\"fine\">The host must answer. Others can go through the host.</div>";
        }
        r += "</div>";
        set_board(r);
    }

    void build_failed() {
        std::string r = track(failStep, true);
        r += "<div id=\"lobbycard\" class=\"failed\"><div class=\"head bad\">" + enc(failTitle) +
             "</div><div class=\"why\">" + enc(failLine) + "</div>";
        if (!failSmall.empty())
            r += "<div class=\"fine\">" + enc(failSmall) + "</div>";
        r += "</div><div class=\"buttons fail\">";
        if (failCode) {
            r += "<button id=\"edit\" class=\"btn primary\">EDIT CODE</button>";
            Item it{"edit", 0, 0, true};
            it.act = [this] {
                pc_xn_punch_end();
                go(Page::Code);
            };
            items.push_back(std::move(it));
        } else {
            r += "<button id=\"retry\" class=\"btn primary\">TRY AGAIN</button>";
            Item it{"retry", 0, 0, true};
            it.act = [this] {
                pc_xn_punch_end();
                join();
            };
            items.push_back(std::move(it));
        }
        r += "<button id=\"back\" class=\"btn\">BACK</button></div>";
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
        failSmall.clear();
        failStep = 0;
        if (why == "not_found" || why == "bad_code") {
            failTitle = "No lobby with that code";
            failLine = "Check the code with the host. Codes end when the host leaves.";
            failCode = true;
        } else if (why == "full") {
            failTitle = "Lobby is full", failLine = "Someone joined first.";
        } else if (why == "declined") {
            failTitle = "The host declined", failLine = "Try another lobby.";
        } else if (why == "version") {
            failTitle = "Different Xelee version", failLine = "Both of you need the same Xelee version.";
        } else if (why == "banned") {
            failTitle = "You were removed from this lobby", failLine = "The host removed you.";
        } else if (why == "closed") {
            failTitle = "The lobby closed", failLine = "The host closed it. Try another lobby.";
        } else if (punch.find("host") != std::string::npos || s_preview == "failed") {
            PcXnPunchView v;
            view(&v);
            const std::string host = v.nplayers > 0 && v.players[0].port == 0 ? v.players[0].name : "the host";
            failTitle = "Couldn't reach the host, " + host;
            failLine = "One of your networks blocks direct connections (a strict router, or a phone hotspot).";
            failSmall = "Try another lobby, or host one and have them join you.";
            failStep = 1;
        } else {
            failTitle = "XeleeNet is unreachable";
            failLine = err != nullptr && err[0] ? std::string(err) + ". Check your internet, then try again."
                                                : "Check your internet, then try again.";
        }
        pc_log_line("xeleenet pages: join failed: %s (%s)", failTitle.c_str(), why.c_str());
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

    // ---- the board ------------------------------------------------------------

    void set_board(const std::string& r) {
        if (r == last) {
            mark();
            return;
        }
        last = r;
        auto* board = document->GetElementById("board");
        board->SetInnerRML(r);
        const char* kinds[] = {"host", "stages", "more", "lobbies", "code", "joining", "failed"};
        for (int i = 0; i < 7; i++)
            board->SetClass(kinds[i], static_cast<int>(page) == i);
        mark();
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
            mark();
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
            hintText = n > 0 ? "Connecting to " + std::to_string(n) + (n == 1 ? " player" : " players") +
                                   "...  B: cancel"
                             : "Finding the lobby...  B: cancel";
        } else if (page == Page::Failed) {
            hintText = "This attempt is saved to your logs.";
        } else if (page == Page::Lobbies) {
            hintText = "A: join   Y: join with code   B: back";
        } else if (page == Page::Code) {
            hintText = code.size() == 6 ? "START: join this lobby." : "Type the 6-letter code the host shared.";
        } else if (!hostNote.empty() && page == Page::Host) {
            hintText = hostNote;
        } else {
            hintText = items.empty() ? "" : items[cursor].hint;
        }
        return hintText.c_str();
    }

    const char* title() const {
        switch (page) {
        case Page::Host: return "Host";
        case Page::More: return "Rules";
        case Page::Stages: return "Stages";
        case Page::Lobbies: return "Lobbies";
        case Page::Code: return "Code";
        case Page::Joining:
        case Page::Failed: return "Joining";
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
        const std::string name = host ? lobby_name() : known ? info.name : "XeleeNet lobby";
        const std::string rules =
            host ? rules_line(s_host.teams, s_host.seats, s_host.stock, s_host.stocks, s_host.minutes,
                              kItemIds[s_host.items + 1])
                 : known ? rules_line(info.teams, info.seats, info.stock, info.stocks, info.time_min, info.items) : "";
        std::string code = v.code[0] ? std::string(v.code, 3) + " " + std::string(v.code + 3, 3) : "";
        if (host && s_host.hide_code)
            code = "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2 \xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2";
        const int seats = host ? s_host.seats : known && info.seats ? info.seats : 4;
        const std::string r = "<div class=\"brand\">XELEENET</div><div class=\"pill\"><div class=\"names\"><div "
                              "class=\"bn\">" + enc(name) + "</div><div class=\"br\">" + enc(rules) +
                              "</div></div><div class=\"seats\">" + std::to_string(std::max(1, v.nplayers)) + "/" +
                              std::to_string(seats) + "</div>" +
                              (code.empty() ? "" : "<div class=\"code\">CODE " + enc(code) + "</div>") + "</div>";
        if (r != last) {
            last = r;
            bar->SetInnerRML(r);
        }
    }
};

Screen s_screen;
Banner s_banner;
bool s_active = false;
bool s_shown = false;
bool s_f5_was = false;

// F5: every document's stylesheets again from disk, for working on the look.
void reload_styles() {
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const bool down = keys != nullptr && keys[SDL_SCANCODE_F5];
    if (down && !s_f5_was) {
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
                     pc_xn_name()[0] == '\0' ? "Pick a name for XeleeNet. Others see it, and it can repeat."
                                             : "Your XeleeNet name.",
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

extern "C" const char* pc_xn_menu_title(void) { return s_what == PC_XN_MENU_NAME ? "Name" : s_screen.title(); }

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
            if (boot != nullptr && std::strcmp(boot, "css") == 0 && SDL_GetTicks() > 6000)
                s_banner.show(true);
        }
        s_banner.update();
    }
}

extern "C" void pc_xn_banner_show(bool show) { s_banner.show(show); }
