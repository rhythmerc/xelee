/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Audio screen (audio_menu.h): its rows on the Options pages' engine
 * (settings_page.hpp). */
#include "audio_menu.h"

#include "settings.h"
#include "settings_page.hpp"

#include <cmath>
#include <string>

namespace {

using pc::settings::Kind;
using pc::settings::Row;

constexpr int kSteps = 20; // a volume's steps, 5% each

PcAudioSettings s_audio;

void save() { pc_settings_set_audio(&s_audio); }

Row volume(const char* group, const char* name, const char* hint, float PcAudioSettings::*field) {
    Row r;
    r.group = group;
    r.name = name;
    r.hint = hint;
    r.kind = Kind::Bar;
    r.count = kSteps;
    r.get = [field] { return static_cast<int>(std::lround(s_audio.*field * kSteps)); };
    r.set = [field](int step) {
        s_audio.*field = static_cast<float>(step) / kSteps;
        save();
    };
    r.label = [](int step) { return std::to_string(step * 100 / kSteps) + "%"; };
    return r;
}

Row toggle(const char* group, const char* name, const char* hint, bool PcAudioSettings::*field) {
    Row r;
    r.group = group;
    r.name = name;
    r.hint = hint;
    r.get = [field] { return s_audio.*field ? 1 : 0; };
    r.set = [field](int on) {
        s_audio.*field = on != 0;
        save();
    };
    return r;
}

std::vector<Row> rows() {
    pc_settings_audio(&s_audio);
    std::vector<Row> v;
    v.push_back(volume("VOLUME", "Master volume", "Everything the game plays. Left and right adjust it.",
                       &PcAudioSettings::volume));
    v.push_back(volume(nullptr, "Music", "Background music.", &PcAudioSettings::music));
    v.push_back(volume(nullptr, "Sound effects", "Sound effects, voices and the crowd.", &PcAudioSettings::sfx));
    v.push_back(toggle(nullptr, "Mute", "Silence the game without changing the volumes.", &PcAudioSettings::mute));
    v.push_back(toggle("SOUND", "Reverb", "Stage reverb and echo.", &PcAudioSettings::reverb));
    if (pc_settings_spatial_built()) {
        v.push_back(toggle(nullptr, "Spatial audio",
                           "Fighters' and items' sounds come from where they are on the stage.",
                           &PcAudioSettings::spatial));
        Row stage = toggle(nullptr, "Expanded stage sounds",
                           "Sounds the stages lack, such as the wind on the Great Fox at Corneria.",
                           &PcAudioSettings::stage_sounds);
        stage.available = [] { return s_audio.spatial; };
        stage.note = [] { return s_audio.spatial ? "" : "NEEDS SPATIAL AUDIO"; };
        stage.hint_now = [] {
            return s_audio.spatial ? nullptr : "Made for spatial audio. Turn spatial audio on first.";
        };
        v.push_back(stage);
    }
    Row reset;
    reset.name = "Restore defaults";
    reset.hint = "Put every audio setting back to how a new install has it.";
    reset.kind = Kind::Action;
    reset.set = [](int) {
        pc_settings_audio_defaults(&s_audio);
        save();
    };
    v.push_back(reset);
    return v;
}

pc::settings::Page s_page("options-page.rml", rows);

} // namespace

extern "C" void pc_audio_menu_show(bool show) { s_page.show(show); }
extern "C" void pc_audio_menu_presence(float presence) { s_page.presence(presence); }
extern "C" bool pc_audio_menu_input(int what) { return s_page.input(what); }
extern "C" const char* pc_audio_menu_hint(void) { return s_page.hint(); }
extern "C" void pc_audio_menu_update(void) { s_page.update(); }
