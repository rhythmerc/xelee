/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Graphics screen (Options > Graphics, in the stock Sound row's
 * place; the settings are settings.h's, the launcher's own),
 * pc_graphics_screen (menu_screen.h). On the Quest: the eyes' resolution,
 * SpaceWarp, foveation, custom textures, the stage's shadow and the
 * lighting; on a screen: the window, sync, resolution, aspect, HUD, FPS
 * counter, anti-aliasing, post-processing, textures and the backend. The
 * lighting's Calibrate room light runs the room light calibration again
 * (src/pc/xr_calibrate.h) and comes back. What
 * only applies at launch (SpaceWarp, foveation, anti-aliasing, the backend)
 * says so until then, and leaving the page offers to restart. Rows on the
 * Options pages' engine (settings_page.hpp), in the native page of
 * src/melee/mn/mnpage.c.
 */
#include "settings.h"
#include "settings_page.hpp"
#include "xr_calibrate.h"
#include "xr_place.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using pc::settings::Kind;
using pc::settings::Row;

PcGraphicsSettings s_g, s_launch;

void save() { pc_settings_set_graphics(&s_g); }

pc::settings::Page& page();

Row toggle(const char* group, const char* name, const char* hint, bool PcGraphicsSettings::*field) {
    Row r;
    r.group = group;
    r.name = name;
    r.hint = hint;
    r.get = [field] { return s_g.*field ? 1 : 0; };
    r.set = [field](int on) {
        s_g.*field = on != 0;
        save();
    };
    return r;
}

// A bool as a choice of two names (off's first).
Row named_toggle(const char* group, const char* name, const char* hint, bool PcGraphicsSettings::*field,
                 const char* off, const char* on) {
    Row r = toggle(group, name, hint, field);
    r.kind = Kind::Choice;
    r.label = [off, on](int i) { return std::string(i == 1 ? on : off); };
    return r;
}

// A choice over `values` (what the setting stores), named by `names`.
Row choice(const char* group, const char* name, const char* hint, int PcGraphicsSettings::*field,
           std::vector<int> values, std::vector<std::string> names) {
    Row r;
    r.group = group;
    r.name = name;
    r.hint = hint;
    r.kind = Kind::Choice;
    r.count = static_cast<int>(values.size());
    r.get = [field, values] {
        const auto it = std::find(values.begin(), values.end(), s_g.*field);
        return it == values.end() ? 0 : static_cast<int>(it - values.begin());
    };
    r.set = [field, values](int i) {
        s_g.*field = values[std::clamp(i, 0, static_cast<int>(values.size()) - 1)];
        save();
    };
    r.label = [names](int i) { return names[std::clamp(i, 0, static_cast<int>(names.size()) - 1)]; };
    return r;
}

// Lighting: Roomtone once the headset cameras may be used, the system asked
// from here when they may not yet.
enum class Camera { Idle, Asking, Refused, Blocked };
Camera s_camera = Camera::Idle;

Row lighting() {
    Row r = choice("ROOM", "Lighting",
                   "How fights are lit. Vanilla is Melee's own. Roomtone lights them with your room's light.",
                   &PcGraphicsSettings::xr_lighting, {0, 1}, {"Vanilla", "Roomtone (Beta)"});
    r.set = [](int i) {
        if (i == 1 && !pc_settings_camera_granted()) {
            s_camera = Camera::Asking;
            pc_settings_camera_request();
            return;
        }
        s_camera = Camera::Idle;
        s_g.xr_lighting = i;
        save();
    };
    r.tick = [] {
        if (s_camera != Camera::Asking)
            return false;
        const int answer = pc_settings_camera_result();
        if (answer < 0)
            return false;
        if (answer == 1) {
            s_camera = Camera::Idle;
            s_g.xr_lighting = 1;
            save();
        } else {
            s_camera = answer == 2 ? Camera::Blocked : Camera::Refused;
        }
        return true;
    };
    r.hint_now = []() -> const char* {
        switch (s_camera) {
        case Camera::Asking:
            return "Roomtone measures your room's light with the cameras. The images never leave the headset.";
        case Camera::Refused:
            return "Roomtone needs the headset cameras. Lighting stays Vanilla.";
        case Camera::Blocked:
            return "Camera access is off for Xelee. Allow it in the headset's settings, then pick Roomtone.";
        default:
            return nullptr;
        }
    };
    return r;
}

// Calibrate room light: the calibration again, Roomtone only; beside it
// this launch's result. Restore defaults leaves it be.
Row calibrate() {
    Row r;
    r.name = "Calibrate room light";
    r.hint = "Look at your room's bright lights again. Worth doing when the light changes, like after dark.";
    r.kind = Kind::Action;
    r.get = [] { return 0; };
    r.set = [](int) { page().go(PC_SCREEN_GO_CALIBRATE); };
    r.label = [](int) { return std::string("Start"); };
    r.available = [] { return s_g.xr_lighting == 1 && pc_xr_place_available(); };
    r.note = []() -> const char* {
        if (s_g.xr_lighting != 1)
            return "";
        static char text[24];
        const int n = pc_xr_calibrate_result();
        if (n < 0)
            return "NOT YET";
        if (n == 0)
            return "NO LIGHT";
        std::snprintf(text, sizeof text, n == 1 ? "%d LIGHT" : "%d LIGHTS", n);
        return text;
    };
    r.hint_now = []() -> const char* {
        return s_g.xr_lighting != 1 ? "Roomtone only. Pick Roomtone above first." : nullptr;
    };
    return r;
}

std::vector<Row> quest_rows() {
    std::vector<Row> v;
    Row res = choice("PICTURE", "Resolution",
                     "How sharp the headset renders, against its recommended size. Dynamic follows the load.",
                     &PcGraphicsSettings::xr_resolution, {0, 80, 90, 100, 110, 120, 130},
                     {"Dynamic", "80%", "90%", "100%", "110%", "120%", "130%"});
    v.push_back(res);
    Row sw = toggle(nullptr, "SpaceWarp",
                    "The headset makes every other frame from the game's motion, smooth at 120 Hz.",
                    &PcGraphicsSettings::xr_spacewarp);
    sw.pending = [] { return s_g.xr_spacewarp != s_launch.xr_spacewarp; };
    v.push_back(sw);
    Row fov = choice(nullptr, "Foveated rendering (FFR)",
                     "The edges of each eye drawn at a lower resolution, leaving the GPU more room.",
                     &PcGraphicsSettings::xr_foveation, {0, 1, 2, 3}, {"Off", "Low", "Medium", "High"});
    fov.pending = [] { return s_g.xr_foveation != s_launch.xr_foveation; };
    v.push_back(fov);
    v.push_back(toggle(nullptr, "Custom HD textures", "Load replacement textures from the textures folder.",
                       &PcGraphicsSettings::custom_textures));
    Row shadow = toggle("ROOM", "Stage shadow", "A soft shadow under the stage on the table it sits over.",
                        &PcGraphicsSettings::xr_shadow);
    v.push_back(shadow);
    Row light = lighting();
    light.group = nullptr;
    v.push_back(light);
    v.push_back(calibrate());
    return v;
}

std::vector<Row> screen_rows() {
    std::vector<Row> v;
    v.push_back(named_toggle("DISPLAY", "Display mode", "Windowed or fullscreen.", &PcGraphicsSettings::fullscreen,
                             "Windowed", "Fullscreen"));
    Row sync = toggle(nullptr, "Vertical sync", "Present in step with your display.", &PcGraphicsSettings::vsync);
    sync.available = [] { return std::getenv("MELEE_VSYNC") == nullptr; };
    sync.note = [] { return std::getenv("MELEE_VSYNC") != nullptr ? "SET BY MELEE_VSYNC" : ""; };
    v.push_back(sync);
    std::vector<int> scales;
    std::vector<std::string> scale_names;
    for (int i = 0; i <= 10; i++) {
        scales.push_back(i);
        scale_names.push_back(i == 0 ? "Auto" : std::to_string(i) + "x");
    }
    v.push_back(choice(nullptr, "Internal resolution", "How sharp the game renders. Auto follows the window.",
                       &PcGraphicsSettings::render_scale, scales, scale_names));
    v.push_back(choice(nullptr, "Aspect ratio", "Fights only. Menus and cutscenes keep the original aspect.",
                       &PcGraphicsSettings::widescreen, {0, 1, 2},
                       {"Original (73:60)", "Widescreen 16:9", "The window's"}));
    v.push_back(choice(nullptr, "HUD layout", "Classic keeps 4:3 margins. Wide puts damage and stocks at the edges.",
                       &PcGraphicsSettings::hud_mode, {0, 1}, {"Classic (4:3)", "Wide (16:9)"}));
    v.push_back(toggle(nullptr, "FPS counter", "The frame rate in a corner of the screen.", &PcGraphicsSettings::fps));
    Row aa = choice("QUALITY", "Anti-aliasing", "Smooth polygon edges with multisampling.", &PcGraphicsSettings::msaa,
                    {1, 4}, {"Off", "4x MSAA"});
    aa.pending = [] { return s_g.msaa != s_launch.msaa; };
    v.push_back(aa);
    std::vector<int> filters{0, 1, 2, 3};
    std::vector<std::string> filter_names;
    for (int f : filters)
        filter_names.push_back(pc_settings_filter_name(f));
    v.push_back(choice(nullptr, "Post-processing", "A filter over the finished frame.",
                       &PcGraphicsSettings::filter_mode, filters, filter_names));
    v.push_back(toggle(nullptr, "Custom HD textures", "Load replacement textures from the textures folder.",
                       &PcGraphicsSettings::custom_textures));
    std::vector<int> backends{0};
    for (int b = pc_settings_backend_next(0); b != 0; b = pc_settings_backend_next(b))
        backends.push_back(b);
    std::vector<std::string> backend_names;
    for (int b : backends)
        backend_names.push_back(pc_settings_backend_name(b));
    Row backend = choice(nullptr, "Graphics backend", "Auto, or a graphics API this platform has.",
                         &PcGraphicsSettings::backend, backends, backend_names);
    backend.pending = [] { return s_g.backend != s_launch.backend; };
    v.push_back(backend);
    return v;
}

std::vector<Row> rows() {
    pc_settings_graphics(&s_g);
    pc_settings_graphics_at_launch(&s_launch);
    s_camera = Camera::Idle;
    std::vector<Row> v = pc_settings_quest() ? quest_rows() : screen_rows();
    Row reset;
    reset.name = "Restore defaults";
    reset.hint = "Put every graphics setting back to how a new install has it.";
    reset.kind = Kind::Action;
    reset.set = [](int) {
        pc_settings_graphics_defaults(&s_g);
        save();
    };
    v.push_back(reset);
    return v;
}

pc::settings::Page s_page("options-page.rml", rows);

pc::settings::Page& page() { return s_page; }

} // namespace

PC_SETTINGS_SCREEN(pc_graphics_screen, s_page);
