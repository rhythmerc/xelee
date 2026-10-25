/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Audio screen (audio_menu.h). */
#include "audio_menu.h"

#include "fight_menu.h"
#include "launcher.h"
#include "pc.h"
#include "settings.h"

#include <RmlUi/Core.h>
#include <aurora/rmlui.hpp>
#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" void lbAudioAx_80024030(int);

namespace {

enum { SFX_BACK = 0, SFX_CONFIRM = 1, SFX_MOVE = 2 };

// The layout, in px, scaled to fit the panel frame's inside, as Controls'.
constexpr float kScreenW = 1120.f, kScreenH = 660.f;
constexpr float kRowW = 900.f, kRowH = 64.f, kRowGap = 8.f, kGroupH = 38.f;
constexpr float kRowX = (kScreenW - kRowW) / 2;
constexpr int kSteps = 20; // a bar's steps, 5% each
constexpr float kStepW = 16.f, kStepPitch = 21.f;
constexpr float kBarX = kRowW - 22.f - 72.f - 16.f - kSteps * kStepPitch; // in the row
constexpr float kResetW = 260.f, kResetH = 42.f;

enum class Kind { Bar, Switch, Reset };
enum class Field { Volume, Music, Sfx, Mute, Reverb, Spatial, StageSounds, Reset };

struct Row {
    Field field;
    Kind kind;
    const char* name;
    const char* hint;
    float y = 0;
};

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

int percent(float v) { return static_cast<int>(std::lround(v * 100.f)); }

struct Menu : Rml::EventListener {
    Rml::ElementDocument* document = nullptr;
    bool shown = false;
    PcAudioSettings s{};
    std::vector<Row> rows;
    int cursor = 0;
    int hoverStep = -1; // the bar step under the pointer, on the cursor's row
    float presence = 0.f;
    Rml::Vector2i dims{0, 0};
    float placedPresence = -1.f;
    Rml::Vector2i placedDims{0, 0};
    bool pointerWas = false, pointerPressed = false;
    std::string hintText;

    bool load() {
        if (document != nullptr)
            return true;
        auto* context = aurora::rmlui::get_context();
        if (context == nullptr)
            return false;
        const std::string dir = pc_resources_dir();
        for (const char* face : {"menu.ttf", "menu-bold.ttf"})
            Rml::LoadFontFace(dir + face);
        document = context->LoadDocument(dir + "audio-settings.rml");
        if (document == nullptr) {
            pc_log_line("audio settings: could not load audio-settings.rml");
            return false;
        }
        document->AddEventListener(Rml::EventId::Click, this);
        document->AddEventListener(Rml::EventId::Mouseover, this);
        return true;
    }

    bool stage_sounds_available() const { return s.spatial; }

    void build() {
        rows.clear();
        rows.push_back({Field::Volume, Kind::Bar, "Master volume", "Everything the game plays. Left and right adjust it."});
        rows.push_back({Field::Music, Kind::Bar, "Music", "Background music."});
        rows.push_back({Field::Sfx, Kind::Bar, "Sound effects", "Sound effects, voices and the crowd."});
        rows.push_back({Field::Mute, Kind::Switch, "Mute", "Silence the game without changing the volumes."});
        rows.push_back({Field::Reverb, Kind::Switch, "Reverb", "Stage reverb and echo."});
        if (pc_settings_spatial_built()) {
            rows.push_back({Field::Spatial, Kind::Switch, "Spatial audio",
                "Fighters' and items' sounds come from where they are on the stage."});
            rows.push_back({Field::StageSounds, Kind::Switch, "Expanded stage sounds",
                "Sounds the stages lack, such as the wind on the Great Fox at Corneria."});
        }
        rows.push_back({Field::Reset, Kind::Reset, "Restore defaults", "Put every audio setting back to how a new install has it."});

        // Volume's rows, then Sound's, each under its heading; Reset last.
        std::string rml;
        float y = 8.f;
        auto group = [&](const char* label) {
            rml += "<div class=\"group\" style=\"left: " + fmt(kRowX) + "px; top: " + fmt(y) + "px; width: " +
                   fmt(kRowW) + "px;\"><div class=\"rule\"></div><div class=\"label\">" + label + "</div></div>";
            y += kGroupH - 8.f;
        };
        group("VOLUME");
        for (size_t i = 0; i < rows.size(); i++) {
            Row& r = rows[i];
            const std::string id = std::to_string(i);
            if (r.field == Field::Reverb) {
                y += 12.f;
                group("SOUND");
            }
            if (r.kind == Kind::Reset) {
                r.y = kScreenH - kResetH - 14.f;
                rml += "<button id=\"reset\" data-row=\"" + id + "\" style=\"left: " + fmt((kScreenW - kResetW) / 2) +
                       "px; top: " + fmt(r.y) + "px;\">Restore defaults</button>";
                continue;
            }
            r.y = y;
            rml += "<div id=\"row-" + id + "\" class=\"row\" data-row=\"" + id + "\" style=\"left: " + fmt(kRowX) +
                   "px; top: " + fmt(y) + "px; width: " + fmt(kRowW) + "px;\"><span class=\"name\">" + r.name +
                   "</span>";
            if (r.kind == Kind::Bar) {
                rml += "<div class=\"bar\" style=\"left: " + fmt(kBarX) + "px; width: " + fmt(kSteps * kStepPitch) +
                       "px;\">";
                for (int k = 0; k < kSteps; k++)
                    rml += "<div id=\"step-" + id + "-" + std::to_string(k) + "\" class=\"step\" data-step=\"" +
                           std::to_string(k) + "\" style=\"left: " + fmt(k * kStepPitch) + "px; width: " +
                           fmt(kStepW) + "px;\"></div>";
                rml += "</div><span id=\"pct-" + id + "\" class=\"pct\" style=\"left: " +
                       fmt(kRowW - 22.f - 72.f - 6.f) + "px;\"></span>";
            } else {
                const float onX = kRowW - 22.f - 96.f - 6.f, offX = onX - 96.f - 8.f;
                rml += "<span id=\"note-" + id + "\" class=\"note\" style=\"left: " + fmt(offX - 260.f) +
                       "px; width: 250px;\"></span>";
                rml += "<span id=\"off-" + id + "\" class=\"choice\" style=\"left: " + fmt(offX) + "px;\">Off</span>";
                rml += "<span id=\"on-" + id + "\" class=\"choice\" style=\"left: " + fmt(onX) + "px;\">On</span>";
            }
            rml += "</div>";
            y += kRowH + kRowGap;
        }
        document->GetElementById("board")->SetInnerRML(rml);
        cursor = 0;
        refresh();
    }

    bool value_of(Field f) const {
        switch (f) {
        case Field::Mute:
            return s.mute;
        case Field::Reverb:
            return s.reverb;
        case Field::Spatial:
            return s.spatial;
        case Field::StageSounds:
            return s.stage_sounds;
        default:
            return false;
        }
    }

    float* bar_of(Field f) {
        switch (f) {
        case Field::Volume:
            return &s.volume;
        case Field::Music:
            return &s.music;
        case Field::Sfx:
            return &s.sfx;
        default:
            return nullptr;
        }
    }

    void refresh() {
        for (size_t i = 0; i < rows.size(); i++) {
            const Row& r = rows[i];
            const std::string id = std::to_string(i);
            const bool on = static_cast<int>(i) == cursor;
            if (r.kind == Kind::Reset) {
                if (auto* e = document->GetElementById("reset"))
                    e->SetClass("cursor", on);
                continue;
            }
            auto* box = document->GetElementById("row-" + id);
            if (box == nullptr)
                continue;
            box->SetClass("cursor", on);
            if (r.kind == Kind::Bar) {
                const float v = *bar_of(r.field);
                const int lit = static_cast<int>(std::lround(v * kSteps));
                for (int k = 0; k < kSteps; k++)
                    if (auto* e = document->GetElementById("step-" + id + "-" + std::to_string(k)))
                        e->SetClass("on", k < lit);
                if (auto* e = document->GetElementById("pct-" + id))
                    e->SetInnerRML(std::to_string(percent(v)) + "%");
                box->SetClass("muted", s.mute);
                continue;
            }
            const bool value = value_of(r.field);
            const bool unavailable = r.field == Field::StageSounds && !stage_sounds_available();
            box->SetClass("unavailable", unavailable);
            if (auto* e = document->GetElementById("on-" + id))
                e->SetClass("on", value);
            if (auto* e = document->GetElementById("off-" + id))
                e->SetClass("on", !value);
            if (auto* e = document->GetElementById("note-" + id))
                e->SetInnerRML(unavailable ? "NEEDS SPATIAL AUDIO" : "");
        }
    }

    void apply() {
        pc_settings_set_audio(&s);
        refresh();
    }

    void show() {
        if (shown || !load())
            return;
        pc_settings_audio(&s);
        build();
        shown = true;
        presence = 0.f;
        placedPresence = -1.f;
        document->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        layout();
    }

    void hide() {
        if (!shown)
            return;
        shown = false;
        document->Hide();
    }

    void move(int dir) {
        const int n = static_cast<int>(rows.size());
        cursor = (cursor + (dir < 0 ? n - 1 : 1)) % n;
        hoverStep = -1;
        lbAudioAx_80024030(SFX_MOVE);
        refresh();
    }

    // Left (-1) or right (+1) on the cursor's row.
    void change(int dir) {
        const Row& r = rows[cursor];
        if (r.kind == Kind::Bar) {
            float* v = bar_of(r.field);
            const int step = std::clamp(static_cast<int>(std::lround(*v * kSteps)) + dir, 0, kSteps);
            if (step == static_cast<int>(std::lround(*v * kSteps)))
                return;
            *v = static_cast<float>(step) / kSteps;
            apply();
            lbAudioAx_80024030(SFX_MOVE); // heard at the new volume
        } else if (r.kind == Kind::Switch) {
            flip(r.field);
        }
    }

    void flip(Field f) {
        if (f == Field::StageSounds && !stage_sounds_available())
            return;
        switch (f) {
        case Field::Mute:
            s.mute = !s.mute;
            break;
        case Field::Reverb:
            s.reverb = !s.reverb;
            break;
        case Field::Spatial:
            s.spatial = !s.spatial;
            break;
        case Field::StageSounds:
            s.stage_sounds = !s.stage_sounds;
            break;
        default:
            return;
        }
        apply();
        lbAudioAx_80024030(SFX_MOVE);
    }

    void set_step(int row, int step) {
        float* v = bar_of(rows[row].field);
        if (v == nullptr)
            return;
        *v = static_cast<float>(step + 1) / kSteps;
        apply();
        lbAudioAx_80024030(SFX_MOVE);
    }

    void select() {
        const Row& r = rows[cursor];
        if (r.kind == Kind::Reset) {
            pc_settings_audio_defaults(&s);
            apply();
            lbAudioAx_80024030(SFX_CONFIRM);
        } else if (r.kind == Kind::Switch) {
            flip(r.field);
        } else if (pointerWas && hoverStep >= 0) {
            set_step(cursor, hoverStep); // a laser's click on a step
        }
    }

    const char* hint() {
        if (!shown || rows.empty())
            return nullptr;
        const Row& r = rows[cursor];
        if (r.field == Field::StageSounds && !stage_sounds_available())
            return "Made for spatial audio. Turn spatial audio on first.";
        return r.hint;
    }

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

    static int attr_of(Rml::Element* e, const char* name) {
        for (; e != nullptr; e = e->GetParentNode())
            if (const Rml::Variant* v = e->GetAttribute(name))
                return std::atoi(v->Get<Rml::String>().c_str());
        return -1;
    }

    void ProcessEvent(Rml::Event& event) override {
        if (!shown || presence < 1.f)
            return;
        Rml::Element* target = event.GetTargetElement();
        const int row = attr_of(target, "data-row");
        const int step = attr_of(target, "data-step");
        switch (event.GetId()) {
        case Rml::EventId::Mouseover:
            if (row >= 0 && row != cursor) {
                cursor = row;
                lbAudioAx_80024030(SFX_MOVE);
                refresh();
            }
            hoverStep = row == cursor ? step : -1;
            break;
        case Rml::EventId::Click:
            // A laser's click arrives as the menus' A too (xr_pointer.c).
            if (pointerWas || row < 0)
                break;
            cursor = row;
            if (step >= 0)
                set_step(row, step);
            else
                select();
            break;
        default:
            break;
        }
    }

    void update() {
        if (!shown)
            return;
        layout();
#ifdef AURORA_ENABLE_OPENXR
        // The lasers: the mouse on the screen.
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

Menu s_menu;

} // namespace

extern "C" void pc_audio_menu_show(bool show) {
    if (show)
        s_menu.show();
    else
        s_menu.hide();
}

extern "C" void pc_audio_menu_presence(float presence) { s_menu.presence = presence; }

extern "C" bool pc_audio_menu_input(int what) {
    if (!s_menu.shown || s_menu.presence < 1.f)
        return false;
    switch (what) {
    case PC_FIGHT_MENU_UP:
        s_menu.move(-1);
        return true;
    case PC_FIGHT_MENU_DOWN:
        s_menu.move(1);
        return true;
    case PC_FIGHT_MENU_LEFT:
        s_menu.change(-1);
        return true;
    case PC_FIGHT_MENU_RIGHT:
        s_menu.change(1);
        return true;
    case PC_FIGHT_MENU_CONFIRM:
        s_menu.select();
        return true;
    default:
        return false; // B: nothing to close, the page goes back
    }
}

extern "C" const char* pc_audio_menu_hint(void) { return s_menu.hint(); }

extern "C" void pc_audio_menu_update(void) { s_menu.update(); }
