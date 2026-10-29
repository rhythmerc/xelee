#!/usr/bin/env python3
"""XeleeNet through the game's own menus (docs/xeleenet-plan.md, wireframe
M2 and J1-J3): N instances on one machine and a local lobby service.

    tools/net_test_xn_menu.py [--players 2|3|4] [--seconds 60] [--block 1:2]
                              [--exe build/linux/melee] [--disc ...]
                              [--work /tmp/net_test_xn_menu] [--shots DIR]

Player 1 walks Online Mode > XeleeNet > Host, opens the lobby from the Host
page (H1) and waits in the CSS, which is the lobby; its code comes off its
log. The others walk to Join, which opens the lobby browser (B1): the first
types the code on the keypad (J1), the rest pick the lobby off the list. Each join
reloads everyone's CSS with a door more (the roster reload). Then every
instance must agree one stage at the SSS and play a match with no DESYNC. Each screen on the way is captured into --shots (default
<work>/shots) for the share.

The names come from MELEE_XN_NAME, so the first-visit keyboard is skipped,
and each instance has its own key (MELEE_XN_IDENTITY).
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "xeleenet"))
import net_test as nt  # noqa: E402
import test_service  # noqa: E402

KEYS = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # xeleenet_menu.cpp kCodeKeys
COLS = 9                                   # its kKeysAcross
TO_ONLINE = [("Down", 7), ("Down", 7), ("X", 7)]  # main menu > Online Mode
STEP = 0.35
# Players' names as people pick them; one at the service's cap (12).
NAMES = ["RHYTHM", "PIXEL", "WAVEDASH KID", "KOOPA"]


def shot(inst, shots, name):
    out = os.path.join(shots, f"{name}.png")
    env = dict(os.environ, MELEE_WINDOW_TITLE=f"melee-net-{inst.name}")
    r = subprocess.run([sys.executable, os.path.join(HERE, "devctl.py"), "shot", out], env=env,
                       capture_output=True, text=True)
    print(f"net_test_xn_menu: shot {name}" + ("" if r.returncode == 0 else f" FAILED {r.stderr}"),
          flush=True)


def walk(inst, keys):
    for key, frames in keys:
        nt.press((inst,), key, frames, 0)
        nt.settle((inst,), nt.MENU_SETTLE)


def keypad():
    """The keypad's items as xeleenet_menu.cpp lays them out: (id, row, col)."""
    items = [(ch, i // COLS, i % COLS) for i, ch in enumerate(KEYS)]
    n = len(KEYS)
    items.append(("del", n // COLS, n % COLS))
    return items


def step(items, cur, key):
    """Join::move: the nearest enabled item that way, ties to the first."""
    r0, c0 = items[cur][1], items[cur][2]
    best, best_score = cur, None
    for i, (_, r, c) in enumerate(items):
        if i == cur:
            continue
        dr, dc = r - r0, c - c0
        ok, score = {"Up": (dr < 0, -dr * 10 + abs(dc)), "Down": (dr > 0, dr * 10 + abs(dc)),
                     "Left": (dr == 0 and dc < 0, -dc), "Right": (dr == 0 and dc > 0, dc)}[key]
        if ok and (best_score is None or score < best_score):
            best, best_score = i, score
    return best


def type_code(inst, code):
    """The keypad: arrows to each character the way the game moves, A types it."""
    items = keypad()
    cur = 0
    for ch in code:
        want = next(i for i, it in enumerate(items) if it[0] == ch)
        for _ in range(12):
            if cur == want:
                break
            r, c = items[cur][1], items[cur][2]
            wr, wc = items[want][1], items[want][2]
            key = "Down" if wr > r else "Up" if wr < r else "Right" if wc > c else "Left"
            nt.press((inst,), key, 7, STEP)
            cur = step(items, cur, key)
        nt.press((inst,), "X", 7, STEP)


def drive_css(insts):
    """nt.drive_css for N: every hand sweeps a row of portraits, then every
    player's START (Ready)."""
    settle = nt.settle
    settle(insts, nt.MENU_SETTLE)
    for row, down in enumerate(nt.CSS_ROWS):
        nt.press(insts, "Left+Up", nt.CSS_PIN, 1.2)
        nt.press(insts, "Down", down, 0.8)
        nt.press(insts, "Right", nt.CSS_FIRST, 0.6)
        for _ in range(nt.CSS_COLS):
            nt.press(insts, "X", 7, 0.5)
            nt.press(insts, "Right", nt.CSS_STEP, 0.4)
        # Every player readies with START (the CSS's Ready); the stage select
        # follows a 2 s countdown.
        if nt.press_until(insts, "Return", 9, nt.SCENE_FILE["sss"], tries=2, each=8.0):
            return True
        print(f"net_test_xn_menu: no pick on CSS row {row}", flush=True)
    return False


def sss_loads(inst):
    return len(nt.re.findall(nt.SCENE_FILE["sss"], inst.text()))


def ready_scenario(insts, shots):
    """Ready on the CSS (P2 of docs/xeleenet-ready-plan.md): one player's
    START doesn't start; everyone's starts a countdown; a B in it stops it;
    the stage select follows everyone's START, on every player."""
    host, guest = insts[0], insts[1]
    nt.settle(insts, nt.MENU_SETTLE)
    for down in nt.CSS_ROWS[:2]:  # the first row's sweep often misses (drive_css)
        nt.press(insts, "Left+Up", nt.CSS_PIN, 1.2)
        nt.press(insts, "Down", down, 0.8)
        nt.press(insts, "Right", nt.CSS_FIRST, 0.6)
        for _ in range(nt.CSS_COLS):  # every portrait of the row: the last pick stands
            nt.press(insts, "X", 7, 0.5)
            nt.press(insts, "Right", nt.CSS_STEP, 0.4)
    before = [sss_loads(i) for i in insts]
    nt.press((host,), "Return", 9, 4.0)
    if [sss_loads(i) for i in insts] != before:
        print("FAIL: one player's START left the CSS")
        return False
    if not all("css: P1 ready" in i.text() for i in insts):
        print("FAIL: not every player saw P1 ready")
        return False
    print("net_test_xn_menu: the host alone ready, the CSS stayed", flush=True)
    # The hand onto the legend under the doors: it must draw on top of it.
    nt.press((host,), "Down", 40, 0.3)
    nt.press((host,), "Left", 22, 0.6)
    shot(host, shots, "9b-css-host-ready")
    nt.press((guest,), "Return", 9, 0.5)
    nt.press((guest,), "Z", 9, 3.5)  # inside the 2 s countdown
    if [sss_loads(i) for i in insts] != before:
        print("FAIL: B in the countdown didn't stop it")
        return False
    if not all("css: the countdown stopped" in i.text() for i in insts):
        print("FAIL: not every player saw the countdown stop")
        return False
    print("net_test_xn_menu: B stopped the countdown", flush=True)
    nt.press((guest,), "Return", 9, 0.5)
    deadline = time.time() + 15
    while time.time() < deadline and not all(sss_loads(i) > n for i, n in zip(insts, before)):
        time.sleep(0.5)
    if not all(sss_loads(i) > n for i, n in zip(insts, before)):
        print("FAIL: everyone ready, but no stage select")
        return False
    print("net_test_xn_menu: everyone ready, the stage select on every player", flush=True)
    return True


def css_loads(inst):
    return len(nt.re.findall(nt.SCENE_FILE["css"], inst.text()))


def sss_host_b(insts, shots):
    """The stage select in a lobby: a guest's B does nothing; a guest's A on a
    stage off the lobby's list is refused; the host's B takes everyone back to
    the CSS, which picks again (P1 of docs/xeleenet-ready-plan.md)."""
    host, guest = insts[0], insts[1]
    before = [css_loads(i) for i in insts]
    nt.press((guest,), "Z", 9, 2.0)
    if [css_loads(i) for i in insts] != before:
        print("FAIL: a guest's B left the stage select")
        return False
    print("net_test_xn_menu: a guest's B did nothing", flush=True)
    # The cursor starts under the bottom row; straight up are rows of stages
    # off the Competitive list (up-left is RANDOM, a valid pick). Step up
    # until one refuses a pick.
    refused = False
    for frames in (12, 12, 12, 12, 12):
        nt.press((guest,), "Up", frames, 0.4)
        if "is off the lobby's list" in guest.text():
            nt.press((guest,), "X", 7, 1.2)  # A there: nothing to pick
            refused = "sss: we picked" not in guest.text()
            break
    if not refused:
        print("FAIL: no off-list stage under the cursor, or A there picked it")
        return False
    print("net_test_xn_menu: an off-list stage took no hover and no pick", flush=True)
    shot(guest, shots, "10b-sss-guest")
    nt.press((host,), "Z", 9, 1.0)
    deadline = time.time() + 30
    while time.time() < deadline and not all(css_loads(i) > n for i, n in zip(insts, before)):
        time.sleep(0.5)
    if not all(css_loads(i) > n for i, n in zip(insts, before)):
        print("FAIL: the host's B didn't take everyone back to the CSS")
        return False
    print("net_test_xn_menu: the host's B took everyone back to the CSS", flush=True)
    time.sleep(3)
    if not drive_css(insts):
        print("FAIL: no CSS pick reached the SSS the second time")
        return False
    time.sleep(2)
    return True


# The stage select's cells (mnstagesel.c's table) on the Competitive list.
BF, FD, DL, PS = 24, 25, 26, 18  # Pokemon Stadium: the upper grid, straight up from Dream Land


def over(inst):
    """The listed cell the cursor was last over (mnstagesel.c logs it)."""
    m = nt.re.findall(r"sss: over cell (\d+)", inst.text())
    return int(m[-1]) if m else -1


def steer(inst, key, cell, tries=40):
    """Tap the stick toward `key` until the cursor is over `cell`. The cursor
    starts under the bottom row: straight up is Battlefield, and Final
    Destination and Dream Land follow to its right."""
    for _ in range(tries):
        if over(inst) == cell:
            return True
        nt.press((inst,), key, 3, 0.15)
    print(f"[{inst.name}] FAIL: the cursor never reached cell {cell} (over {over(inst)})")
    return False


def finals(inst):
    """Each FINAL line's picks and cards, as this player logged it."""
    return nt.re.findall(r"sss: final(?: from the host)?: (picks .*?, cards [-\d ]+), timed out (\w+), "
                         r"P(\d)'s card", inst.text())


def sss_picks(insts, shots, scenario):
    """The picks card, the draw and the timer (P5 of
    docs/xeleenet-ready-plan.md). sss-same: everyone picks Battlefield and
    there's no draw. sss-repick: P2 changes its pick twice before the last
    pick, then once more racing P3's (P2 has added delay); everyone draws from
    the same FINAL. sss-timeout: P3 never picks, and the host ends the picking
    15 s after the first pick with P3 as Random."""
    host, b = insts[0], insts[1]
    c = insts[2] if len(insts) > 2 else None
    time.sleep(1.0)
    if scenario == "sss-same":
        for i in insts:
            if not steer(i, "Up", BF):
                return False
            nt.press((i,), "X", 7, 0.3)
        time.sleep(0.6)
        shot(host, shots, "10c-sss-draw-same")
    elif scenario == "sss-repick":
        if c is None:
            print("FAIL: sss-repick wants three players")
            return False
        # P2: Battlefield, then Final Destination, then Dream Land.
        if not steer(b, "Up", BF):
            return False
        nt.press((b,), "X", 7, 0.5)
        shot(b, shots, "10c-sss-card-p2")
        for cell in (FD, DL):
            if not steer(b, "Right", cell):
                return False
            nt.press((b,), "X", 7, 0.5)
        # The host picks Battlefield; then P3's Random and P2's last change
        # (back to Final Destination) go in together.
        if not steer(host, "Up", BF):
            return False
        nt.press((host,), "X", 7, 0.8)
        shot(host, shots, "10d-sss-card-host")
        if not steer(b, "Left", FD):
            return False
        b.key("X 120")
        c.key("Return 120")
        time.sleep(0.9)
        shot(host, shots, "10e-sss-draw")
        time.sleep(1.1)
        shot(host, shots, "10f-sss-landed")
    elif scenario == "sss-timeout":
        if c is None:
            print("FAIL: sss-timeout wants three players")
            return False
        for i in (host, b):
            if not steer(i, "Up", BF):
                return False
        nt.press((host,), "X", 7, 0.2)
        t0 = time.time()
        # A frame of the upper grid's shape: along to Dream Land, then up.
        if not all(steer(b, key, cell) for key, cell in (("Right", FD), ("Right", DL), ("Up", PS))):
            return False
        nt.press((b,), "X", 7, 0.5)
        time.sleep(4)
        shot(c, shots, "10c-sss-card-timer")
        if not host.wait_log(r"sss: final: ", 30):
            print("FAIL: the pick timer never ended the picking")
            return False
        took = time.time() - t0
        print(f"net_test_xn_menu: the timer ended the picking {took:.1f} s after the first pick", flush=True)
        if not 13 <= took <= 19:
            print("FAIL: not 15 s after the first pick")
            return False
    if not all(i.wait_log(r"sss: resolved cell", 40) for i in insts):
        print("FAIL: the SSS never resolved a stage everywhere")
        return False
    got = {i.name: finals(i)[-1:] for i in insts}
    print(f"net_test_xn_menu: FINAL {got}", flush=True)
    if len({tuple(v) for v in got.values()}) != 1 or not got[host.name]:
        print("FAIL: the players drew from different FINALs")
        return False
    picks, timed_out, chosen = got[host.name][0]
    if scenario == "sss-same":
        if "(one stage: no draw)" not in host.text() or f"cards {BF} {BF}" not in picks:
            print("FAIL: everyone picked Battlefield, but there was a draw")
            return False
    elif scenario == "sss-repick":
        p2 = int(nt.re.search(r"P2=(-?\d+)", picks).group(1))
        if p2 not in (FD, DL):
            print(f"FAIL: P2's FINAL pick {p2} is neither its last change nor the one before")
            return False
        print(f"net_test_xn_menu: P2's pick in FINAL {p2} ({'the race won' if p2 == FD else 'the race lost'})",
              flush=True)
    elif scenario == "sss-timeout":
        if int(timed_out, 16) != 4 or "P3=30" not in picks:
            print("FAIL: P3 didn't count as Random after the timer")
            return False
    return True


def menu_loads(inst):
    return len(nt.re.findall(nt.SCENE_FILE["menu"], inst.text()))


def leave_guest(insts, shots):
    """A guest holding B leaves alone (P4 of docs/xeleenet-ready-plan.md): it
    lands on the XeleeNet pages' lobby list; everyone else stays, and the CSS
    reloads without it. The leaver joins again from the list straight away,
    and run() goes on to a match with everyone."""
    host, leaver = insts[0], insts[-1]
    stay = insts[:-1]
    nt.settle(insts, nt.MENU_SETTLE)
    # Everyone picks first: the picks that stand go on through each reload.
    for down in nt.CSS_ROWS[:2]:
        nt.press(insts, "Left+Up", nt.CSS_PIN, 1.2)
        nt.press(insts, "Down", down, 0.8)
        nt.press(insts, "Right", nt.CSS_FIRST, 0.6)
        for _ in range(nt.CSS_COLS):
            nt.press(insts, "X", 7, 0.5)
            nt.press(insts, "Right", nt.CSS_STEP, 0.4)
    menus = [menu_loads(i) for i in insts]
    # One hold of 1.5 s (the leave takes 1 s of it), shot two thirds of the
    # way to leaving: the ring and the red notice.
    leaver.key(f"Z {int(90 * nt.frame_ms(leaver))}")
    time.sleep(40 * nt.frame_ms(leaver) / 1000)
    shot(leaver, shots, "9c-css-leaving")
    time.sleep(4.0)
    if any(menu_loads(i) > n for i, n in zip(stay, menus)):
        print("FAIL: a guest's B hold took others out of the lobby")
        return False
    if f"css: P{len(insts)} left the lobby" not in leaver.text():
        print("FAIL: the leaver never left")
        return False
    stay_mask = (1 << len(stay)) - 1
    if len(stay) == 1:
        # A 1v1: the host is alone again, no session; its CSS reloads at once.
        if not host.wait_log(r"the host is alone in the lobby again", 30):
            print("FAIL: the host didn't go back to waiting alone")
            return False
    else:
        want = f"(players {stay_mask:02x})"
        for i in stay:
            if not i.wait_log(nt.re.escape(want), 30):
                print(f"[{i.name}] FAIL: no session without the leaver {want}")
                return False
        if not all(i.wait_log(r"online: XeleeNet roster changed", 30) for i in stay):
            print("FAIL: the CSS never reloaded without the leaver")
            return False
    kept = nt.re.escape(f"picks kept {stay_mask:02x} (of {stay_mask:02x})")
    if not all(i.wait_log(kept, 10) for i in stay):
        print("FAIL: the stayers' picks didn't survive the reload")
        return False
    print("net_test_xn_menu: the guest left alone; the others reloaded without it, picks kept", flush=True)
    time.sleep(3)
    shot(host, shots, "9d-css-after-leave")
    shot(leaver, shots, "9e-leaver-menu")
    # Straight back in: the leaver lands on the lobby list, and A on it joins
    # the same lobby again (the host must not take it for the stay it left).
    full = f"(players {(1 << len(insts)) - 1:02x})"
    seen = [i.text().count(full) for i in insts]
    nt.press((leaver,), "X", 7, 0.4)
    for i, n in zip(insts, seen):
        deadline = time.time() + 60
        while time.time() < deadline and i.text().count(full) <= n:
            time.sleep(0.5)
        if i.text().count(full) <= n:
            print(f"[{i.name}] FAIL: the leaver never got back in {full}")
            return False
    kept = nt.re.escape(f"picks kept {stay_mask:02x} (of {(1 << len(insts)) - 1:02x})")
    if not all(i.wait_log(kept, 60) for i in insts):
        print("FAIL: the picks didn't reach everyone, the newcomer included")
        return False
    print("net_test_xn_menu: the leaver joined again from the list; the picks went on", flush=True)
    time.sleep(4)
    shot(host, shots, "9f-css-rejoined")
    return True


def leave_host(insts, shots):
    """The host holding B closes the lobby (P4): every player lands on the
    XeleeNet pages, the guests on the lobby list with a note."""
    host, guests = insts[0], insts[1:]
    nt.settle(insts, nt.MENU_SETTLE)
    host.key(f"Z {int(90 * nt.frame_ms(host))}")
    time.sleep(4.0)
    if not all("css: P1 left the lobby" in i.text() for i in insts):
        print("FAIL: not everyone saw the host's hold end")
        return False
    if not all("online: XeleeNet the host closed the lobby" in g.text() for g in guests):
        print("FAIL: a guest didn't take it as the lobby closing")
        return False
    if any("DESYNC" in i.text() or "peer left" in i.text() for i in insts):
        print("FAIL: the close went through as a failure")
        return False
    time.sleep(2)
    shot(host, shots, "9g-host-after-close")
    shot(guests[0], shots, "9h-guest-after-close")
    print("net_test_xn_menu: the host closed the lobby; everyone is back on the XeleeNet pages", flush=True)
    return True


def run(args):
    names = "abcd"[:args.players]
    shots = args.shots or os.path.join(args.work, "shots")
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(shots)
    ports = [args.port + k for k in range(args.players)]
    for p in ports:
        if not nt.wait_port_free(p):
            return False
    insts = []
    for k, name in enumerate(names):
        ident = os.path.join(args.work, f"{name}.id")
        os.makedirs(ident)
        env = {
            "MELEE_XN_URL": args.url,
            "MELEE_XN_NAME": NAMES[k],
            "MELEE_XN_IDENTITY": ident,
            "MELEE_NET_EXIT_AFTER_FRAMES": str(int(args.seconds * 60) + 30000),
        }
        if args.block:
            env["MELEE_NET_SIM_BLOCK"] = args.block
        if args.scenario == "sss-repick" and k == 1:
            env["MELEE_NET_SIM_DELAY_MS"] = "80"  # P2's last change races P3's pick
        insts.append(nt.Instance(name, args.exe, args.disc, args.work, ports[k], 0, env, True))
    ok = False
    try:
        for inst in insts:
            if not nt.to_main_menu((inst,)):
                print(f"[{inst.name}] FAIL: never reached the main menu")
                return False
        host, guests = insts[0], insts[1:]
        walk(host, TO_ONLINE)
        shot(host, shots, "1-online-mode")
        walk(host, [("X", 7)])
        shot(host, shots, "2-xeleenet-menu")
        walk(host, [("X", 7)])
        time.sleep(1.5)
        if args.players > 2:
            nt.press((host,), "Right", 7, 0.6)  # Preset: 1v1 (2 seats) -> Doubles (4)
        shot(host, shots, "3-host-page")
        nt.press((host,), "Return", 9, 1.0)  # START: open the lobby
        if not host.wait_log(r"xeleenet: lobby code [A-Z0-9]{6}", 60):
            print("FAIL: the host never opened a lobby")
            return False
        code = nt.re.search(r"xeleenet: lobby code ([A-Z0-9]{6})", host.text()).group(1)
        print(f"net_test_xn_menu: lobby code {code}", flush=True)
        if not host.wait_log(nt.SCENE_FILE["css"], 60):
            print("FAIL: the host never reached the CSS")
            return False
        time.sleep(3)
        shot(host, shots, "4-host-css-alone")
        for k, g in enumerate(guests):
            # Join opens the lobby browser (B1), which lists the host's lobby.
            walk(g, TO_ONLINE + [("X", 7), ("Down", 7), ("X", 7)])
            time.sleep(2.5)
            shot(g, shots, f"5-lobbies-{g.name}")
            if k == 0:
                # The first guest types the code: Y opens the keypad (J1).
                nt.press((g,), "V", 7, 1.0)
                shot(g, shots, "6-join-keypad")
                type_code(g, code)
                shot(g, shots, "7-join-code-typed")
                nt.press((g,), "Return", 9, 0.4)
            else:
                # The others pick the lobby off the list with A.
                nt.press((g,), "X", 7, 0.4)
            time.sleep(0.6)
            shot(g, shots, f"8-joining-{g.name}")
            # Seated: this guest's own session agreed and its CSS entered.
            if not g.wait_log(r"lobby: entering CSS", 90):
                print(f"[{g.name}] FAIL: never got into the lobby")
                return False
        want = f"(players {(1 << len(insts)) - 1:02x})"
        for inst in insts:
            if not inst.wait_log(nt.re.escape(want), 90):
                print(f"[{inst.name}] FAIL: never in a session with everyone {want}")
                return False
        if not all(i.wait_log(nt.SCENE_FILE["css"], 120) for i in insts):
            print("FAIL: the CSS never loaded everywhere")
            return False
        time.sleep(4)
        shot(host, shots, "9-css")
        if args.scenario == "ready":
            if not ready_scenario(insts, shots):
                return False
        if args.scenario == "leave-guest":
            if not leave_guest(insts, shots):
                return False
        if args.scenario == "leave-host":
            ok = leave_host(insts, shots)
            return ok
        if args.scenario != "ready" and not drive_css(insts):
            print("FAIL: no CSS pick reached the SSS")
            return False
        time.sleep(2)
        shot(host, shots, "10-sss")
        if args.scenario == "sss-host-b" and not sss_host_b(insts, shots):
            return False
        if args.scenario.startswith("sss-") and args.scenario != "sss-host-b":
            if not sss_picks(insts, shots, args.scenario):
                return False
        elif not nt.press_until(insts, "Return", 9, r"sss: resolved cell", tries=8, each=6.0):
            print("FAIL: the SSS never resolved a stage everywhere")
            return False
        cells = {i.name: nt.re.findall(r"sss: resolved cell (\d+)", i.text())[-1:] for i in insts}
        print(f"net_test_xn_menu: stages {cells}", flush=True)
        if len({tuple(v) for v in cells.values()}) != 1:
            print("FAIL: the players resolved different stages")
            return False
        # Inputs from here on: an idle match's checksum never moves, and
        # wait_match only counts a match whose state does.
        drive = __import__("net_test_n").Drive(insts)
        # In the fight on every instance (gmonlinemode.c logs the VS entry);
        # nt.wait_match's moving-checksum test wants busier hands than these.
        if not all(i.wait_log(r"online: enter VS", 180) for i in insts):
            drive.done()
            print("FAIL: the match never loaded")
            return False
        time.sleep(12)
        shot(host, shots, "11-match")
        time.sleep(args.seconds)
        drive.done()
        shot(host, shots, "12-match-later")
        texts = [i.text() for i in insts]
        ok = not any(nt.re.search(r"net: DESYNC", t) for t in texts)
        for i, t in zip(insts, texts):
            print(f"[{i.name}] " + "; ".join(nt.re.findall(r"xeleenet: (P\d (?:direct|through the host))", t))
                  + ("  DESYNC" if "net: DESYNC" in t else ""))
    finally:
        for inst in insts:
            inst.kill(5)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--players", type=int, choices=(2, 3, 4), default=3)
    ap.add_argument("--seconds", type=float, default=40, help="of match after it loads")
    ap.add_argument("--block", default="")
    ap.add_argument("--scenario", default="",
                    choices=("", "sss-host-b", "ready", "leave-guest", "leave-host", "sss-repick", "sss-timeout",
                             "sss-same"),
                    help="sss-host-b: guest B and off-list picks refused, host B back to the CSS; "
                         "ready: Ready on the CSS (one START, the countdown, B stops it); "
                         "leave-guest: the last guest holds B and leaves alone, then joins again; "
                         "leave-host: the host holds B and closes the lobby; "
                         "sss-repick: picks change, one racing the last pick; "
                         "sss-timeout: P3 never picks, the 15 s timer ends it; "
                         "sss-same: everyone picks Battlefield, no draw")
    ap.add_argument("--url", default="", help="a running service instead of a local wrangler dev")
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "build", "linux", "melee"))
    ap.add_argument("--disc", default=os.path.expanduser(
        "~/Downloads/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso"))
    ap.add_argument("--port", type=int, default=42250)
    ap.add_argument("--work", default=os.path.join(HERE, "..", "build", "net_test_xn_menu"))
    ap.add_argument("--shots", default="")
    args = ap.parse_args()
    service = None
    if not args.url:
        proc, args.url, state = test_service.start_wrangler(short=False)
        service = (proc, state)
    try:
        ok = run(args)
    finally:
        if service:
            test_service.stop_wrangler(*service)
    print("net_test_xn_menu: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
