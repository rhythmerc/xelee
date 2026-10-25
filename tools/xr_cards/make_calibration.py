#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draws the XR room light calibration's pictures (src/pc/xr_calibrate.c), into
resources/xr/. The placing board's frame (make_cards.py: Melee's red border
and slanted title tab) round the Options pages' look (resources/
options-page.rcss: gold line work, the cursor's pale border and glow, the
menus' Noto Sans):

  calibrate-intro.png                 the intro card (A Start, B Back)
  calibrate-clip.jpg                  its clip, a grid of frames (footage/)
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

Needs Pillow and ffmpeg. Run from anywhere: python3 tools/xr_cards/make_calibration.py
"""
import math
import os
import subprocess
import tempfile

from PIL import Image, ImageDraw, ImageFilter, ImageFont

import make_cards as mc
from make_cards import FRAME_DARK, FRAME_RED, GOLD, SILVER, WHITE, s

# The Options pages' colours (options-page.rcss).
ROW_BORDER = (240, 194, 38, 176)  # .row
ROW_FILL = (12, 11, 8, 224)
ROW_GLOW = (255, 150, 20, 80)
CURSOR_BORDER = (255, 244, 196)   # .row.cursor
CURSOR_FILL = (29, 24, 10, 240)
CURSOR_GLOW = (255, 176, 32, 224)
NAME = (255, 248, 224)            # a row's name
VALUE = (240, 228, 184)           # its value, the body text
LABEL = (201, 183, 124)           # a group's heading, notes
RULE = (240, 194, 38, 96)

MENU = os.path.join(mc.ROOT, "resources", "menu.ttf")
MENU_BOLD = os.path.join(mc.ROOT, "resources", "menu-bold.ttf")


def mfont(size, bold=False):
    return ImageFont.truetype(MENU_BOLD if bold else MENU, size * mc.SS)


def mline(d, x, y, parts, size, fill=VALUE, bold=False):
    """A line in the menus' face: strings, and ("btn", label) glyphs."""
    f = mfont(size, bold)
    for p in parts:
        if isinstance(p, tuple):
            x = mc.button(d, x, y, p[1], size=size * 1.25) + s(10)
        else:
            d.text((x, y), p, font=f, fill=fill, anchor="lm")
            x += int(f.getlength(p))
    return x


def mwidth(parts, size, bold=False):
    probe = Image.new("RGBA", (s(2400), s(200)))
    return mline(ImageDraw.Draw(probe), 0, s(100), parts, size, bold=bold)


def glow(img, box, radius, colour, blur, spread):
    """A box-shadow: the box's shape, grown by `spread`, blurred under it."""
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    x0, y0, x1, y1 = box
    ImageDraw.Draw(layer).rounded_rectangle((x0 - spread, y0 - spread, x1 + spread, y1 + spread),
                                            radius=radius + spread, fill=colour)
    img.alpha_composite(layer.filter(ImageFilter.GaussianBlur(blur)))


def box(img, rect, cursor=False):
    """An Options row or button: gold line work, the cursor's pale and lit."""
    glow(img, rect, s(7), CURSOR_GLOW if cursor else ROW_GLOW, s(7 if cursor else 4), s(3 if cursor else 1))
    ImageDraw.Draw(img).rounded_rectangle(rect, radius=s(7), fill=CURSOR_FILL if cursor else ROW_FILL,
                                          outline=CURSOR_BORDER if cursor else ROW_BORDER, width=s(3))

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


# The frame's title tab, above the panel.
TAB = s(64)


def frame(w, h, title):
    """A card in the placing board's frame: the dark panel in Melee's red
    border, the title in a slanted tab on its top edge, hatching beside it.
    `h` is the panel's; the card is TAB taller."""
    img = Image.new("RGBA", (w, TAB + h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    top = TAB
    d.rounded_rectangle((s(6), top, w - s(6), top + h - s(6)), radius=s(40), fill=(0, 0, 0, 215), outline=FRAME_RED,
                        width=s(12))
    d.rounded_rectangle((s(22), top + s(16), w - s(22), top + h - s(22)), radius=s(28), outline=FRAME_DARK, width=s(3))
    end = s(110) + int(mc.font(40).getlength(title)) + s(60)
    d.polygon([(s(40), top + s(6)), (s(70), s(6)), (end, s(6)), (end - s(30), top + s(6))], fill=(0, 0, 0, 230),
              outline=FRAME_RED, width=s(8))
    for i in range(5):
        x = end + s(30 + i * 30)
        d.polygon([(x, top), (x + s(30), s(14)), (x + s(44), s(14)), (x + s(14), top)], fill=FRAME_RED)
    mc.italic_text(img, (s(100), s(52)), title, 40, SILVER, outline=(40, 40, 46))
    return img, d


def lines(d, x, y, rows, size=28, step=42, fill=VALUE):
    for parts in rows:
        mline(d, x, y, parts, size, fill)
        y += s(step)
    return y


def wrap(text, size, width):
    """The words of `text` in lines as wide as `width` (supersampled) allows."""
    f = mfont(size)
    out, line = [], ""
    for word in text.split():
        trial = word if not line else line + " " + word
        if line and f.getlength(trial) > width:
            out.append([line])
            line = word
        else:
            line = trial
    if line:
        out.append([line])
    return out


def centred(d, w, y, parts, size, fill=VALUE):
    """A line of text and buttons centred across a card `w` wide."""
    mline(d, (w - mwidth(parts, size)) // 2, y, parts, size, fill)


def buttons(img, w, y, items, cursor=-1):
    """Options buttons side by side, centred: each a box with its glyph and
    name, as tall as an Options row."""
    bw, bh, gap = s(250), s(62), s(36)
    x = (w - (len(items) * bw + (len(items) - 1) * gap)) // 2
    d = ImageDraw.Draw(img)
    for i, (glyph, name) in enumerate(items):
        rect = (x, y, x + bw, y + bh)
        box(img, rect, i == cursor)
        parts = [("btn", glyph), name]
        mline(d, x + (bw - mwidth(parts, 26, True)) // 2, y + bh // 2, parts, 26, NAME, True)
        x += bw + gap
    return y + bh


# The intro's clip (the user's headset recording, clip_atlas): where it sits
# on the card, beside the text.
CLIP_RECT = (s(48), TAB + s(48), s(300), s(300))


def intro():
    w, h = CARD_W, s(540)
    img, d = frame(w, h, "Room Light")
    cx, cy, cw, ch = CLIP_RECT
    box(img, (cx - s(4), cy - s(4), cx + cw + s(3), cy + ch + s(3)))
    x = cx + cw + s(40)
    width = w - x - s(40)
    y = lines(d, x, cy + s(24), wrap("Roomtone lights fights with your room's light.", 28, width))
    lines(d, x, y + s(16), wrap("Look at every bright light in the room: windows, ceiling lights, lamps. Hold each "
                                "one in the middle of your view until it rings.", 28, width))
    y = buttons(img, w, cy + ch + s(30), [("A", "Start"), ("B", "Back")])
    centred(d, w, y + s(34), ["Recalibrate in Options > Graphics > Calibrate room light."], 20, LABEL)
    return img


def strip():
    w, h = CARD_W, s(150)
    img, d = frame(w, h, "Room Light")
    y = TAB + h // 2 - s(2)
    d.text((s(52), y), "Look at each bright light", font=mfont(34, True), fill=NAME, anchor="lm")
    label = "LIGHTS FOUND"
    d.text((w - s(150), TAB + s(40)), label, font=mfont(18, True), fill=LABEL, anchor="mm")
    # The count goes under the label (slot "count"); "Room changed" under the
    # line (slot "changed").
    return img, (w - s(230), TAB + s(52), s(160), s(80)), (s(54), TAB + s(98))


def done():
    w, h = CARD_W, s(250)
    img, _ = frame(w, h, "Room Light Set")
    return img, (s(40), TAB + s(54), w - s(80), s(80)), (s(40), TAB + s(146), w - s(80), s(50))


def choice(title, text, options, cursor):
    w = CARD_W
    rows = wrap(text, 28, w - s(120))
    h = s(54) + len(rows) * s(42) + s(20) + len(options) * s(80) + s(40)
    img, d = frame(w, h, title)
    y = lines(d, s(60), TAB + s(60), rows)
    y += s(8)
    for i, label in enumerate(options):
        top = y + i * s(80)
        box(img, (s(52), top, w - s(52), top + s(62)), i == cursor)
        d.text((s(80), top + s(31)), label, font=mfont(26, True), fill=NAME, anchor="lm")
    return img


def legend(parts):
    """The controls under the strip, in an Options box."""
    size = 26
    pad = s(14)
    w = mwidth(parts, size, True) + s(56) + 2 * pad
    h = s(64) + 2 * pad
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    box(img, (pad, pad, w - pad, h - pad))
    mline(ImageDraw.Draw(img), pad + s(28), h // 2, parts, size, NAME, True)
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

    def piece(name, text, size, fill, italic=False, outline=None, bold=True):
        f = mfont(size, bold)
        l, t, r, b = f.getbbox(text, anchor="ls")
        pad = s(6)
        w, h = r - l + 2 * pad + (s(size) // 3 if italic else 0), b - t + 2 * pad
        w, h = w + w % mc.SS, h + h % mc.SS  # whole pixels once reduced
        im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        if italic:
            mc.italic_text(im, (pad - l, pad - t), text, size, fill, outline=outline)  # Melee's face, as the tabs
        else:
            ImageDraw.Draw(im).text((pad - l, pad - t), text, font=f, fill=fill, anchor="ls")
        pieces.append((name, im))

    for k in range(10):
        piece(f"big-{k}", str(k), 64, GOLD, italic=True, outline=(40, 30, 4))
        piece(f"small-{k}", str(k), 48, GOLD)
    piece("light-found", "light found.", 44, NAME)
    piece("lights-found", "lights found.", 44, NAME)
    piece("changed", "Room changed. Look again.", 22, (255, 190, 120))
    for name in NEXT:
        piece("next:" + name, "NEXT: " + name, 22, LABEL)
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


# The intro's clip: footage/calibration-look.mp4, 2.5 s of the user's
# headset recording (2026-10-25, 4.3-6.8 s of calibration-example.mp4: the
# look up from the door to the ceiling light, its ring, the count going to
# 1), played forward and looped at CLIP_FPS, CLIP_SIZE square a frame. As a
# JPEG grid like the placing cards' clips: no decoding as it plays, and a
# 1.8 x 1.5k texture only while the intro shows.
CLIP_FPS = 15
CLIP_SIZE = 256


def clip_atlas():
    src = os.path.join(mc.HERE, "footage", "calibration-look.mp4")
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run(["ffmpeg", "-v", "error", "-i", src, "-an", "-vf",
                        f"fps={CLIP_FPS},scale={CLIP_SIZE}:{CLIP_SIZE}:flags=lanczos", os.path.join(tmp, "f%03d.png")],
                       check=True)
        frames = [Image.open(os.path.join(tmp, f)).convert("RGB") for f in sorted(os.listdir(tmp))]
    cols = math.ceil(math.sqrt(len(frames)))
    rows = math.ceil(len(frames) / cols)
    atlas = Image.new("RGB", (cols * CLIP_SIZE, rows * CLIP_SIZE))
    for i, fr in enumerate(frames):
        atlas.paste(fr, ((i % cols) * CLIP_SIZE, (i // cols) * CLIP_SIZE))
    path = os.path.join(mc.OUT, "calibrate-clip.jpg")
    atlas.save(path, quality=85, optimize=True)
    print(f"{path}: {len(frames)} frames, {cols}x{rows}, {os.path.getsize(path) // 1024} KB")
    return len(frames), cols


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
        mc.save(choice("No Bright Light Found", "Look straight at a window or a lamp until it rings.",
                       ["Keep looking", "Done without a light"], cursor), f"calibrate-nolight-{cursor}.png")
        mc.save(choice("Your Room Is Dark", "Fights will be as dim as your room. Turn on a lamp and look at it, or "
                                            "keep it dark.",
                       ["Look again", "Keep it dark"], cursor), f"calibrate-dark-{cursor}.png")
    mc.save(legend([("btn", "A"), "Done   ", ("btn", "B"), "Back"]), "calibrate-legend.png")
    mc.save(legend(["Pick up your controller"]), "calibrate-legend-hands.png")
    mc.save(markers(), "calibrate-markers.png")
    frames, cols = clip_atlas()
    atlas, rects = text_pieces()
    mc.save(atlas, "calibrate-text.png")
    with open(os.path.join(mc.OUT, "calibrate-layout.txt"), "w") as f:
        for name, x, y, w, h in layout:
            f.write(f"slot {name} {one(x)} {one(y)} {one(w)} {one(h)}\n")
        # frames cols frame_w frame_h fps, then where it goes on the intro
        x, y, w, h = CLIP_RECT
        f.write(f"clip {frames} {cols} {CLIP_SIZE} {CLIP_SIZE} {CLIP_FPS} {one(x)} {one(y)} {one(w)} {one(h)}\n")
        for name, x, y, w, h in rects:
            f.write(f"piece {one(x)} {one(y)} {one(w)} {one(h)} {name}\n")
    print(open(os.path.join(mc.OUT, "calibrate-layout.txt")).read())
