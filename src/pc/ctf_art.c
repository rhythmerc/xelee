/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pc/ctf_art.h"

#include <dolphin/gx.h>
#include <math.h>

static void setup_colour_blend(void)
{
    GXSetCullMode(GX_CULL_NONE);
    GXSetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
    GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
    GXSetColorUpdate(GX_ENABLE);
    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHT_NULL, GX_DF_NONE, GX_AF_NONE);
}

void pc_ctf_art_quad(PcCtfArt which, float cx, float cy, float z, float size, float angle, GXColor tint)
{
    static GXTexObj tex[PC_CTF_ART_COUNT];
    static unsigned char inited[PC_CTF_ART_COUNT];
    const float h = size * 0.5f;
    const float rad = -angle * 3.14159265f / 180.0f;
    const float c = cosf(rad), s = sinf(rad);
    /* corners: top-left, top-right, bottom-right, bottom-left (y up) */
    static const float cx_[4] = { -1, 1, 1, -1 }, cy_[4] = { 1, 1, -1, -1 };
    static const float u_[4] = { 0, 1, 1, 0 }, v_[4] = { 0, 0, 1, 1 };
    int i;

    if ((int) which < 0 || which >= PC_CTF_ART_COUNT) {
        return;
    }
    if (!inited[which]) {
        GXInitTexObj(&tex[which], (void*) pc_ctf_art_data[which], PC_CTF_ART_SIZE, PC_CTF_ART_SIZE, GX_TF_RGBA8,
                     GX_CLAMP, GX_CLAMP, GX_FALSE);
        GXInitTexObjLOD(&tex[which], GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        inited[which] = 1;
    }
    setup_colour_blend();
    GXClearVtxDesc();
    GXSetNumTexGens(1);
    GXSetNumTevStages(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_DISABLE, GX_PTIDENTITY);
    GXLoadTexObj(&tex[which], GX_TEXMAP0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    for (i = 0; i < 4; i++) {
        const float x = cx_[i] * h, y = cy_[i] * h;
        GXPosition3f32(cx + x * c - y * s, cy + x * s + y * c, z);
        GXColor4u8(tint.r, tint.g, tint.b, tint.a);
        GXTexCoord2f32(u_[i], v_[i]);
    }
    GXEnd();
}

void pc_ctf_arc(float cx, float cy, float z, float r, float w, float f, float frac, GXColor color)
{
    /* Four rings of vertices across the band: outer feather (alpha 0), outer
     * edge, inner edge, inner feather (alpha 0); a strip per pair. */
    const int segs = 72;
    const int n = (int) ceilf(segs * (frac < 0 ? 0 : frac > 1 ? 1 : frac));
    const float radius[4] = { r + w * 0.5f + f, r + w * 0.5f, r - w * 0.5f, r - w * 0.5f - f };
    const unsigned char alpha[4] = { 0, color.a, color.a, 0 };
    int band, i;

    if (n <= 0) {
        return;
    }
    setup_colour_blend();
    GXClearVtxDesc();
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    for (band = 0; band < 3; band++) {
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, (u16) ((n + 1) * 2));
        for (i = 0; i <= n; i++) {
            /* from 12 o'clock, clockwise */
            const float t = (i == n ? frac : (float) i / segs) * 6.2831853f;
            const float sx = sinf(t), sy = cosf(t);
            GXPosition3f32(cx + sx * radius[band], cy + sy * radius[band], z);
            GXColor4u8(color.r, color.g, color.b, alpha[band]);
            GXPosition3f32(cx + sx * radius[band + 1], cy + sy * radius[band + 1], z);
            GXColor4u8(color.r, color.g, color.b, alpha[band + 1]);
        }
        GXEnd();
    }
}

static void setup_geometry(bool additive, bool depth)
{
    setup_colour_blend();
    if (additive) {
        GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, GX_LO_CLEAR);
    }
    if (depth) {
        GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    }
    GXClearVtxDesc();
    GXSetNumTexGens(0);
    GXSetNumTevStages(1);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
    GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
}

static void ellipse_strips(float cx, float cy, float z, float rx, float ry, float w, float f, GXColor color)
{
    const int segs = 64;
    const float k[4] = { 0.5f * w + f, 0.5f * w, -0.5f * w, -0.5f * w - f };
    const unsigned char alpha[4] = { 0, color.a, color.a, 0 };
    int band, i;
    for (band = 0; band < 3; band++) {
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, (u16) ((segs + 1) * 2));
        for (i = 0; i <= segs; i++) {
            const float a = (float) i / segs * 6.2831853f;
            const float sx = cosf(a), sy = sinf(a);
            GXPosition3f32(cx + sx * (rx + k[band]), cy + sy * (ry + k[band] * (ry / rx)), z);
            GXColor4u8(color.r, color.g, color.b, alpha[band]);
            GXPosition3f32(cx + sx * (rx + k[band + 1]), cy + sy * (ry + k[band + 1] * (ry / rx)), z);
            GXColor4u8(color.r, color.g, color.b, alpha[band + 1]);
        }
        GXEnd();
    }
}

void pc_ctf_ellipse(float cx, float cy, float z, float rx, float ry, float w, float f, GXColor color, bool additive)
{
    setup_geometry(additive, false);
    ellipse_strips(cx, cy, z, rx, ry, w, f, color);
}

/* A column across [x - hw, x + hw], `height` up from y: alpha across falls
 * off as (1 - |d|)^sharp, alpha up as (1 - t)^0.55. */
static void column(float x, float y, float z, float hw, float height, float sharp, GXColor c, float alpha)
{
    enum { ACROSS = 16, UP = 6 };
    int i, j;
    for (j = 0; j < UP; j++) {
        const float t0 = (float) j / UP, t1 = (float) (j + 1) / UP;
        const float v0 = powf(1.0f - t0, 0.55f), v1 = powf(1.0f - t1, 0.55f);
        GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, (ACROSS + 1) * 2);
        for (i = 0; i <= ACROSS; i++) {
            const float d = (float) i / ACROSS * 2.0f - 1.0f;
            const float h = powf(1.0f - fabsf(d), sharp) * alpha;
            GXPosition3f32(x + d * hw, y + t0 * height, z);
            GXColor4u8(c.r, c.g, c.b, (u8) (255.0f * h * v0));
            GXPosition3f32(x + d * hw, y + t1 * height, z);
            GXColor4u8(c.r, c.g, c.b, (u8) (255.0f * h * v1));
        }
        GXEnd();
    }
}

void pc_ctf_beam(float x, float y, float z, float height, float width, GXColor body, GXColor light, float alpha,
                 int tick)
{
    static const GXColor white = { 255, 255, 255, 255 };
    const float hw = width * 0.5f;
    int k;

    if (alpha <= 0.0f) {
        return;
    }

    /* The coloured body over the scene, then light on top: the core, the
     * floor ring and the rising rings. */
    setup_geometry(false, true);
    column(x, y, z, hw, height, 1.1f, body, 0.92f * alpha);
    setup_geometry(true, true);
    column(x, y, z, hw, height, 7.0f, white, 0.85f * alpha);
    {
        GXColor ring = light;
        ring.a = (u8) (220.0f * alpha);
        ellipse_strips(x, y, z, hw * 0.78f, hw * 0.16f, hw * 0.14f, hw * 0.08f, ring);
        ring.a = (u8) (110.0f * alpha);
        ellipse_strips(x, y, z, hw * 0.3f, hw * 0.06f, hw * 0.3f, hw * 0.1f, ring);
    }
    for (k = 0; k < 4; k++) {
        /* Rings rising up the column, as a beacon pulses. */
        const float phase = fmodf((float) k / 4.0f + (float) tick * 0.006f, 1.0f);
        const float ry = y + height * (0.08f + 0.72f * phase);
        const float fade = (1.0f - phase) * alpha;
        GXColor c;
        c.r = (u8) (light.r * 0.6f + 255 * 0.4f);
        c.g = (u8) (light.g * 0.6f + 255 * 0.4f);
        c.b = (u8) (light.b * 0.6f + 255 * 0.4f);
        c.a = (u8) (230.0f * fade);
        ellipse_strips(x, ry, z, hw * (0.36f + 0.14f * phase), hw * 0.08f, hw * 0.07f, hw * 0.05f, c);
    }
}

/* One lap of the prism's wall, from height y0 to y1: the rectangle's corners
 * in order and back to the first, alpha a0 at the bottom and a1 at the top. */
static void zone_band(const float* cx, const float* cz, float y0, float y1, GXColor c, float a0, float a1)
{
    int i;
    GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT0, 10);
    for (i = 0; i <= 4; i++) {
        const int k = i & 3;
        GXPosition3f32(cx[k], y0, cz[k]);
        GXColor4u8(c.r, c.g, c.b, (u8) (255.0f * a0));
        GXPosition3f32(cx[k], y1, cz[k]);
        GXColor4u8(c.r, c.g, c.b, (u8) (255.0f * a1));
    }
    GXEnd();
}

void pc_ctf_zone_ring(float l, float r, float y, float z_near, float z_far, float height, GXColor body, GXColor edge,
                      float alpha)
{
    /* Corners round the footprint: back left, back right, front right,
     * front left (a closed lap). */
    const float cx[4] = { l, r, r, l };
    const float cz[4] = { z_far, z_far, z_near, z_near };
    const float lip = height * 0.16f; /* the bright top edge's band */

    if (alpha <= 0.0f) {
        return;
    }
    setup_geometry(false, true);
    /* The wall: strongest at the floor, thinning upward. Seen from above,
     * its front and back overlap, so each stays light. */
    zone_band(cx, cz, y, y + height - lip, body, 0.34f * alpha, 0.16f * alpha);
    /* Its lip, a lighter line that marks the top. */
    zone_band(cx, cz, y + height - lip, y + height, edge, 0.75f * alpha, 0.9f * alpha);
    /* A soft glow where it meets the floor. */
    setup_geometry(true, true);
    zone_band(cx, cz, y, y + height * 0.35f, edge, 0.25f * alpha, 0.0f);
}
