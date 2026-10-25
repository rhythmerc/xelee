/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Gameplay screen (gameplay_menu.h): its rows on the Options pages'
 * engine (settings_page.hpp). */
#include "gameplay_menu.h"

#include "fight_menu.h"
#include "settings.h"
#include "settings_page.hpp"
#include "xr_place.h"

#include <cstdlib>
#include <string>
#include <vector>

namespace {

using pc::settings::Kind;
using pc::settings::Row;

PcGameplaySettings s_play;

void save() { pc_settings_set_gameplay(&s_play); }

pc::settings::Page& page();

// A switch; `env` (MELEE_UCF and the like) overrides it when set.
Row toggle(const char* group, const char* name, const char* hint, bool PcGameplaySettings::*field,
           const char* env = nullptr) {
    Row r;
    r.group = group;
    r.name = name;
    r.hint = hint;
    r.get = [field] { return s_play.*field ? 1 : 0; };
    r.set = [field](int on) {
        s_play.*field = on != 0;
        save();
    };
    if (env != nullptr) {
        r.available = [env] { return std::getenv(env) == nullptr; };
        r.note = [env] { return std::getenv(env) != nullptr ? "SET BY ENVIRONMENT" : ""; };
    }
    return r;
}

// An action that leaves the page for `where` (PC_OPTIONS_*).
Row leave_to(const char* group, const char* name, const char* hint, const char* button, int where) {
    Row r;
    r.group = group;
    r.name = name;
    r.hint = hint;
    r.kind = Kind::Action;
    r.get = [] { return 0; };
    r.set = [where](int) { page().go(where); };
    r.label = [button](int) { return std::string(button); };
    return r;
}

std::vector<Row> rows() {
    pc_settings_gameplay(&s_play);
    std::vector<Row> v;
    v.push_back(toggle("FIGHTS", "Universal Controller Fix",
                       "UCF as Slippi plays it: dashbacks, shield drops and SDI on any controller.",
                       &PcGameplaySettings::ucf, "MELEE_UCF"));
    v.push_back(toggle(nullptr, "Slippi gameplay fixes",
                       "No wobbling, neutral spawns and the freeze-glitch fix offline. Always on online.",
                       &PcGameplaySettings::slippi_fixes, "MELEE_SLIPPI_FIXES"));
    v.push_back(toggle(nullptr, "C-Stick in 1P",
                       "Smash attacks and aerials on the C-stick in the 1P modes, not the camera.",
                       &PcGameplaySettings::cstick_1p, "MELEE_CSTICK_1P"));
    v.push_back(toggle(nullptr, "Hazardless stages",
                       "Pokemon Stadium keeps its neutral form, with no transformations.",
                       &PcGameplaySettings::frozen_stadium, "MELEE_FROZEN_STADIUM"));
    const bool quest = pc_settings_quest();
    if (!quest)
        v.push_back(toggle(nullptr, "Free camera", "Free the pause camera's rotation and zoom.",
                           &PcGameplaySettings::free_camera));
    if (quest || pc_xr_place_available()) {
        Row hold = toggle("STAGE", "Before each fight",
                          "Hold each fight's first frame to place the stage, or start right away.",
                          &PcGameplaySettings::place_hold);
        hold.kind = Kind::Choice;
        hold.label = [](int i) { return std::string(i == 1 ? "Place the stage" : "Start right away"); };
        v.push_back(hold);
        Row place = leave_to(nullptr, "Place stage", "Move, turn and size the stage now.", "Place",
                             PC_OPTIONS_PLACE_STAGE);
        place.available = [] { return pc_xr_place_available(); };
        v.push_back(place);
    }
    v.push_back(toggle("PROGRESS", "Unlock everything",
                       "All 26 characters, the 11 secret stages and All-Star mode, from the start.",
                       &PcGameplaySettings::unlock_all));
    v.push_back(leave_to(nullptr, "Clear game progress",
                         "Erase the unlocks, high scores, records or trophies, or all of it. It asks first.",
                         "Erase", PC_OPTIONS_ERASE_DATA));
    Row reset;
    reset.name = "Restore defaults";
    reset.hint = "Put every gameplay setting back to how a new install has it.";
    reset.kind = Kind::Action;
    reset.set = [](int) {
        pc_settings_gameplay_defaults(&s_play);
        save();
    };
    v.push_back(reset);
    return v;
}

pc::settings::Page s_page("options-page.rml", rows);

pc::settings::Page& page() { return s_page; }

} // namespace

extern "C" void pc_gameplay_menu_show(bool show) { s_page.show(show); }
extern "C" void pc_gameplay_menu_presence(float presence) { s_page.presence(presence); }
extern "C" bool pc_gameplay_menu_input(int what) { return s_page.input(what); }
extern "C" const char* pc_gameplay_menu_hint(void) { return s_page.hint(); }
extern "C" void pc_gameplay_menu_update(void) { s_page.update(); }
extern "C" int pc_gameplay_menu_leaving(void) { return s_page.leaving(); }
