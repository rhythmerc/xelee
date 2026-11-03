/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_CTF_ART_H
#define PC_CTF_ART_H

/* Capture the Flag's HUD art (tools/gen_ctf_art.py draws it; none of it is
 * the game's) and the two primitives the HUD draws it with. Both draw in the
 * current position matrix's space (the HUD camera's world units for the HUD,
 * the stage's for the beams), y up, and set the GX state they need. */

#include <dolphin/gx.h>
#include <stdbool.h>

typedef enum PcCtfArt {
    PC_CTF_ART_FLAG_RED_HOME,
    PC_CTF_ART_FLAG_BLUE_HOME,
    PC_CTF_ART_FLAG_RED,
    PC_CTF_ART_FLAG_BLUE,
    PC_CTF_ART_EMBLEM, /* white: tinted per team */
    PC_CTF_ART_COUNT,
} PcCtfArt;

#define PC_CTF_ART_SIZE 160

extern const unsigned char* const pc_ctf_art_data[PC_CTF_ART_COUNT];

/* A square of the art centred on (cx, cy), `size` units across, turned
 * `angle` degrees clockwise, its colour and alpha multiplied by `tint`. */
void pc_ctf_art_quad(PcCtfArt which, float cx, float cy, float z, float size, float angle, GXColor tint);

/* A ring arc centred on (cx, cy): radius r, width w, from 12 o'clock
 * clockwise over `frac` of the circle, with soft (feathered) edges `f` wide.
 * Alpha-blended in `color`. */
void pc_ctf_arc(float cx, float cy, float z, float r, float w, float f, float frac, GXColor color);

/* A ring as an ellipse: radii rx, ry, band w, feather f. `additive` adds
 * light instead of blending over. */
void pc_ctf_ellipse(float cx, float cy, float z, float rx, float ry, float w, float f, GXColor color, bool additive);

/* A light beam standing on (x, y) in the current camera's world: a column
 * `height` tall and `width` across, in `body` with a white core, a ring on
 * the floor and rings rising up it (`tick` moves them). `alpha` 0-1 fades it.
 * Depth-tested against the scene, not written. The caller loads the camera's
 * view as the current position matrix. */
void pc_ctf_beam(float x, float y, float z, float height, float width, GXColor body, GXColor light, float alpha,
                 int tick);

/* A capture zone's edge: a low translucent wall round the rectangle x l..r,
 * z z_near..z_far, standing on the floor at y and `height` tall, in `body`
 * with a brighter lip in `edge` along its top and a glow at its foot. Real 3D
 * in the current camera's world (it reads the same in a headset's eyes).
 * Depth-tested, not written; `alpha` 0-1 fades it. The caller loads the
 * camera's view as the current position matrix. */
void pc_ctf_zone_ring(float l, float r, float y, float z_near, float z_far, float height, GXColor body, GXColor edge,
                      float alpha);

#endif
