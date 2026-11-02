#!/usr/bin/env python3
"""Draw Capture the Flag's HUD art and write it as GX RGBA8 textures to
src/pc/ctf_art_data.c (docs/xeleenet-modes-plan.md §6e).

All of it is drawn here, nothing is taken from the game: the flag icon (with
and without its plate, per team) and the white flag emblem the HUD tints per
team. The digits and stock icons the HUD shows next to them are Melee's own,
loaded from the disc at run time. The ring and the beams are geometry
(ifctf.c, gmctf.c). Needs Pillow. Run from the repository root:

    tools/gen_ctf_art.py [--png DIR]   (--png also writes the art as PNGs to look at)
"""
import argparse
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter

SIZE = 160   # texture edge, texels (a multiple of 4)
SS = 4       # supersampling

TEAM = {
    0: dict(cloth=(232, 58, 58), shade=(150, 28, 34), light=(255, 120, 120)),
    1: dict(cloth=(63, 111, 240), shade=(32, 60, 154), light=(130, 170, 255)),
}
POLE = (232, 226, 207)
POLE_DARK = (160, 152, 128)
GOLD = (240, 194, 38)
GOLD_HI = (255, 236, 150)
OUTLINE = (10, 10, 12)


def cloth_poly(x0, y0, w, h, phase=0.0, segs=24):
    """A waving cloth: the top edge and the bottom edge, from the pole out."""
    top, bot = [], []
    for i in range(segs + 1):
        u = i / segs
        wave = math.sin(phase + u * 3.2) * 0.07 * h * u
        x = x0 + w * u
        top.append((x, y0 + wave + 0.06 * h * u))
        bot.append((x, y0 + h + wave + 0.03 * h * u))
    return top + bot[::-1]


def with_outline(im, px):
    """A dark rim around the art's silhouette, as Melee's HUD art has."""
    a = im.getchannel("A")
    grown = a.filter(ImageFilter.MaxFilter(px * 2 + 1))
    rim = Image.new("RGBA", im.size, OUTLINE + (0,))
    rim.putalpha(grown)
    out = Image.new("RGBA", im.size)
    out.alpha_composite(rim)
    out.alpha_composite(im)
    return out


def flag_icon(team, plate):
    """The in-game flag as a HUD icon: cream pole, gold knob, team cloth, and
    (home) its plate. Drawn on a SIZE square with the pole's foot at the
    bottom centre-left, so a rotated quad tips it about the middle."""
    c = TEAM[team]
    S = SIZE * SS
    im = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(im)
    H = int(S * 0.84)            # the icon's height inside the square
    W = int(H * 0.95)
    ox = (S - W) // 2
    oy = (S - H) // 2
    pole_x = ox + int(W * 0.28)
    pw = max(2, int(H * 0.06))
    top_y = oy + int(H * 0.12)
    bot_y = oy + int(H * 0.86)
    poly = cloth_poly(pole_x + pw // 2, top_y + int(H * 0.04), W * 0.62, H * 0.36, phase=0.6)
    d.polygon(poly, fill=c["shade"] + (255,))
    poly2 = cloth_poly(pole_x + pw // 2, top_y + int(H * 0.04), W * 0.62, H * 0.24, phase=0.6)
    d.polygon(poly2, fill=c["cloth"] + (255,))
    poly3 = cloth_poly(pole_x + pw // 2, top_y + int(H * 0.06), W * 0.6, H * 0.06, phase=0.6)
    d.polygon(poly3, fill=c["light"] + (150,))
    d.rectangle([pole_x - pw // 2, top_y, pole_x + pw // 2, bot_y], fill=POLE + (255,))
    d.rectangle([pole_x + pw // 6, top_y, pole_x + pw // 2, bot_y], fill=POLE_DARK + (255,))
    r = int(H * 0.075)
    d.ellipse([pole_x - r, top_y - r, pole_x + r, top_y + r], fill=GOLD + (255,))
    d.ellipse([pole_x - r * 0.55, top_y - r * 0.7, pole_x - r * 0.05, top_y - r * 0.2], fill=GOLD_HI + (255,))
    if plate:
        pr = int(W * 0.22)
        d.ellipse([pole_x - pr, bot_y - int(H * 0.045), pole_x + pr, bot_y + int(H * 0.055)], fill=c["cloth"] + (255,))
        d.ellipse([pole_x - pr * 0.8, bot_y - int(H * 0.04), pole_x + pr * 0.8, bot_y + int(H * 0.01)],
                  fill=c["light"] + (255,))
    im = im.resize((SIZE, SIZE), Image.LANCZOS)
    return with_outline(im, 3)


def emblem():
    """The team emblem behind a score, as the series emblem sits behind a
    damage number: a flat flag silhouette in white, tinted per team."""
    S = SIZE * SS
    im = Image.new("RGBA", (S, S))
    d = ImageDraw.Draw(im)
    H = int(S * 0.92)
    W = H
    ox = (S - W) // 2
    oy = (S - H) // 2
    pole_x = ox + int(W * 0.24)
    pw = int(H * 0.09)
    d.rectangle([pole_x - pw // 2, oy + int(H * 0.08), pole_x + pw // 2, oy + int(H * 0.97)], fill=(255, 255, 255, 255))
    r = int(H * 0.085)
    d.ellipse([pole_x - r, oy + int(H * 0.08) - r, pole_x + r, oy + int(H * 0.08) + r], fill=(255, 255, 255, 255))
    d.polygon(cloth_poly(pole_x, oy + int(H * 0.11), W * 0.74, H * 0.56, phase=0.4), fill=(255, 255, 255, 255))
    return im.resize((SIZE, SIZE), Image.LANCZOS)


def gx_rgba8(im):
    """GX RGBA8: 4x4 texel tiles, each 64 bytes: the 16 texels' A,R pairs then
    their G,B pairs."""
    raw = im.tobytes()
    out = bytearray()
    for ty in range(0, SIZE, 4):
        for tx in range(0, SIZE, 4):
            ar, gb = bytearray(), bytearray()
            for y in range(4):
                for x in range(4):
                    i = ((ty + y) * SIZE + tx + x) * 4
                    r, g, b, a = raw[i:i + 4]
                    ar += bytes([a, r])
                    gb += bytes([g, b])
            out += ar + gb
    return bytes(out)


ART = [  # the order of PcCtfArt (src/pc/ctf_art.h)
    ("flag_red_home", lambda: flag_icon(0, True)),
    ("flag_blue_home", lambda: flag_icon(1, True)),
    ("flag_red", lambda: flag_icon(0, False)),
    ("flag_blue", lambda: flag_icon(1, False)),
    ("emblem", emblem),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--png", metavar="DIR", help="also write each texture as a PNG here")
    args = ap.parse_args()
    lines = ["/* Generated by tools/gen_ctf_art.py: Capture the Flag's HUD art, drawn there",
             " * (none of it from the game), as GX RGBA8 textures. Do not edit. */",
             '#include "pc/ctf_art.h"', ""]
    for name, make in ART:
        im = make().convert("RGBA")
        assert im.size == (SIZE, SIZE)
        if args.png:
            os.makedirs(args.png, exist_ok=True)
            im.save(os.path.join(args.png, f"{name}.png"))
        data = gx_rgba8(im)
        lines.append(f"static const unsigned char art_{name}[{len(data)}] __attribute__((aligned(32))) = {{")
        for i in range(0, len(data), 32):
            lines.append("    " + ",".join(str(b) for b in data[i:i + 32]) + ",")
        lines.append("};")
        lines.append("")
    lines.append("const unsigned char* const pc_ctf_art_data[PC_CTF_ART_COUNT] = {")
    for name, _ in ART:
        lines.append(f"    art_{name},")
    lines.append("};")
    open("src/pc/ctf_art_data.c", "w").write("\n".join(lines) + "\n")
    print("wrote src/pc/ctf_art_data.c", file=sys.stderr)


if __name__ == "__main__":
    main()
