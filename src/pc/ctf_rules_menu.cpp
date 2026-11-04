/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Capture the Flag's rules page (VS Mode > Game Modes > Capture the Flag,
 * docs/xeleenet-modes-plan.md §2.3 and §3.6), pc_ctf_rules_screen
 * (menu_screen.h). It stands in for Melee's Rules page before the CSS: the
 * mode's own rows, kept for next time (settings.h's PcCtfSettings, saved
 * with the launcher's preferences), and a bottom button on to the
 * characters, as START is from any row. Rows on the Options pages' engine
 * (settings_page.hpp), in the native page of src/melee/mn/mnpage.c, which
 * puts it in VS. Mode's red frame.
 *
 * The rest of a match's rules are Melee's own (Custom Rules: handicap,
 * damage ratio, friendly fire, pause, and the items when Items is on), so
 * this page has no "more rules" of its own.
 */
#include "lang.h"
#include "menu_screen.h"
#include "settings.h"
#include "settings_page.hpp"

#include <string>
#include <vector>

namespace {

using pc::settings::Kind;
using pc::settings::Row;

PcCtfSettings s_ctf;

// The lobby's steps (xeleenet_menu.cpp's kMinutes, kCtfReturns), so a rule
// reads the same offline and online.
const int kMinutes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 15, 20, 30};
const int kReturns[] = {0, 5, 10, 15, 20, 30, 45, 60};

void save() { pc_settings_set_ctf(&s_ctf); }

pc::settings::Page& page();

template <size_t N> int index_of(const int (&values)[N], int v) {
    for (size_t i = 0; i < N; i++)
        if (values[i] == v)
            return int(i);
    return 0;
}

// A choice over `values`, a field of the settings.
template <size_t N>
Row steps(const char* name, const char* hint, const int (&values)[N], int PcCtfSettings::*field,
          std::string (*label)(int)) {
    Row r;
    r.name = name;
    r.hint = hint;
    r.kind = Kind::Choice;
    r.count = int(N);
    r.get = [&values, field] { return index_of(values, s_ctf.*field); };
    r.set = [&values, field](int i) {
        s_ctf.*field = values[i];
        save();
    };
    r.label = [&values, label](int i) { return label(values[i]); };
    return r;
}

Row toggle(const char* name, const char* hint, bool PcCtfSettings::*field) {
    Row r;
    r.name = name;
    r.hint = hint;
    r.get = [field] { return s_ctf.*field ? 1 : 0; };
    r.set = [field](int on) {
        s_ctf.*field = on != 0;
        save();
    };
    return r;
}

std::string minutes_label(int m) {
    return m == 0 ? tr("modes.ctf.values.no_limit") : tr("modes.ctf.values.minutes", {{"n", m}});
}

std::string return_label(int s) {
    return s == 0 ? tr("modes.ctf.values.off") : tr("modes.ctf.values.seconds", {{"n", s}});
}

std::vector<Row> rows() {
    pc_settings_ctf(&s_ctf);
    std::vector<Row> v;
    Row caps;
    caps.group = pc_tr("modes.ctf.groups.win");
    caps.name = pc_tr("modes.ctf.rows.caps");
    caps.hint = pc_tr("modes.ctf.hints.caps");
    caps.kind = Kind::Choice;
    caps.count = 9;
    caps.get = [] { return s_ctf.captures - 1; };
    caps.set = [](int i) {
        s_ctf.captures = i + 1;
        save();
    };
    caps.label = [](int i) { return std::to_string(i + 1); };
    v.push_back(caps);
    v.push_back(steps(pc_tr("modes.ctf.rows.time"), pc_tr("modes.ctf.hints.time"), kMinutes,
                      &PcCtfSettings::minutes, minutes_label));
    Row home = toggle(pc_tr("modes.ctf.rows.home"), pc_tr("modes.ctf.hints.home"), &PcCtfSettings::home);
    home.group = pc_tr("modes.ctf.groups.flags");
    v.push_back(home);
    v.push_back(steps(pc_tr("modes.ctf.rows.return"), pc_tr("modes.ctf.hints.return"), kReturns,
                      &PcCtfSettings::return_s, return_label));
    v.push_back(toggle(pc_tr("modes.ctf.rows.slow"), pc_tr("modes.ctf.hints.slow"), &PcCtfSettings::slow));
    Row items = toggle(pc_tr("modes.ctf.rows.items"), pc_tr("modes.ctf.hints.items"), &PcCtfSettings::items);
    items.group = pc_tr("modes.ctf.groups.match");
    v.push_back(items);
    // The bottom button: on to the CSS, as START is from any row.
    Row go;
    go.name = pc_tr("modes.ctf.rows.start");
    go.hint = pc_tr("modes.ctf.hints.start");
    go.kind = Kind::Action;
    go.set = [](int) { page().go(PC_SCREEN_GO_START); };
    v.push_back(go);
    return v;
}

pc::settings::Page s_page("options-page.rml", rows);

pc::settings::Page& page() { return s_page; }

} // namespace

PC_SETTINGS_SCREEN(pc_ctf_rules_screen, s_page);
