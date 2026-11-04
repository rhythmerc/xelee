#!/usr/bin/env python3
"""Check a CTF map's waypoint graph (src/melee/gm/gmctfnav.c) against the
stage's collision lines (docs/ctf-ai-plan.md C3).

    tools/ctf_nav_check.py --survey survey.txt [--svg graph.svg]
    DISPLAY=:0 tools/ctf_nav_check.py --exe build/linux/melee   # dumps the survey itself

The survey is the game's "ctf-survey:" lines (MELEE_DEBUG_CTF_SURVEY=1,
gmctf.c ctf_survey). The graph is read from gmctfnav.c's Temple tables, so
the C data stays the one source. Checks:
  - every floor line of the stage has an area, and walls and ceilings none;
  - every node stands on a floor line of its own area (within 1.5 units);
  - walks join nodes of one area;
  - a jump rises at most its reach, ends higher than it starts, is more than
    35 degrees up (Melee's mover runs at anything flatter, ftCo_800AB224),
    and its path (straight up from the start, then across just over the
    landing, as the mover flies it) crosses no wall or ceiling; a
    pass-through platform (a floor line) may be jumped through;
  - a drop (through a platform) or a hop (off a ledge) ends lower than it
    starts;
  - every node reaches both stands, and each stand the other, for the
    strongest class (the others may lose the high jumps: listed as notes).
With --svg it draws the stage, the nodes and the links.
"""
import argparse
import glob
import math
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
NAV_C = os.path.join(REPO, "src/melee/gm/gmctfnav.c")
DISC = "/home/rmercado/Downloads/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso"
BODY = 8.0  # half a fighter's height: a jump's line is checked this high


def dump_survey(exe, disc):
    env = dict(os.environ, MELEE_BOOT_SCENE="vs", MELEE_DEBUG_CTF="1", MELEE_DEBUG_CTF_SURVEY="1",
               MELEE_NO_CARD="1", MELEE_UNPACED="1", MELEE_VSYNC="0", MELEE_DRAW_EVERY="10",
               MELEE_WINDOW_TITLE="ctf-nav-check")
    xa = glob.glob(f"/run/user/{os.getuid()}/xauth_*")
    if xa and "XAUTHORITY" not in env:
        env["XAUTHORITY"] = xa[0]
    out = tempfile.NamedTemporaryFile(prefix="ctf_survey_", suffix=".txt", delete=False).name
    with open(out, "w") as f:
        p = subprocess.Popen([exe, "--no-card", disc], env=env, stdout=f, stderr=subprocess.STDOUT)
    t0 = time.time()
    while time.time() - t0 < 120 and "ctf-survey: end" not in open(out, errors="replace").read():
        time.sleep(0.5)
    p.kill()
    p.wait()
    return out


def read_survey(path):
    lines = {}
    for m in re.finditer(r"ctf-survey: line (\d+) ([FCLR?]) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)",
                         open(path, errors="replace").read()):
        lines[int(m.group(1))] = (m.group(2), float(m.group(3)), float(m.group(4)), float(m.group(5)),
                                  float(m.group(6)))
    return lines


def read_graph(path):
    src = open(path).read()
    areas = re.search(r"enum \{\s*(T_TOP,.*?)\};", src, re.S).group(1)
    area_names = re.findall(r"\b(T_[A-Z]+)\b", areas)
    nodes_enum = re.search(r"enum \{\s*(N_RED_STAND,.*?)\};", src, re.S).group(1)
    node_names = [n for n in re.findall(r"\b(N_[A-Z_]+)\b", nodes_enum) if n != "N_TEMPLE_NODES"]
    nodes = {}
    for m in re.finditer(r"\[(N_[A-Z_]+)\] = \{ (-?[\d.]+)f, (-?[\d.]+)f, (T_[A-Z]+) \}", src):
        nodes[m.group(1)] = (float(m.group(2)), float(m.group(3)), m.group(4))
    links = []
    block = re.search(r"temple_links\[\] = \{(.*?)\};", src, re.S).group(1)
    for m in re.finditer(r"\{ (N_[A-Z_]+), (N_[A-Z_]+), (LINK_[A-Z]+), (\d+) \}", block):
        links.append((m.group(1), m.group(2), m.group(3), int(m.group(4))))
    block = re.search(r"temple_line_area\[\d+\] = \{(.*?)\};", src, re.S).group(1)
    line_area = re.findall(r"\b(T_[A-Z]+)\b", block)
    reach = [int(x) for x in re.search(r"nav_class_reach\[CTFNAV_CLASSES\] = \{ ([\d, ]+) \}", src).group(1)
             .split(",")]
    return area_names, node_names, nodes, links, line_area, reach


def seg_cross(p1, p2, q1, q2):
    def orient(a, b, c):
        return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    d1, d2 = orient(q1, q2, p1), orient(q1, q2, p2)
    d3, d4 = orient(p1, p2, q1), orient(p1, p2, q2)
    return (d1 > 0) != (d2 > 0) and (d3 > 0) != (d4 > 0)


def floor_y(line, x):
    _, x0, y0, x1, y1 = line
    lo, hi = min(x0, x1), max(x0, x1)
    if x < lo - 0.5 or x > hi + 0.5:
        return None
    if abs(x1 - x0) < 1e-6:
        return min(y0, y1)
    return y0 + (y1 - y0) * (x - x0) / (x1 - x0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--survey")
    ap.add_argument("--exe")
    ap.add_argument("--disc", default=DISC)
    ap.add_argument("--nav", default=NAV_C)
    ap.add_argument("--svg")
    args = ap.parse_args()
    survey = args.survey or dump_survey(args.exe or os.path.join(REPO, "build/linux/melee"), args.disc)
    coll = read_survey(survey)
    if not coll:
        print(f"no collision lines in {survey}")
        return 2
    area_names, node_names, nodes, links, line_area, reach = read_graph(args.nav)
    errors = []

    # Areas by line: floors have one, nothing else does.
    for i, (kind, *_rest) in coll.items():
        if kind == "F" and i >= len(line_area):
            errors.append(f"floor line {i} has no area")
    for i, a in enumerate(line_area):
        if i in coll and coll[i][0] != "F":
            errors.append(f"line {i} is a {coll[i][0]} line but has area {a}")

    # Nodes on their floors.
    for name, (x, y, area) in nodes.items():
        ok = False
        for i, a in enumerate(line_area):
            if a != area or i not in coll:
                continue
            fy = floor_y(coll[i], x)
            if fy is not None and abs(fy - y) <= 1.5:
                ok = True
                break
        if not ok:
            errors.append(f"node {name} ({x}, {y}) is on no floor line of {area}")

    walls = [(i, l) for i, l in coll.items() if l[0] in "CLR"]
    for a, b, kind, r in links:
        ax, ay, aa = nodes[a]
        bx, by, ba = nodes[b]
        if kind == "LINK_WALK" and aa != ba:
            errors.append(f"walk {a}->{b} joins two areas")
        if kind in ("LINK_DROP", "LINK_HOP") and by >= ay:
            errors.append(f"drop {a}->{b} doesn't go down ({ay} -> {by})")
        if kind == "LINK_JUMP":
            rise = by - ay
            ang = math.degrees(math.atan2(rise, abs(bx - ax)))
            if rise <= 0:
                errors.append(f"jump {a}->{b} doesn't go up")
            if rise > r + 1.0:
                errors.append(f"jump {a}->{b} rises {rise:.1f}, more than its reach {r}")
            if ang <= 35.0:
                errors.append(f"jump {a}->{b} is {ang:.0f} degrees up: Melee's mover runs at it")
            # Melee's mover jumps straight up and drifts once it is high
            # enough (ftCo_800A9CB4): up from the start, then across a
            # little over the landing.
            top = by + BODY + 4.0
            path = [((ax, ay + BODY), (ax, top)), ((ax, top), (bx, top))]
            for i, l in walls:
                for p1, p2 in path:
                    if seg_cross(p1, p2, (l[1], l[2]), (l[3], l[4])):
                        errors.append(f"jump {a}->{b} crosses {l[0]} line {i}")

    # Reachability for the strongest class (walks join x-neighbours per area).
    def edges(cls_reach):
        e = {n: set() for n in nodes}
        for area in set(v[2] for v in nodes.values()):
            row = sorted((v[0], n) for n, v in nodes.items() if v[2] == area)
            for (_, p), (_, q) in zip(row, row[1:]):
                e[p].add(q)
                e[q].add(p)
        for a, b, kind, r in links:
            if kind != "LINK_JUMP" or r <= cls_reach:
                e[a].add(b)
        return e

    def reach_from(e, s):
        seen, todo = {s}, [s]
        while todo:
            n = todo.pop()
            for m in e[n]:
                if m not in seen:
                    seen.add(m)
                    todo.append(m)
        return seen

    for ci, cr in enumerate(reach):
        e = edges(cr)
        stands = ["N_RED_STAND", "N_BLUE_STAND"]
        for s in stands:
            other = stands[1] if s == stands[0] else stands[0]
            if other not in reach_from(e, s):
                msg = f"class {ci} (reach {cr}): no path from {s} to {other}"
                if ci == len(reach) - 1:
                    errors.append(msg)
                else:
                    print("note: " + msg)
        for n in nodes:
            got = reach_from(e, n)
            for s in stands:
                if s not in got:
                    msg = f"class {ci} (reach {cr}): {n} can't get to {s}"
                    if ci == len(reach) - 1:
                        errors.append(msg)
                    else:
                        print("note: " + msg)

    if args.svg:
        draw(args.svg, coll, nodes, links)
    print(f"{len(nodes)} nodes, {len(links)} links, {len(coll)} collision lines: "
          f"{'OK' if not errors else str(len(errors)) + ' problems'}")
    for e in errors:
        print("  " + e)
    return 1 if errors else 0


def draw(path, coll, nodes, links):
    """An SVG of the stage's lines, the nodes and the links (no plotting
    library needed)."""
    xs = [v for l in coll.values() for v in (l[1], l[3])]
    ys = [v for l in coll.values() for v in (l[2], l[4])]
    x0, x1, y0, y1 = min(xs) - 20, max(xs) + 20, min(ys) - 20, max(ys) + 20
    s = 2.0
    w, h = (x1 - x0) * s, (y1 - y0) * s

    def P(x, y):
        return (x - x0) * s, (y1 - y) * s

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{w:.0f}" height="{h:.0f}" '
           f'style="background:#11161f;font-family:sans-serif">',
           '<defs><marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" '
           'orient="auto"><path d="M0,0L10,5L0,10z" fill="#ddd"/></marker></defs>']
    colors = {"F": "#3fbf5f", "C": "#e0903a", "L": "#8090a8", "R": "#8090a8"}
    for i, (k, ax, ay, bx, by) in coll.items():
        (px, py), (qx, qy) = P(ax, ay), P(bx, by)
        out.append(f'<line x1="{px:.1f}" y1="{py:.1f}" x2="{qx:.1f}" y2="{qy:.1f}" '
                   f'stroke="{colors.get(k, "#888")}" stroke-width="{3 if k == "F" else 1.5}"/>')
    style = {"LINK_JUMP": ("#ff6060", ""), "LINK_DROP": ("#6f8fff", ' stroke-dasharray="6,4"'),
             "LINK_HOP": ("#f0c040", ' stroke-dasharray="3,3"')}
    for a, b, kind, r in links:
        (px, py), (qx, qy) = P(*nodes[a][:2]), P(*nodes[b][:2])
        c, dash = style.get(kind, ("#999", ""))
        out.append(f'<line x1="{px:.1f}" y1="{py:.1f}" x2="{qx:.1f}" y2="{qy:.1f}" stroke="{c}" '
                   f'stroke-width="2"{dash} marker-end="url(#a)"/>')
    for n, (x, y, a) in nodes.items():
        px, py = P(x, y)
        out.append(f'<circle cx="{px:.1f}" cy="{py:.1f}" r="4" fill="#f0f0f0"/>')
        out.append(f'<text x="{px + 5:.1f}" y="{py - 6:.1f}" fill="#d8dde6" font-size="12">{n[2:].lower()}</text>')
    out.append('<text x="10" y="20" fill="#d8dde6" font-size="14">Temple CTF nav graph: red jumps, yellow hops off ledges, '
               'blue dashed drops; walks join neighbours along each floor</text></svg>')
    open(path, "w").write("\n".join(out))


if __name__ == "__main__":
    sys.exit(main())
