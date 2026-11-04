/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_WINDOW_ICON_H
#define PC_WINDOW_ICON_H

#include <aurora/aurora.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Before aurora_initialize: SDL's app name and id ("Xelee", "xelee") and,
 * on desktops, the window icon from <resources_dir>xelee-icon.png. */
void pc_app_identity(AuroraConfig* config, const char* resources_dir);

#ifdef __cplusplus
}
#endif

#endif
