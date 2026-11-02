#!/usr/bin/env python3
"""Draw a stage's collision to scale from a MELEE_DEBUG_CTF_SURVEY dump, with
proposed CTF bases (docs/xeleenet-modes-plan.md §3.2).

    DISPLAY=:0 MELEE_BOOT_SCENE=vs MELEE_DEBUG_VS_STAGE=14 MELEE_DEBUG_CTF_SURVEY=1 \\
        MELEE_UNPACED=1 build/linux/melee --no-card DISC > survey.txt   # Ctrl-C after "ctf-survey: end"
    tools/ctf_survey_draw.py survey.txt temple.png

The BASES table below holds the proposal being drawn (Temple's)."""
import sys
from PIL import Image, ImageDraw, ImageFont
lines, pts, blast, cam = [], {}, None, None
for ln in open(sys.argv[1], errors="replace"):
    if "ctf-survey:" not in ln:
        continue
    w = ln.split("ctf-survey:")[1].split()
    if w[0] == "line":
        lines.append((w[2], *map(float, w[3:7])))
    elif w[0] == "point":
        pts[int(w[1])] = (float(w[2]), float(w[3]))
    elif w[0] == "blast":
        blast = list(map(float, w[1:5]))
    elif w[0] == "cam":
        cam = list(map(float, w[1:5]))
K = 1.7
X0, X1, Y0, Y1 = -330, 330, -270, 220
W, H = int((X1 - X0) * K), int((Y1 - Y0) * K)
im = Image.new("RGB", (W, H + 70), "#10141c")
d = ImageDraw.Draw(im)
def P(x, y):
    return ((x - X0) * K, (Y1 - y) * K)
try:
    f = ImageFont.truetype("DejaVuSans.ttf", 14); fb = ImageFont.truetype("DejaVuSans-Bold.ttf", 15)
except OSError:
    f = fb = ImageFont.load_default()
for gx in range(-300, 301, 50):
    d.line([P(gx, Y0), P(gx, Y1)], fill="#1b2230")
    d.text(P(gx + 2, Y0 + 12), str(gx), fill="#4a5568", font=f)
for gy in range(-250, 201, 50):
    d.line([P(X0, gy), P(X1, gy)], fill="#1b2230")
    d.text(P(X0 + 2, gy + 10), str(gy), fill="#4a5568", font=f)
def rect(b, col, label):
    l, r, t, bt = b
    d.rectangle([P(l, t), P(r, bt)], outline=col, width=2)
    d.text((P(l, t)[0] + 6, P(l, t)[1] + 4), label, fill=col, font=f)
rect(blast, "#c0392b", "blast zone")
rect(cam, "#7f8c8d", "camera bounds")
COL = {"F": "#7ed957", "C": "#f0a040", "L": "#8fa3c0", "R": "#8fa3c0", "?": "#ff00ff"}
for k, x0, y0, x1, y1 in lines:
    d.line([P(x0, y0), P(x1, y1)], fill=COL[k], width=4 if k == "F" else 2)
# Proposed bases: (name, colour, zone l r bottom top, stand, respawns)
BASES = [
    ("RED  bottom-left", "#ff5a5a", (-134, -75, -88.6, -50), (-105, -88.6), [(-125, -70), (-88, -70)]),
    ("BLUE top-right", "#5a8cff", (169, 197, 92.2, 125), (183, 92.2), [(176, 112), (190, 112)]),
]
for name, col, (l, r, b, t), stand, spawns in BASES:
    d.rectangle([P(l, t), P(r, b)], outline=col, width=3)
    sx, sy = P(*stand)
    d.line([sx, sy, sx, sy - 34], fill="#e9e3cf", width=3)
    d.polygon([(sx + 2, sy - 34), (sx + 26, sy - 28), (sx + 2, sy - 20)], fill=col)
    for x, y in spawns:
        px, py = P(x, y)
        d.ellipse([px - 6, py - 6, px + 6, py + 6], outline=col, width=3)
    d.text((P(l, t)[0], P(l, t)[1] - 20), name, fill=col, font=fb)
for i, (x, y) in pts.items():
    px, py = P(x, y)
    d.line([px - 6, py, px + 6, py], fill="#fff"); d.line([px, py - 6, px, py + 6], fill="#fff")
    if i < 4:
        d.text((px + 6, py - 16), f"P{i+1} start", fill="#d0d0d0", font=f)
x, y = pts.get(4, (0, 0))
d.text((P(x, y)[0] + 8, P(x, y)[1] + 2), "Melee respawn (all ports)", fill="#d0d0d0", font=f)
leg = [("floor", "#7ed957"), ("ceiling", "#f0a040"), ("wall", "#8fa3c0"), ("flag stand", "#e9e3cf"),
       ("○ base respawn", "#e9e3cf")]
x = 14
for name, col in leg:
    d.rectangle([x, H + 22, x + 22, H + 34], fill=col)
    d.text((x + 28, H + 20), name, fill="#d0d0d0", font=f)
    x += 150
d.text((14, H + 44), "Hyrule Temple collision, from the game (units). Boxes are proposed base zones.", fill="#9aa1ad", font=f)
im.save(sys.argv[2])
print(sys.argv[2], im.size)
