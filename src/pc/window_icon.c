/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Xelee's app identity at run time on desktops: the window icon (the Xelee
 * badge, resources/xelee-icon.png, made by branding/gen_logo.py) and the
 * name SDL reports for the app (taskbars, audio mixers, the Wayland app id,
 * which matches platforms/linux/xelee.desktop). Phones and the headset take
 * theirs from the APK. */
#include "pc/window_icon.h"

#include <SDL3/SDL.h>
#include <stdio.h>

#if !defined(__ANDROID__)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "pc/stb_image.h"
#endif

void pc_app_identity(AuroraConfig* config, const char* resources_dir)
{
#if defined(__ANDROID__)
    (void) config;
    (void) resources_dir;
#else
    /* "xelee" is the desktop file's name, so a taskbar matches the window to
     * it (X11 WM_CLASS, Wayland app_id). */
    SDL_SetAppMetadata("Xelee", NULL, "xelee");

    char path[4096];
    size_t size = 0;
    void* data;
    int w = 0, h = 0, n = 0;
    unsigned char* rgba;

    if (resources_dir == NULL) {
        return;
    }
    snprintf(path, sizeof path, "%sxelee-icon.png", resources_dir);
    data = SDL_LoadFile(path, &size);
    if (data == NULL) {
        return; /* no icon: the platform's default, nothing worse */
    }
    rgba = stbi_load_from_memory(data, (int) size, &w, &h, &n, 4);
    SDL_free(data);
    if (rgba == NULL) {
        return;
    }
    /* Kept for the process: aurora sets it when it makes the window. 128 px:
     * X11 dropped a 256 px icon (its _NET_WM_ICON came back empty). */
    config->iconRGBA8 = rgba;
    config->iconWidth = (uint32_t) w;
    config->iconHeight = (uint32_t) h;
#endif
}
