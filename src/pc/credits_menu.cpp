/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Options > Credits: who and what Xelee is built from. A list under group
 * headings; up and down move the cursor and scroll, and the hint box says
 * what the lit entry is for. A screen of its own (menu_screen.h) in the
 * settings pages' look (credits.rcss). licenses/LICENSE.md has the full
 * terms; this is the in-game credit, which some of them ask for (DB-IP's
 * CC BY: the country in the XeleeNet lobby browser). */
#include "menu_screen.h"
#include "launcher.h"
#include "pc.h"

#include <aurora/rmlui.hpp>
#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif

#include <RmlUi/Core.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

extern "C" void lbAudioAx_80024030(int);

namespace {

enum { SFX_MOVE = 2 };

struct Credit {
    const char* group; // a heading above this entry
    const char* name;
    const char* by;    // who, and the license
    const char* about; // the hint box, while lit
};

const Credit kCredits[] = {
    {"XELEE", "Super Smash Bros. Melee", "Nintendo / HAL Laboratory",
     "Melee itself. Its art, sound and text come from your own disc."},
    {nullptr, "doldecomp/melee", "the decompilation's contributors", "The Melee decompilation Xelee is built on."},
    {nullptr, "melee-pc", "999sian and contributors, GPL-3.0", "The PC port of Melee that Xelee is forked from."},
    {nullptr, "Aurora", "Luke Street, MIT", "GameCube graphics, input and audio on modern machines."},
    {nullptr, "Project Slippi", "the Slippi team", "Online play with Slippi players, and the replay format."},
    {nullptr, "Roomtone", "MIT", "Room light for the stage in mixed reality."},

    {"LIBRARIES", "Dawn", "Google, BSD-3-Clause", "WebGPU: drawing on every platform."},
    {nullptr, "SDL", "zlib License", "Windows, input, controllers and sound."},
    {nullptr, "RmlUi", "MIT", "These menus' pages and overlays."},
    {nullptr, "Steam Audio", "Valve, Apache-2.0", "Positional sound effects."},
    {nullptr, "OpenXR loader", "Khronos Group, Apache-2.0", "The headset."},
    {nullptr, "nod", "Apache-2.0", "Reading the disc image."},
    {nullptr, "ENet", "Lee Salzman, MIT", "Online play's connections."},
    {nullptr, "Monocypher", "Loup Vaillant, BSD-2-Clause", "Online identities and signatures."},
    {nullptr, "dht", "Juliusz Chroboczek, MIT", "Finding opponents without a server."},
    {nullptr, "rapidyaml", "MIT", "Reading the game's text."},
    {nullptr, "FreeType", "FreeType License", "Drawing text."},
    {nullptr, "libpng", "libpng License", "Reading pictures."},
    {nullptr, "zlib-ng", "zlib License", "Compression."},
    {nullptr, "Zstandard", "Meta, BSD-3-Clause", "Compression."},
    {nullptr, "xxHash", "Yann Collet, BSD-2-Clause", "Fast hashing for the caches."},
    {nullptr, "Abseil", "Google, Apache-2.0", "Building blocks under Dawn."},
    {nullptr, "{fmt}", "MIT", "Formatting text."},
    {nullptr, "Dear ImGui", "Omar Cornut, MIT", "The developer panels."},
    {nullptr, "SQLite", "public domain", "The shader and pipeline caches."},

    {"FONTS", "Noto Sans", "The Noto Project Authors, SIL OFL 1.1", "The menus' and launcher's text."},
    {nullptr, "Liberation Sans", "SIL OFL 1.1", "Text the menus' face has no letters for."},

    {"ART", "GameCube button icons", "SmashWiki, CC BY-SA 4.0", "The button pictures in prompts and legends."},
    {nullptr, "Gamepad pictures", "thehoodieguy02 (OpenGameArt), CC0", "Fight Controls' gamepad."},
    {nullptr, "Touch controller pictures", "Meta Quest Hardware Art", "Fight Controls' Touch controllers."},

    {"DATA", "IP Geolocation by DB-IP", "db-ip.com, CC BY 4.0",
     "The countries in XeleeNet's lobby list, and nearest first."},
};
constexpr int kCount = sizeof kCredits / sizeof kCredits[0];

// The layout, in px, scaled to fit the panel frame's inside (options-page's).
constexpr float kScreenW = 1120.f, kScreenH = 660.f;
constexpr float kRowW = 900.f, kRowH = 44.f, kRowGap = 6.f, kGroupH = 36.f;
constexpr float kRowX = (kScreenW - kRowW) / 2;
constexpr float kTrackX = kRowX + kRowW + 24.f;

struct State {
    Rml::ElementDocument* document = nullptr;
    std::vector<Rml::Element*> rows;
    std::vector<float> top; // each row's, on the board
    // Every row and heading with its top and height: those not wholly in
    // view are hidden (Aurora's RmlUi doesn't clip a transformed overflow).
    struct Item {
        Rml::Element* e;
        float top, height;
    };
    std::vector<Item> items;
    float height = 0.f;     // the board's
    float scroll = 0.f;
    bool shown = false;
    int cursor = 0;
    float presence = 0.f, placedPresence = -1.f;
    Rml::Vector2i dims{0, 0}, placedDims{0, 0};
    bool pointerWas = false;
} s;

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

std::string escape(const char* text) {
    std::string out;
    for (const char* c = text; *c != '\0'; c++) {
        switch (*c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += *c; break;
        }
    }
    return out;
}

class Listener : public Rml::EventListener {
public:
    void ProcessEvent(Rml::Event& event) override;
} s_listener;

void build() {
    auto* board = s.document->GetElementById("board");
    std::string rml;
    float y = 0.f;
    s.top.clear();
    for (int i = 0; i < kCount; i++) {
        const Credit& c = kCredits[i];
        if (c.group != nullptr) {
            if (i > 0)
                y += 10.f;
            rml += "<div class='group' style='left: " + fmt(kRowX) + "px; top: " + fmt(y + 7.f) + "px; width: " +
                   fmt(kRowW) + "px;'><div class='rule'></div><div class='label'>" + escape(c.group) +
                   "</div></div>";
            y += kGroupH;
        }
        s.top.push_back(y);
        rml += "<div class='row' data-row='" + std::to_string(i) + "' style='left: " + fmt(kRowX) +
               "px; top: " + fmt(y) + "px; width: " + fmt(kRowW) + "px; height: " + fmt(kRowH) +
               "px;'><div class='name'>" + escape(c.name) + "</div><div class='by'>" + escape(c.by) +
               "</div></div>";
        y += kRowH + kRowGap;
    }
    s.height = y;
    board->SetInnerRML(rml);
    board->SetProperty("height", fmt(s.height) + "px");
    Rml::ElementList found;
    board->GetElementsByClassName(found, "row");
    s.rows.assign(found.begin(), found.end());
    s.items.clear();
    for (int i = 0; i < board->GetNumChildren(); i++) {
        Rml::Element* e = board->GetChild(i);
        const float top = e->GetProperty<float>("top");
        s.items.push_back({e, top, e->IsClassSet("row") ? kRowH : 22.f});
    }
}

// Keeps the cursor's row in view (with its group heading for the first of
// a group), and draws the scroll bar.
void scroll_to_cursor() {
    const float top = s.top[s.cursor] - (kCredits[s.cursor].group != nullptr ? kGroupH : 0.f);
    const float bottom = s.top[s.cursor] + kRowH;
    if (top < s.scroll)
        s.scroll = top;
    if (bottom > s.scroll + kScreenH)
        s.scroll = bottom - kScreenH;
    s.scroll = std::clamp(s.scroll, 0.f, std::max(0.f, s.height - kRowGap - kScreenH));
    s.document->GetElementById("board")->SetProperty("top", fmt(-s.scroll) + "px");
    for (const State::Item& it : s.items) {
        const bool in = it.top >= s.scroll - 0.5f && it.top + it.height <= s.scroll + kScreenH + 0.5f;
        it.e->SetProperty("visibility", in ? "visible" : "hidden");
    }

    auto* track = s.document->GetElementById("track");
    auto* thumb = s.document->GetElementById("thumb");
    const float h = s.height - kRowGap;
    if (h <= kScreenH) {
        track->SetProperty("display", "none");
        return;
    }
    track->SetProperty("display", "block");
    track->SetProperty("left", fmt(kTrackX) + "px");
    track->SetProperty("height", fmt(kScreenH) + "px");
    thumb->SetProperty("top", fmt(s.scroll / h * kScreenH) + "px");
    thumb->SetProperty("height", fmt(kScreenH / h * kScreenH) + "px");
}

void refresh() {
    for (int i = 0; i < static_cast<int>(s.rows.size()); i++)
        s.rows[i]->SetClass("cursor", i == s.cursor);
    scroll_to_cursor();
}

bool load() {
    if (s.document != nullptr)
        return true;
    auto* context = aurora::rmlui::get_context();
    if (context == nullptr)
        return false;
    const std::string dir = pc_resources_dir();
    for (const char* face : {"menu.ttf", "menu-bold.ttf"})
        Rml::LoadFontFace(dir + face);
    s.document = context->LoadDocument(dir + "credits.rml");
    if (s.document == nullptr) {
        pc_log_line("credits: could not load credits.rml");
        return false;
    }
    s.document->AddEventListener(Rml::EventId::Mouseover, &s_listener);
    build();
    return true;
}

void layout() {
    auto* context = aurora::rmlui::get_context();
    auto* screen = s.document->GetElementById("screen");
    if (context == nullptr || screen == nullptr)
        return;
    const Rml::Vector2i d = context->GetDimensions();
    s.dims = d;
    if (d == s.placedDims && s.presence == s.placedPresence)
        return;
    s.placedDims = d;
    s.placedPresence = s.presence;
    // The frame's inside, measured on the Erase Data frames, as Controls'.
    const float x0 = 0.123f * d.x, y0 = 0.170f * d.y;
    const float w = 0.755f * d.x, h = 0.672f * d.y;
    const float p = std::clamp(s.presence, 0.f, 1.f);
    const float ease = 1.f - (1.f - p) * (1.f - p);
    const float scale = std::min(w / kScreenW, h / kScreenH);
    screen->SetProperty("transform", "scale(" + fmt(scale) + ")");
    screen->SetProperty("left", fmt(x0 + (w - kScreenW * scale) / 2) + "px");
    screen->SetProperty("top", fmt(y0 + (h - kScreenH * scale) / 2 + (1.f - ease) * 10.f * scale) + "px");
    screen->SetProperty("opacity", fmt(ease));
}

void Listener::ProcessEvent(Rml::Event& event) {
    if (!s.shown || s.presence < 1.f || event.GetId() != Rml::EventId::Mouseover)
        return;
    for (Rml::Element* e = event.GetTargetElement(); e != nullptr; e = e->GetParentNode()) {
        if (const Rml::Variant* v = e->GetAttribute("data-row")) {
            const int row = std::atoi(v->Get<Rml::String>().c_str());
            if (row != s.cursor) {
                s.cursor = row;
                lbAudioAx_80024030(SFX_MOVE);
                refresh();
            }
            return;
        }
    }
}

void show(bool show) {
    if (!show) {
        if (s.shown) {
            s.shown = false;
            s.document->Hide();
        }
        return;
    }
    if (s.shown || !load())
        return;
    s.shown = true;
    s.cursor = 0;
    s.scroll = 0.f;
    s.presence = 0.f;
    s.placedPresence = -1.f;
    refresh();
    s.document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    layout();
}

bool input(int what) {
    switch (what) {
    case PC_SCREEN_UP:
    case PC_SCREEN_DOWN:
        s.cursor = (s.cursor + (what == PC_SCREEN_UP ? kCount - 1 : 1)) % kCount;
        lbAudioAx_80024030(SFX_MOVE);
        refresh();
        return true;
    case PC_SCREEN_LEFT:
    case PC_SCREEN_RIGHT:
    case PC_SCREEN_CONFIRM:
        return true;
    default:
        return false; // B: back to Options
    }
}

const char* hint() {
    return s.shown ? kCredits[s.cursor].about : nullptr;
}

void update() {
    if (!s.shown)
        return;
    layout();
#ifdef AURORA_ENABLE_OPENXR
    // The lasers: the mouse on the screen (hover moves the cursor).
    float px = 0.f, py = 0.f;
    bool down = false;
    auto* context = aurora::rmlui::get_context();
    if (context != nullptr && aurora_xr_screen_pointer(&px, &py, &down)) {
        context->ProcessMouseMove(static_cast<int>(px * s.dims.x), static_cast<int>(py * s.dims.y), 0);
        s.pointerWas = true;
    } else if (s.pointerWas && context != nullptr) {
        context->ProcessMouseLeave();
        s.pointerWas = false;
    }
#endif
}

} // namespace

extern "C" const PcMenuScreen pc_credits_screen = {
    show,
    [](float presence) { s.presence = presence; },
    input,
    hint,
    update,
    nullptr,
};
