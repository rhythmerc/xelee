/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Gameplay screen (Options > Gameplay, in the stock Erase Data row's
 * place; the settings are settings.h's, the launcher's own),
 * pc_gameplay_screen (menu_screen.h). The fights' rules: UCF, Slippi's
 * fixes, the C-stick in 1P, hazardless stages and (on a screen) the free
 * camera; on the Quest the stage's placing, Before each fight and Place
 * stage now; then progress: Unlock everything, and Clear game progress,
 * which opens Melee's Erase Data page. Each applies as the game runs. Rows
 * on the Options pages' engine (settings_page.hpp), in the native page of
 * src/melee/mn/mnpage.c.
 */
#include "menu_screen.h"
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

// An action that leaves the page for `where` (PC_SCREEN_GO_*).
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
                             PC_SCREEN_GO_PLACE_STAGE);
        place.available = [] { return pc_xr_place_available(); };
        v.push_back(place);
    }
    v.push_back(toggle("PROGRESS", "Unlock everything",
                       "All 26 characters, the 11 secret stages and All-Star mode, from the start.",
                       &PcGameplaySettings::unlock_all));
    v.push_back(leave_to(nullptr, "Clear game progress",
                         "Erase the unlocks, high scores, records or trophies, or all of it. It asks first.",
                         "Erase", PC_SCREEN_GO_ERASE_DATA));
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

PC_SETTINGS_SCREEN(pc_gameplay_screen, s_page);
