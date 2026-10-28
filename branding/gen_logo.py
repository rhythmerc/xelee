#!/usr/bin/env python3
"""Placeholder Xelee logo: a custom slanted block wordmark (straight-line
letters drawn as polygons, no font), a chrome fill with a horizon line, on an
angled red-orange-gold panel; plus an X badge with an orbit ring. No
Nintendo artwork: it only borrows Melee's chrome-on-red menu style.

Run it to rewrite the SVGs beside it; the PNG is
    magick -background none -density 384 xelee-badge.svg -resize 1024x1024 xelee-badge-1024.png
"""
import os

OUT = os.path.dirname(os.path.abspath(__file__))
H, T = 100.0, 26.0     # cap height, stroke
GAP = 9.0
S = 9.0                # the diagonal cut on arm ends


def isect(p1, p2, p3, p4):
    (x1, y1), (x2, y2), (x3, y3), (x4, y4) = p1, p2, p3, p4
    d = (x1 - x2) * (y3 - y4) - (y1 - y2) * (x3 - x4)
    a = x1 * y2 - y1 * x2
    b = x3 * y4 - y3 * x4
    return ((a * (x3 - x4) - (x1 - x2) * b) / d, (a * (y3 - y4) - (y1 - y2) * b) / d)


def letter_E(x):
    W, Wm, Tm = 72.0, 58.0, 22.0
    m1 = (H - Tm) / 2
    m2 = m1 + Tm
    pts = [(0, 0), (W, 0), (W - S, T), (T, T), (T, m1), (Wm, m1), (Wm - S * 0.8, m2), (T, m2),
           (T, H - T), (W - S, H - T), (W - 2 * S, H), (0, H)]
    return [(x + px, py) for px, py in pts], W


def letter_L(x):
    W = 66.0
    pts = [(0, 0), (T, 0), (T, H - T), (W, H - T), (W - S, H), (0, H)]
    return [(x + px, py) for px, py in pts], W


def letter_X(x):
    W, t = 92.0, 30.0
    tl, tr = (0, 0), (t, 0)
    tr2, tl2 = (W - t, 0), (W, 0)
    bl, bl2 = (0, H), (t, H)
    br2, br = (W - t, H), (W, H)
    # bar A: top-left -> bottom-right; bar B: top-right -> bottom-left
    top = isect(tr, br, tr2, bl)
    right = isect(tr, br, tl2, bl2)
    bottom = isect(tl, br2, tl2, bl2)
    left = isect(tl, br2, tr2, bl)
    pts = [tl, tr, top, tr2, tl2, right, br, br2, bottom, bl2, bl, left]
    return [(x + px, py) for px, py in pts], W


def wordmark_paths():
    x, polys = 0.0, []
    for f in (letter_X, letter_E, letter_L, letter_E, letter_E):
        p, w = f(x)
        polys.append(p)
        x += w + GAP
    return polys, x - GAP


def path(pts):
    return "M" + " L".join(f"{a:.1f},{b:.1f}" for a, b in pts) + " Z"


DEFS = """
<defs>
  <linearGradient id="chrome" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#ffffff"/>
    <stop offset="0.42" stop-color="#eef1f5"/>
    <stop offset="0.5" stop-color="#8d97a3"/>
    <stop offset="0.56" stop-color="#b9c1ca"/>
    <stop offset="0.85" stop-color="#f4f6f8"/>
    <stop offset="1" stop-color="#ffffff"/>
  </linearGradient>
  <linearGradient id="gold" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#fff3b0"/>
    <stop offset="0.45" stop-color="#ffc62e"/>
    <stop offset="0.5" stop-color="#e07a00"/>
    <stop offset="1" stop-color="#ffb21f"/>
  </linearGradient>
  <linearGradient id="panel" x1="0" y1="0" x2="1" y2="0">
    <stop offset="0" stop-color="#7a0610"/>
    <stop offset="0.45" stop-color="#d0141e"/>
    <stop offset="0.8" stop-color="#ff5a0a"/>
    <stop offset="1" stop-color="#ff9a1a"/>
  </linearGradient>
  <linearGradient id="panelshade" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#ffffff" stop-opacity="0.28"/>
    <stop offset="0.5" stop-color="#ffffff" stop-opacity="0"/>
    <stop offset="1" stop-color="#000000" stop-opacity="0.35"/>
  </linearGradient>
  <radialGradient id="disc" cx="0.4" cy="0.3" r="0.8">
    <stop offset="0" stop-color="#2a3550"/>
    <stop offset="1" stop-color="#0b0f1c"/>
  </radialGradient>
  <filter id="glow" x="-20%" y="-40%" width="140%" height="180%">
    <feGaussianBlur stdDeviation="6" result="b"/>
    <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
  </filter>
</defs>"""


def wordmark_group(dx, dy, scale=1.0, panel=True):
    polys, width = wordmark_paths()
    d = " ".join(path(p) for p in polys)
    g = [f'<g transform="translate({dx},{dy}) scale({scale})">']
    if panel:
        # An angled plate behind the letters, cut like Melee's menu tabs,
        # with a gold rule and two speed slashes.
        pw, ph, pad = width + 120, H + 56, 60
        plate = [(-pad + 40, -28), (pw - pad, -28), (pw - pad - 40, ph - 28), (-pad, ph - 28)]
        g.append(f'<path d="{path(plate)}" fill="url(#panel)"/>')
        g.append(f'<path d="{path(plate)}" fill="url(#panelshade)"/>')
        g.append(f'<path d="M{-pad + 40},-28 L{pw - pad},-28" stroke="#ffd35a" stroke-width="5"/>')
        g.append(f'<path d="M{-pad},{ph - 28} L{pw - pad - 40},{ph - 28}" stroke="#3a0006" stroke-width="5"/>')
        for k, off in enumerate((0, 26)):
            x0 = pw - pad - 18 + off
            g.append(f'<path d="M{x0},-28 L{x0 + 22},-28 L{x0 - 18},{ph - 28} L{x0 - 40},{ph - 28} Z" '
                     f'fill="#ffb31a" opacity="{0.85 - 0.35 * k}"/>')
    g.append('<g transform="skewX(-12)">')
    g.append(f'<path d="{d}" fill="#0a0c14" stroke="#0a0c14" stroke-width="16" stroke-linejoin="round"/>')
    g.append(f'<path d="{d}" fill="url(#chrome)" stroke="#20140a" stroke-width="3" stroke-linejoin="round"/>')
    g.append('</g></g>')
    return "\n".join(g), width


def badge_group(cx, cy, r):
    """A slanted hex tab, a chrome-gold X, and an orbit ring passing behind
    and in front of it."""
    import math
    hexp = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)))
            for a in range(0, 360, 60)]
    p, w = letter_X(0)
    s = r * 1.05 / H
    xpath = path([(cx + (px - w / 2) * s, cy + (py - H / 2) * s) for px, py in p])
    ring = f'cx="{cx}" cy="{cy}" rx="{r * 1.08:.1f}" ry="{r * 0.34:.1f}"'
    rot = f'rotate(-22 {cx} {cy})'
    g = [f'<path d="{path(hexp)}" fill="url(#disc)" stroke="#ffb31a" stroke-width="{r * 0.06:.1f}" stroke-linejoin="round"/>',
         f'<path d="{path(hexp)}" fill="none" stroke="#7a0610" stroke-width="{r * 0.02:.1f}" stroke-linejoin="round" transform="translate(0,{r * 0.035:.1f})"/>',
         # The ring splits along its own long axis: the upper arc behind the
         # X, the lower one in front. No rotate on the rects: a clip path is in
         # the clipped ellipse's user space, which already carries `rot`, and
         # rotating the rects too tilted the split 44 degrees, so the top-right
         # of the swoop came out in front of the X.
         f'<clipPath id="back{int(cx)}"><rect x="{cx - 2 * r}" y="{cy - 2 * r}" width="{4 * r}" height="{2 * r}"/></clipPath>',
         f'<clipPath id="front{int(cx)}"><rect x="{cx - 2 * r}" y="{cy}" width="{4 * r}" height="{2 * r}"/></clipPath>',
         f'<ellipse {ring} transform="{rot}" fill="none" stroke="#c0101a" stroke-width="{r * 0.09:.1f}" clip-path="url(#back{int(cx)})" opacity="0.9"/>',
         f'<g transform="skewX(-8) translate({cy * 0.14:.1f},0)">',
         f'<path d="{xpath}" fill="#0a0c14" stroke="#0a0c14" stroke-width="{r * 0.1:.1f}" stroke-linejoin="round"/>',
         f'<path d="{xpath}" fill="url(#gold)" stroke="#3a1a00" stroke-width="{r * 0.02:.1f}" stroke-linejoin="round"/>',
         '</g>',
         f'<ellipse {ring} transform="{rot}" fill="none" stroke="#ff5a0a" stroke-width="{r * 0.09:.1f}" clip-path="url(#front{int(cx)})"/>',
         f'<ellipse {ring} transform="{rot}" fill="none" stroke="#ffd35a" stroke-width="{r * 0.025:.1f}" clip-path="url(#front{int(cx)})"/>']
    return "\n".join(g)


def svg(w, h, body, bg=None):
    back = f'<rect width="{w}" height="{h}" fill="{bg}"/>' if bg else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">'
            f'{DEFS}{back}{body}</svg>\n')


def write(name, text):
    with open(os.path.join(OUT, name), "w") as f:
        f.write(text)


_, ww = wordmark_paths()
wm, _ = wordmark_group(90, 70, 1.0)
write("xelee-wordmark.svg", svg(int(ww + 200), 230, wm))
write("xelee-badge.svg", svg(320, 320, badge_group(160, 160, 130)))
lock, _ = wordmark_group(330, 92, 1.0)
write("xelee-lockup.svg", svg(int(ww + 470), 280, badge_group(140, 140, 115) + lock))
plain, _ = wordmark_group(40, 30, 1.0, panel=False)
write("xelee-wordmark-plain.svg", svg(int(ww + 80), 160, plain))
print("ok", ww)
