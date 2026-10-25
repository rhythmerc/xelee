#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the XR room light calibration's pictures (src/pc/xr_calibrate.c) in
the placing cards' style (make_cards.py), into resources/xr/:

  calibrate-intro.png                 the intro card (A Start, B Back)
  calibrate-strip.png                 the looking strip, its count left out
  calibrate-done.png                  Room light set, its two lines left out
  calibrate-nolight-<0|1>.png         no light found, the cursor on each choice
  calibrate-dark-<0|1>.png            the dark room's, the same
  calibrate-legend.png                A Done, B Back (Touch controllers)
  calibrate-legend-hands.png          Pick up your controller (tracked hands)
  calibrate-markers.png               the lights' numbered markers, in a row
  calibrate-text.png                  the pieces the game puts on the cards:
                                      digits, "lights found.", the next steps
  calibrate-layout.txt                where they go: "slot name x y w h" on
                                      the cards, "piece x y w h name" in
                                      calibrate-text.png (1x pixels)

Needs Pillow. Run from anywhere: python3 tools/xr_cards/make_calibration.py
"""
import os

from PIL import Image, ImageDraw

import make_cards as mc
from make_cards import GOLD, GOLD_DARK, PANEL, SILVER, WHITE, s

CARD_W = s(1000)

# What comes after the step, as the done card's last line says it: the
# stage's placement, back to Options, or the mode (xr_calibrate.c's names).
NEXT = [
    "PLACE THE STAGE",
    "BACK TO OPTIONS",
    "VS. MODE",
    "TOURNAMENT",
    "SPECIAL MELEE",
    "CLASSIC",
    "ADVENTURE",
    "ALL-STAR",
    "EVENT MATCH",
    "TRAINING",
    "TARGET TEST",
    "HOME-RUN CONTEST",
    "MULTI-MAN MELEE",
    "CAMERA MODE",
    "UNRANKED",
    "DIRECT",
    "LAN",
]


def panel(w, h, title=None):
    """A card: the dark panel in its gold rim, a gold title pill on top."""
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, w - 1, h - 1), radius=s(30), fill=PANEL + (238,), outline=GOLD, width=s(6))
    if title:
        pill_h = s(62)
        d.rounded_rectangle((s(18), s(18), w - s(18), s(18) + pill_h), radius=pill_h // 2, fill=GOLD)
        d.rounded_rectangle((s(18), s(18) + pill_h - s(10), w - s(18), s(18) + pill_h), radius=s(5), fill=GOLD_DARK)
        d.rounded_rectangle((s(18), s(18), w - s(18), s(18) + pill_h - s(6)), radius=pill_h // 2, fill=GOLD)
        mc.italic_text(img, (w // 2 - mc.font(36).getlength(title) // 2, s(18) + pill_h * 0.74), title, 36,
                       (16, 12, 4))
    return img, d


def lines(d, x, y, rows, size=30, step=46, fill=WHITE):
    for parts in rows:
        mc.rich_line(d, x, y, parts, size, fill)
        y += s(step)
    return y


def intro():
    img, d = panel(CARD_W, s(560), "ROOM LIGHT")
    y = lines(d, s(48), s(140), [["Roomtone lights each fight with your room's light."]], 30)
    y = lines(d, s(48), y + s(16), [
        ["Look at every bright light in the room:"],
        ["windows, ceiling lights, lamps. Hold each one"],
        ["in the middle of your view until it rings."],
    ], 30)
    lines(d, s(48), y + s(28), [[("btn", "A"), "Start      ", ("btn", "B"), "Back"]], 32)
    lines(d, s(48), s(560) - s(78), [
        ["About 20 seconds, once each time you play. Redo it"],
        ["in Options > Graphics > Calibrate room light."],
    ], 24, 36, SILVER)
    return img


def strip():
    w, h = CARD_W, s(170)
    img, d = panel(w, h)
    mc.italic_text(img, (s(44), s(82)), "LOOK AT EACH BRIGHT LIGHT", 40, WHITE, outline=(40, 40, 46))
    label = "LIGHTS FOUND"
    f = mc.font(26)
    d.text((w - s(130), s(36)), label, font=f, fill=SILVER, anchor="mm")
    # The count goes under the label (slot "count").
    return img, (w - s(210), s(52), s(160), s(100)), (s(46), s(110))


def done():
    w, h = CARD_W, s(420)
    img, _ = panel(w, h, "ROOM LIGHT SET")
    return img, (s(40), s(150), w - s(80), s(80)), (s(40), s(270), w - s(80), s(80))


def choice(title, rows, options, cursor):
    w, h = CARD_W, s(470)
    img, d = panel(w, h, title)
    y = lines(d, s(48), s(150), rows, 30)
    y += s(24)
    for i, label in enumerate(options):
        top = y + i * s(92)
        if i == cursor:
            d.rounded_rectangle((s(40), top, w - s(40), top + s(74)), radius=s(37), fill=GOLD)
            mc.italic_text(img, (s(84), top + s(51)), label, 34, (16, 12, 4))
        else:
            d.rounded_rectangle((s(40), top, w - s(40), top + s(74)), radius=s(37), outline=(90, 90, 100), width=s(3))
            mc.italic_text(img, (s(84), top + s(51)), label, 34, SILVER)
    return img


def legend(parts):
    size = 30
    w = mc.rich_width(parts, size) + s(80)
    h = s(84)
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, w - 1, h - 1), radius=h // 2, fill=(0, 0, 0, 215), outline=GOLD, width=s(5))
    mc.rich_line(d, s(40), h // 2, parts, size)
    return img


def markers(n=12, side=s(96)):
    img = Image.new("RGBA", (side * n, side), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    f = mc.font(52)
    for i in range(n):
        x = i * side
        d.ellipse((x + s(4), s(4), x + side - s(4), side - s(4)), fill=(250, 250, 250, 235))
        d.ellipse((x + s(10), s(10), x + side - s(10), side - s(10)), fill=GOLD)
        d.text((x + side // 2, side // 2 + s(2)), str(i + 1), font=f, fill=(16, 12, 4), anchor="mm")
    return img


def text_pieces():
    """The pieces, each cut tight round its text, packed in rows."""
    pieces = []

    def piece(name, text, size, fill, italic=False, outline=None):
        f = mc.font(size)
        l, t, r, b = f.getbbox(text, anchor="ls")
        pad = s(6)
        w, h = r - l + 2 * pad + (s(size) // 3 if italic else 0), b - t + 2 * pad
        w, h = w + w % mc.SS, h + h % mc.SS  # whole pixels once reduced
        im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        if italic:
            mc.italic_text(im, (pad - l, pad - t), text, size, fill, outline=outline)
        else:
            ImageDraw.Draw(im).text((pad - l, pad - t), text, font=f, fill=fill, anchor="ls")
        pieces.append((name, im))

    for k in range(10):
        piece(f"big-{k}", str(k), 64, GOLD, italic=True, outline=(40, 30, 4))
        piece(f"small-{k}", str(k), 48, GOLD)
    piece("light-found", "light found.", 48, WHITE)
    piece("lights-found", "lights found.", 48, WHITE)
    piece("changed", "Room changed. Look again.", 28, (255, 190, 120))
    for name in NEXT:
        piece("next:" + name, "NEXT: " + name, 36, SILVER, italic=True)
    width = s(1000)
    x = y = row_h = 0
    placed = []
    for name, im in pieces:
        if x + im.width > width:
            x, y = 0, y + row_h
            row_h = 0
        placed.append((name, x, y, im))
        x += im.width
        row_h = max(row_h, im.height)
    atlas = Image.new("RGBA", (width, y + row_h), (0, 0, 0, 0))
    for name, px, py, im in placed:
        atlas.alpha_composite(im, (px, py))
    return atlas, [(name, px, py, im.width, im.height) for name, px, py, im in placed]


def one(v):
    return v // mc.SS


if __name__ == "__main__":
    layout = []
    mc.save(intro(), "calibrate-intro.png")
    img, count, changed = strip()
    mc.save(img, "calibrate-strip.png")
    layout.append(("count",) + count)
    layout.append(("changed",) + changed + (0, 0))
    img, found, nxt = done()
    mc.save(img, "calibrate-done.png")
    layout.append(("found",) + found)
    layout.append(("next",) + nxt)
    for cursor in (0, 1):
        mc.save(choice("NO BRIGHT LIGHT FOUND", [["Look straight at a window or a lamp"], ["until it rings."]],
                       ["Keep looking", "Done without a light"], cursor), f"calibrate-nolight-{cursor}.png")
        mc.save(choice("YOUR ROOM IS DARK", [["Fights will be as dim as your room."],
                                             ["Turn on a lamp and look at it, or keep it dark."]],
                       ["Look again", "Keep it dark"], cursor), f"calibrate-dark-{cursor}.png")
    mc.save(legend([("btn", "A"), "Done   ", ("btn", "B"), "Back"]), "calibrate-legend.png")
    mc.save(legend(["Pick up your controller"]), "calibrate-legend-hands.png")
    mc.save(markers(), "calibrate-markers.png")
    atlas, rects = text_pieces()
    mc.save(atlas, "calibrate-text.png")
    with open(os.path.join(mc.OUT, "calibrate-layout.txt"), "w") as f:
        for name, x, y, w, h in layout:
            f.write(f"slot {name} {one(x)} {one(y)} {one(w)} {one(h)}\n")
        for name, x, y, w, h in rects:
            f.write(f"piece {one(x)} {one(y)} {one(w)} {one(h)} {name}\n")
    print(open(os.path.join(mc.OUT, "calibrate-layout.txt")).read())
