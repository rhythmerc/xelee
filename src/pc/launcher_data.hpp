/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace launcher {
struct DiscInfo {
    bool supported = false;
    std::string message;
};
enum class VerifyState { Verified, Mismatch, Canceled, Error };
struct Verification {
    VerifyState state = VerifyState::Error;
    std::string message;
};
struct Preferences {
    std::string disc;
    std::string net_name = "PLAYER";
    std::string net_target;  // empty hosts our own connect code
    int net_delay = -1;      // auto; otherwise 0..4 frames
    int net_port = 0;        // operating-system allocated

    bool vsync = true;
    bool fullscreen = false;
    float scale = 1.0f;
#if defined(__ANDROID__)
    float render_scale = 1.0f, volume = 1.0f;
    int msaa = 1, anisotropy = 1;
#else
    float render_scale = 0.0f, volume = 1.0f;
    int msaa = 1, anisotropy = 16;
#endif
    int widescreen = 0;
    int filter_mode = 0;
    int backend = 0;
    bool mute = false, fps = false, reverb = true;
    bool check_updates = true;
    bool custom_textures = true;
    bool unlock_all = false;
    int hud_mode = 0;
    bool frozen_stadium = false;
    bool free_camera = false;
    bool ucf = true;             // UCF 0.84 rules (dashback, shield drop, SDI, tumble, cardinals)
    bool slippi_fixes = true;    // Slippi's other gameplay rules: no wobbling, neutral spawns, freeze-glitch fix
    bool cstick_1p = true;       // the C-stick attacks in the 1P modes too (a common Gecko code)
    float music_volume = 1.0f;
    float sfx_volume = 1.0f;
    bool spatial_audio = true;  // fighters' and items' sounds placed (src/pc/spatial_audio.h)
    bool stage_sounds = true;   // our own stage ambience (src/pc/stage_ambience.h); needs spatial audio
    // Quest: the eyes' resolution in percent of the headset's recommended
    // size (80-130), or 0 for dynamic resolution; and Application SpaceWarp.
    int xr_resolution = 0;
    bool xr_spacewarp = true;
    // Quest: fixed foveated rendering, 0 (off) to 3 (high): the edges of each
    // eye drawn at a lower density (AuroraConfig::xrFoveation).
    int xr_foveation = 2;
    int xr_lighting = 0; // XR: 0 the game's own; 1 Roomtone, the room's light (needs the headset cameras)
    // Quest, mixed reality: the stage's contact shadow on the real surface.
    bool xr_shadow = true;
    bool xr_place_hold = true; // XR: the stage is placed before each fight (off: fights start right away)
    bool xn_notice_seen = false; // the one-time "XeleeNet unlocked" special message has shown (ifprize.c)
    // VS Mode > Xelee Melee > Capture the Flag's rules (src/pc/ctf_rules_menu.cpp),
    // kept for next time as Melee keeps its own: the lobby's defaults
    // (docs/xeleenet-modes-plan.md §6h).
    int ctf_caps = 5;        // 1-9
    int ctf_minutes = 10;    // 0 no limit
    int ctf_return = 0;      // seconds; 0 never
    bool ctf_home = true;    // your own flag must be home to score
    bool ctf_slow = true;    // carriers walk and dash slower
    bool ctf_items = false;  // Melee's own item settings, or none
    bool xr_full_vr = false;   // XR: full VR (whole stages) instead of mixed reality
    uint64_t install_id = 0;  // random once per install (LAN host election); 0 = not yet
};
DiscInfo inspect_disc(const std::string& path);
Verification verify_disc(
    const std::string& path, std::atomic_bool& cancel, std::atomic_uint& progress);
Preferences load_preferences(const std::filesystem::path& path);
bool save_preferences(
    const std::filesystem::path& path, const Preferences& prefs, std::string& error);

// Disc images the launcher can play, in these directories (not their
// subdirectories), in order: for a headset's own build, which has no file
// picker in the headset. At most `limit`.
std::vector<std::string> find_discs(
    const std::vector<std::filesystem::path>& dirs, size_t limit = 8);

// A headset's controllers driving the launcher's gamepad navigation: they
// reach it as the GameCube pad (aurora_xr_get_pad), not as a gamepad SDL
// sees. Buttons are PADStatus::button bits, the stick -128..127 (up
// positive). What the change since `state` does; holding the stick repeats.
struct PadNav {
    int step = 0;        // focus down (+1) or up (-1)
    int horizontal = 0;  // a value right (+1) or left (-1)
    bool activate = false, back = false;
};
struct PadNavState {
    uint16_t buttons = 0;
    uint64_t stepped_ms = 0, moved_ms = 0;
};
PadNav pad_nav(
    PadNavState& state, uint16_t buttons, int8_t stick_x, int8_t stick_y, uint64_t now_ms);
}  // namespace launcher
