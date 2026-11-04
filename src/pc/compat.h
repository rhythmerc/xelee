/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Force-included into every game translation unit (see CMakeLists.txt). */
#ifndef PC_COMPAT_H
#define PC_COMPAT_H

#include <sys/types.h>
#include <stdint.h>
#include <stddef.h>
#include <math.h>

#include "pc/disc.h"
#include "pc/libm/pc_trig.h"

/* Melee has its own expf and powf (lb/lb_00CE.c: series, as the GameCube
 * build had them), which the game's math keeps using. Under libm's names
 * they would also answer every expf, powf, std::exp and std::pow outside
 * the game (Aurora, the XR layer, src/pc), since it all links into one
 * binary; and Melee's series never returns for some arguments (expf(-13.1)
 * loops forever: the XR thread hung on the stage shadow's texture, a black
 * headset). So in the game's own files they go by other names. */
#ifdef MELEE_GAME_TU
float melee_expf(float x);
float melee_powf(float x, float y);
#define expf melee_expf
#define powf melee_powf
#endif

/* The game's sqrtf is MSL's, not libm's (pc/gekko_fp.h), and so is its
 * logf (pc/libm/pc_msl_logf.c): every libm's bits differ there. */
#if defined(MELEE_GAME_TU) && !defined(__cplusplus)
#include "pc/gekko_fp.h"
#define sqrtf gk_sqrtf
float pc_logf(float x);
#define logf pc_logf
#endif

#endif
