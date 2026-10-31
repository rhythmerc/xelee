#!/usr/bin/env python3
"""XeleeNet through the game's own menus (docs/xeleenet-plan.md, wireframe
M2 and J1-J3): N instances on one machine and a local lobby service.

    tools/net_test_xn_menu.py [--players 2|3|4] [--seconds 60] [--block 1:2]
                              [--exe build/linux/melee] [--disc ...]
                              [--work /tmp/net_test_xn_menu] [--shots DIR]

Player 1 walks Online Mode > XeleeNet > Host, opens the lobby from the Host
page (H1) and waits in the CSS, which is the lobby; its code comes off its
log. The others walk to Join, which opens the lobby browser (B1): the first
types the code on the keypad (J1), the rest pick the lobby off the list (A for its
Details, A again to join). Each join
reloads everyone's CSS with a door more (the roster reload). Then every
instance must agree one stage at the SSS and play a match with no DESYNC. Each screen on the way is captured into --shots (default
<work>/shots) for the share.

The names come from MELEE_XN_NAME, so the first-visit keyboard is skipped,
and each instance has its own key (MELEE_XN_IDENTITY).

--net lan does the same on LAN (docs/lan-lobbies-plan.md), with no service:
Online Mode > LAN, the host's game is the lobby service, and the first
guest joins by typing the address (127.0.0.1) on LAN's keypad instead of
the code. --no-discovery (LAN) runs every instance on a network that drops
multicast (MELEE_NET_SIM_NO_MCAST): the list must say discovery is blocked,
and every guest joins by address.
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
TO_LAN_ROW = [("Down", 7), ("Down", 7)]          # Online Mode: XeleeNet, Slippi, LAN
ADDRESS_KEYS = "123456789.0"                     # LAN's keypad, 3 across, then DEL
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


def address_keypad():
    items = [(ch, i // 3, i % 3) for i, ch in enumerate(ADDRESS_KEYS)]
    items.append(("del", 3, 2))
    return items


def type_code(inst, code, items=None):
    """The keypad: arrows to each character the way the game moves, A types it."""
    items = items or keypad()
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


def open_details(inst, timeout=15):
    """A on the highlighted row of the lobby list: the lobby's Details
    (docs/xeleenet-lobby-details-plan.md). Its log line, or None."""
    seen = nt.count(inst, r"xeleenet pages: details ")
    deadline = time.time() + timeout
    while time.time() < deadline:
        nt.press((inst,), "X", 7, 0.3)
        # Another A only once this one has surely missed (the list may still
        # be looking): a second on Details would join.
        for _ in range(10):
            if nt.count(inst, r"xeleenet pages: details ") > seen:
                return nt.re.findall(r"xeleenet pages: details .*", inst.text())[-1]
            time.sleep(0.3)
    return None


def join_off_list(inst):
    """A on the list's row opens Details; A there joins."""
    if open_details(inst) is None:
        print(f"[{inst.name}] FAIL: A on the list never opened a lobby's Details")
        return False
    nt.press((inst,), "X", 7, 0.4)
    return True


def drive_css(insts, want=1):
    """The CSS for N players: every hand sweeps a row of portraits, then every
    player's START (Ready), until the stage select has loaded `want` times."""
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
        if nt.press_until(insts, "Return", 9, nt.SCENE_FILE["sss"], want=want, tries=2, each=8.0):
            return True
        print(f"net_test_xn_menu: no pick on CSS row {row}", flush=True)
    return False


def sss_loads(inst):
    return len(nt.re.findall(nt.SCENE_FILE["sss"], inst.text()))


def ready_scenario(insts, shots):
    """Ready on the CSS (P2 of docs/xeleenet-ready-plan.md): one player's
    START doesn't start; everyone's starts a countdown; a B in it stops it;
    the stage select follows everyone's START, on every player. Any player
    count: every guest readies, and the first guest's B stops it."""
    host, guest, guests = insts[0], insts[1], insts[1:]
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
    nt.press(guests, "Return", 9, 0.5)  # the last of them starts the countdown
    nt.press((guest,), "Z", 9, 3.5)     # inside the 2 s countdown
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


class PadHand:
    """Another pad of an instance's machine (MELEE_KEY_FIFO_P2..), driven like
    an instance: nt.press and the drives take it in a list of instances."""

    def __init__(self, inst, pad):
        self.inst, self.pad = inst, pad
        self.fifo = os.path.join(os.path.dirname(inst.fifo), f"{inst.name}.pad{pad}.keys")
        self.name = f"{inst.name}{pad}"

    def key(self, line):
        nt.fifo_write(self.fifo, line)

    def __getattr__(self, attr):
        return getattr(self.inst, attr)


def multipad_scenario(insts, pad, shots):
    """More than one player on one machine (docs/lan-multipad-plan.md): the
    host's second pad presses START on the CSS and gets the open seat, P3;
    everyone's CSS reloads with it. Its hold of B gives the seat up again
    (the host stays), and a second START takes it back. run() then plays a
    match with every seat, the second pad's included."""
    host = insts[0]
    nt.settle(insts, nt.MENU_SETTLE)
    time.sleep(1.5)
    shot(host, shots, "mp-1-css-spare")
    seated = "(players 07"
    for attempt in range(2):
        before = [i.text().count(seated) for i in insts]
        loads = [css_loads(i) for i in insts]
        pad.key("Return 150")
        for i, n in zip(insts, before):
            deadline = time.time() + 40
            while time.time() < deadline and i.text().count(seated) <= n:
                time.sleep(0.5)
            if i.text().count(seated) <= n:
                print(f"[{i.name}] FAIL: no session with the second pad's seat {seated})")
                return False
        if not host.wait_log(r"net: this machine plays ports 5 \(P1<-pad 1 P3<-pad 2\)", 30):
            print("FAIL: the host doesn't play P3 from its second pad")
            return False
        if not reloaded(insts, loads):
            return False
        time.sleep(1.5)
        shot(host, shots, f"mp-{2 + 3 * attempt}-css-added-host")
        shot(insts[1], shots, f"mp-{2 + 3 * attempt}-css-added-guest")
        print(f"net_test_xn_menu: the second pad plays P3 ({'again' if attempt else 'first'})", flush=True)
        if attempt:
            break
        # The second pad gives its seat up: a hold of B on it. The host stays.
        menus = [menu_loads(i) for i in insts]
        before = [i.text().count("(players 03") for i in insts]
        loads = [css_loads(i) for i in insts]
        pad.key(f"Z {int(90 * nt.frame_ms(host))}")
        time.sleep(40 * nt.frame_ms(host) / 1000)
        shot(host, shots, "mp-3-css-giving-up")
        for i, n in zip(insts, before):
            deadline = time.time() + 40
            while time.time() < deadline and i.text().count("(players 03") <= n:
                time.sleep(0.5)
            if i.text().count("(players 03") <= n:
                print(f"[{i.name}] FAIL: the seat was never given up")
                return False
        if any(menu_loads(i) > n for i, n in zip(insts, menus)):
            print("FAIL: giving up the second pad's seat took someone out of the lobby")
            return False
        if not reloaded(insts, loads):
            return False
        time.sleep(1.5)
        shot(host, shots, "mp-4-css-given-up")
        print("net_test_xn_menu: the second pad gave its seat up; the host stayed", flush=True)
    return True


def reloaded(insts, loads, timeout=60):
    """Every CSS has loaded again since `loads` (css_loads per instance)."""
    for i, n in zip(insts, loads):
        deadline = time.time() + timeout
        while time.time() < deadline and css_loads(i) <= n:
            time.sleep(0.3)
        if css_loads(i) <= n:
            print(f"[{i.name}] FAIL: the CSS never reloaded")
            return False
    return True


def multipad_full(insts, pad, shots):
    """A spare pad's START with no seat open: a notice on that machine only,
    and no session change."""
    host = insts[0]
    nt.settle(insts, nt.MENU_SETTLE)
    pad.key("Return 150")
    if not host.wait_log(r"css: controller 2 asks for a seat; the lobby is full", 15):
        print("FAIL: the full lobby's START wasn't refused")
        return False
    time.sleep(0.8)
    shot(host, shots, "mp-full")
    if "(players 07" in host.text():
        print("FAIL: a seat came anyway")
        return False
    print("net_test_xn_menu: the full lobby refused the second pad", flush=True)
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
        # P2: Battlefield, then Final Destination, then Dream Land; a change
        # is B (the pick back), then A. A with a pick does nothing.
        if not steer(b, "Up", BF):
            return False
        nt.press((b,), "X", 7, 0.5)
        shot(b, shots, "10c-sss-card-p2")
        for cell in (FD, DL):
            if not steer(b, "Right", cell):
                return False
            nt.press((b,), "X", 7, 0.4)  # refused: P2 has a pick
            nt.press((b,), "Z", 7, 0.4)  # B: the pick back
            nt.press((b,), "X", 7, 0.5)
        if not host.wait_log(r"sss: P2 took their pick back", 5):
            print("FAIL: P2's B didn't take its pick back")
            return False
        # The host picks Battlefield; then P3's Random and P2's last change
        # (back to Final Destination) go in together.
        if not steer(host, "Up", BF):
            return False
        nt.press((host,), "X", 7, 0.8)
        shot(host, shots, "10d-sss-card-host")
        if not steer(b, "Left", FD):
            return False
        nt.press((b,), "Z", 7, 0.1)
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
    if not join_off_list(leaver):
        return False
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
    # The rejoin goes straight from Joining to the CSS: a B left over from
    # the leave's hold once read as back, and the list showed for a while
    # (a Quest find).
    rejoin = leaver.text()[leaver.text().rfind("xeleenet pages: joining"):]
    if "xeleenet pages: Joining -> Lobbies" in rejoin:
        print("FAIL: the rejoin went back to the lobby list before the CSS")
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


def css_b_hold(insts, shots):
    """B held over the character grid (a Quest find, 2026-10-29): Melee's B
    takes the token back, and B again puts it down, so a held B mustn't count
    as a press every frame. A hold short of leaving touches nothing: the pick
    stays, and START then readies it."""
    holder = insts[-1]
    port = len(insts)
    nt.settle(insts, nt.MENU_SETTLE)
    for down in nt.CSS_ROWS[:2]:
        nt.press((holder,), "Left+Up", nt.CSS_PIN, 1.2)
        nt.press((holder,), "Down", down, 0.8)
        nt.press((holder,), "Right", nt.CSS_FIRST, 0.6)
        for _ in range(nt.CSS_COLS):
            nt.press((holder,), "X", 7, 0.5)
            nt.press((holder,), "Right", nt.CSS_STEP, 0.4)
    nt.press((holder,), "Return", 9, 1.0)
    if not holder.wait_log(rf"css: P{port} ready", 5):
        print("FAIL: no pick to test with")
        return False
    # Back onto the row's portraits, with nothing in hand.
    nt.press((holder,), "Left", 2 * nt.CSS_STEP, 0.8)
    shot(holder, shots, "9i-b-hold-hand-on-grid")
    flips = lambda: len(nt.re.findall(rf"css: P{port} B (?:took the token back|put the token down)", holder.text()))
    before = flips()
    holder.key(f"Z {int(40 * nt.frame_ms(holder))}")  # 40 frames: short of the 60 that leave
    time.sleep(40 * nt.frame_ms(holder) / 1000 + 1.0)
    moved = flips() - before
    print(f"net_test_xn_menu: a 40-frame B over the grid moved the token {moved} time(s)", flush=True)
    if moved != 0:
        print("FAIL: B over the character grid moved the token")
        return False
    nt.press((holder,), "Return", 9, 1.0)
    if not holder.wait_log(rf"css: P{port} ready", 5):
        print("FAIL: after the B hold, START didn't ready the pick (it was lost)")
        return False
    print("net_test_xn_menu: the pick stayed through the B hold, and START readied it", flush=True)
    return True


# Melee's door layout (mncharsel.c): door 0's team button and the doors'
# spacing, in the hand's world units. A lobby of fewer than four seats moves
# its doors right by half a spacing a missing seat ("css: N seats").
TEAM_BTN_X = (-26.8, -21.0)
TEAM_BTN_Y = (-5.8, -1.0)
DOOR_PITCH = 15.4
HAND_SPEED = 1.24  # units a frame of a held direction (CSS_FIRST, CSS_STEP)


def teams_scenario(insts, shots):
    """--scenario teams: Teams in a lobby of fewer seats than four, so the
    doors stand moved. The host's hand goes to its own door's team button,
    where the door is drawn, and A turns the team over on every player. The
    hand is steered by where the game says each A landed ("css: P1 A at")."""
    host = insts[0]
    m = nt.re.search(r"css: (\d) seats", host.text())
    seats = int(m.group(1)) if m else 4
    dx = (4 - seats) * DOOR_PITCH / 2
    tx, ty = (TEAM_BTN_X[0] + TEAM_BTN_X[1]) / 2 + dx, (TEAM_BTN_Y[0] + TEAM_BTN_Y[1]) / 2
    print(f"net_test_xn_menu: {seats} seats; P1's team button at {tx:.1f} {ty:.1f}", flush=True)
    nt.settle(insts, nt.MENU_SETTLE)
    turned = lambda i: nt.count(i, r"css: P1's team \d")
    for attempt in range(12):
        nt.press((host,), "X", 4, 0.6)
        if turned(host):
            break
        at = nt.re.findall(r"css: P1 A at (-?[\d.]+) (-?[\d.]+)", host.text())
        if not at:
            print("FAIL: the host's A was never seen on the CSS")
            return False
        x, y = float(at[-1][0]), float(at[-1][1])
        print(f"net_test_xn_menu: A at {x:.1f} {y:.1f}", flush=True)
        for d, plus, minus in ((tx - x, "Right", "Left"), (ty - y, "Up", "Down")):
            frames = round(abs(d) / HAND_SPEED)
            if frames > 0:
                nt.press((host,), plus if d > 0 else minus, frames, 0.4)
    else:
        shot(host, shots, "9t-team-missed")
        print("FAIL: A on the moved door's team button never turned its team")
        return False
    time.sleep(1.5)
    shot(host, shots, "9t-team-turned")
    lines = [nt.re.findall(r"css: P1's team \d", i.text()) for i in insts]
    print(f"net_test_xn_menu: {lines[0][-1]} after {attempt + 1} A press(es)", flush=True)
    if any(t != lines[0] for t in lines):
        print(f"FAIL: the players disagree on the team: {lines}")
        return False
    return True


def leave_host_alone(host, shots):
    """The host alone in its lobby holds B (a Quest find, 2026-10-29): with no
    session yet, the lobby still closes, and the host lands on the XeleeNet
    pages."""
    menus = menu_loads(host)
    host.key(f"Z {int(90 * nt.frame_ms(host))}")
    time.sleep(40 * nt.frame_ms(host) / 1000)
    shot(host, shots, "4b-host-alone-leaving")
    deadline = time.time() + 15
    while time.time() < deadline and menu_loads(host) == menus:
        time.sleep(0.5)
    if "css: P1 left the lobby" not in host.text():
        print("FAIL: the host's hold never ended")
        return False
    if menu_loads(host) == menus:
        print("FAIL: the host's hold ended, but it never left the lobby")
        return False
    time.sleep(2)
    shot(host, shots, "4c-host-alone-after")
    print("net_test_xn_menu: the host alone left its lobby for the XeleeNet pages", flush=True)
    return True


# --scenario set: the host's Host page loads these rules (xeleenet-lobby.txt in
# its profile, as tools/xn_bot.py writes it): 1-minute time matches, so the
# first one runs out to the results by itself.
COMPETITIVE = sum(1 << b for b in (24, 25, 26, 4, 5, 7))  # xeleenet_menu.cpp kCompetitive


# The fighter list's Low tiers (src/pc/xeleenet_fighters.h): every playable
# CharacterKind but the High tiers' (Zelda and Sheik together).
FIGHTERS_ALL = (1 << 26) - 1
FIGHTERS_HIGH = sum(1 << b for b in (0x02, 0x09, 0x0F, 0x14, 0x12, 0x13, 0x00, 0x0C, 0x0E, 0x0D, 0x11, 0x10,
                                     0x07, 0x16))
FIGHTERS_LOW = FIGHTERS_ALL & ~FIGHTERS_HIGH


def set_rules(ident, seats, teams=False, fighters=FIGHTERS_ALL, stages=COMPETITIVE):
    with open(os.path.join(ident, "xeleenet-lobby.txt"), "w") as f:
        f.write(f"preset=3\nname=Set test\nopen=1\nseats={seats}\nteams={int(teams)}\nstock=0\nstocks=4\n"
                f"minutes=1\nitems=-1\nff=0\nhandicap=0\ndamage=10\npause=0\nstages={stages}\n"
                f"fighters={fighters}\nfrozen=1\nhide=0\n")


# --scenario details: the host's lobby (2 seats, Low tiers, Final Dest. only).
FINAL_DEST = 1 << 25  # kStages' bit


def details_first(g, shots):
    """The first guest opens the lobby's Details off the list: it names the
    host's lists and the host as P1. A there joins."""
    line = open_details(g)
    want = f"fighters={FIGHTERS_LOW:08x} stages={FINAL_DEST:08x}"
    print(f"net_test_xn_menu: {g.name}'s {line}", flush=True)
    if line is None or want not in line or " 1/2 " not in line or "P1=RHYTHM#" not in line or "(host)" not in line:
        print(f"[{g.name}] FAIL: Details doesn't show the host's lobby as set ({want}, P1 the host)")
        return False
    time.sleep(1.0)
    shot(g, shots, f"5b-details-{g.name}")
    nt.press((g,), "X", 7, 0.4)
    return True


def details_full(g, shots):
    """The second guest turns Show full on and opens the full lobby's Details:
    A there joins nothing, and B goes back to the same row."""
    nt.press((g,), "Return", 9, 1.5)  # START: Show full
    line = open_details(g, timeout=30)
    print(f"net_test_xn_menu: {g.name}'s {line}", flush=True)
    if line is None or " 2/2 " not in line:
        print(f"[{g.name}] FAIL: Show full never offered the full lobby's Details")
        return False
    time.sleep(1.0)
    shot(g, shots, f"5c-details-full-{g.name}")
    joins = nt.count(g, r"xeleenet pages: joining")
    nt.press((g,), "X", 7, 2.0)
    if nt.count(g, r"xeleenet pages: joining") != joins or "xeleenet pages: Details -> Joining" in g.text():
        print(f"[{g.name}] FAIL: A on a full lobby's Details joined")
        return False
    nt.press((g,), "Z", 7, 1.5)
    if "xeleenet pages: Details -> Lobbies at lb0" not in g.text():
        print(f"[{g.name}] FAIL: B on Details didn't go back to the lobby's row")
        return False
    shot(g, shots, f"5d-back-on-list-{g.name}")
    print("net_test_xn_menu: the full lobby's Details had no Join; B went back to its row", flush=True)
    return True


def fighters_check(insts, shots):
    """The fighter list (docs/xeleenet-fighter-list-plan.md): RULES carried the
    host's Low tiers to everyone, the sweeps' A on off-list portraits were
    refused on every player, and every Ready was on a fighter on the list."""
    ok = True
    for i in insts:
        t = i.text()
        if f"fighters={FIGHTERS_LOW:07x}" not in t:
            print(f"[{i.name}] FAIL: RULES never put Low tiers in force")
            ok = False
        if not nt.re.search(r"css: P\d A on off-list \d+", t):
            print(f"[{i.name}] FAIL: no A on an off-list portrait was refused")
            ok = False
        for port, ck in nt.re.findall(r"css: P(\d) ready \(fighter (\d+)\)", t):
            if not (FIGHTERS_LOW >> int(ck)) & 1:
                print(f"[{i.name}] FAIL: P{port} readied on fighter {ck}, off the list")
                ok = False
    if ok:
        print("net_test_xn_menu: the fighter list held: off-list A refused, every pick on the list", flush=True)
    return ok


def wait_more(insts, pattern, since, timeout):
    """Every instance has logged `pattern` more times than `since` (a count per
    instance): a scene's next entry, when an earlier one is in the log."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if all(nt.count(i, pattern) > n for i, n in zip(insts, since)):
            return True
        if any(i.proc.poll() is not None for i in insts):
            return False
        time.sleep(0.5)
    return False


def rematch(insts, shots, drive):
    """--scenario set (the old LAN's --scenes, on XeleeNet's flow): the match
    runs out to the results, START there takes everyone back to the CSS, and a
    second pick, stage and match follow, agreed everywhere."""
    vs = [nt.count(i, r"online: enter VS at frame") for i in insts]  # the first match's
    if not all(i.wait_log(r"online: enter RESULTS", 150) for i in insts):
        drive.done()
        print("FAIL: the match never reached the results")
        return False
    drive.done()
    time.sleep(3)
    shot(insts[0], shots, "13-results")
    css = [nt.count(i, r"online: enter CSS") for i in insts]
    for _ in range(12):
        if wait_more(insts, r"online: enter CSS", css, 5):
            break
        nt.press(insts, "Return", 9, 0)  # START leaves the results
    else:
        print("FAIL: the results never went back to the CSS")
        return False
    time.sleep(3)
    shot(insts[0], shots, "14-css-again")
    if not drive_css(insts, want=2):
        print("FAIL: no second CSS pick reached the SSS")
        return False
    cells = [nt.count(i, r"sss: resolved cell") for i in insts]
    if not nt.press_until(insts, "Return", 9, r"sss: resolved cell", want=min(cells) + 1, tries=8, each=6.0):
        print("FAIL: the second SSS never resolved a stage everywhere")
        return False
    picked = {i.name: nt.re.findall(r"sss: resolved cell (\d+)", i.text())[-1:] for i in insts}
    print(f"net_test_xn_menu: second stages {picked}", flush=True)
    if len({tuple(v) for v in picked.values()}) != 1:
        print("FAIL: the players resolved different stages for the rematch")
        return False
    again = __import__("net_test_n").Drive(insts)
    if not wait_more(insts, r"online: enter VS at frame", vs, 180):
        again.done()
        print("FAIL: the rematch never loaded")
        return False
    time.sleep(15)
    shot(insts[0], shots, "15-rematch")
    again.done()
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
        if args.scenario in ("set", "teams", "fighters") and k == 0:
            set_rules(ident, args.players, teams=args.scenario == "teams",
                      fighters=FIGHTERS_LOW if args.scenario == "fighters" else FIGHTERS_ALL)
        if args.scenario == "details" and k == 0:
            set_rules(ident, 2, fighters=FIGHTERS_LOW, stages=FINAL_DEST)
        env = {
            "MELEE_XN_URL": args.url,
            "MELEE_XN_NAME": NAMES[k],
            "MELEE_XN_IDENTITY": ident,
            "MELEE_NET_EXIT_AFTER_FRAMES": str(int(args.seconds * 60) + 30000),
        }
        if args.block:
            env["MELEE_NET_SIM_BLOCK"] = args.block
        if args.no_discovery:
            env["MELEE_NET_SIM_NO_MCAST"] = "1"
        if args.scenario == "sss-repick" and k == 1:
            env["MELEE_NET_SIM_DELAY_MS"] = "80"  # P2's last change races P3's pick
        if args.scenario.startswith("multipad") and k == 0:
            fifo = os.path.join(args.work, f"{name}.pad2.keys")
            os.mkfifo(fifo)
            env["MELEE_KEY_FIFO_P2"] = fifo  # the host's second pad
        insts.append(nt.Instance(name, args.exe, args.disc, args.work, ports[k], 0, env, True))
    everyone = list(insts)  # killed at the end, whoever plays on
    ok = False
    try:
        for inst in insts:
            if not nt.to_main_menu((inst,)):
                print(f"[{inst.name}] FAIL: never reached the main menu")
                return False
        host, guests = insts[0], insts[1:]
        lan = args.net == "lan"
        to_menu = TO_ONLINE + (TO_LAN_ROW if lan else [])
        walk(host, to_menu)
        shot(host, shots, "1-online-mode")
        walk(host, [("X", 7)])
        shot(host, shots, "2-xeleenet-menu")
        walk(host, [("X", 7)])
        time.sleep(1.5)
        if (args.players > 2 or args.scenario == "multipad") and args.scenario not in ("set", "teams", "fighters",
                                                                                         "details"):
            # (their rules file has the seats)
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
        if args.scenario == "leave-host-alone":
            ok = leave_host_alone(host, shots)
            return ok
        for k, g in enumerate(guests):
            # Join opens the lobby browser (B1), which lists the host's lobby.
            walk(g, to_menu + [("X", 7), ("Down", 7), ("X", 7)])
            time.sleep(2.5)
            shot(g, shots, f"5-lobbies-{g.name}")
            if args.no_discovery and not g.wait_log(r"xeleenet pages: LAN discovery blocked", 20):
                print(f"[{g.name}] FAIL: the list never said discovery is blocked")
                return False
            if args.no_discovery:
                shot(g, shots, f"5-blocked-{g.name}")
            if args.scenario == "details" and k > 0:
                # The lobby is full: the others only look at it.
                if not details_full(g, shots):
                    return False
                continue
            if args.scenario == "details":
                if not details_first(g, shots):
                    return False
            elif (k == 0 or args.no_discovery) and lan:
                # The first guest joins by address: X opens LAN's keypad.
                nt.press((g,), "C", 7, 1.0)
                shot(g, shots, f"6-join-address-{g.name}")
                type_code(g, "127.0.0.1", address_keypad())
                shot(g, shots, f"7-join-address-typed-{g.name}")
                nt.press((g,), "Return", 9, 0.4)
            elif k == 0:
                # The first guest types the code: Y opens the keypad (J1).
                nt.press((g,), "V", 7, 1.0)
                shot(g, shots, "6-join-keypad")
                type_code(g, code)
                shot(g, shots, "7-join-code-typed")
                nt.press((g,), "Return", 9, 0.4)
            else:
                # The others pick the lobby off the list: A for its Details,
                # A again to join.
                if not join_off_list(g):
                    return False
            time.sleep(0.6)
            shot(g, shots, f"8-joining-{g.name}")
            if args.join_shots and k == 0:
                # What the joiner sees until the CSS (a Quest find: the list
                # came back for a few seconds).
                for n in range(16):
                    time.sleep(0.35)
                    shot(g, shots, f"8-joining-{g.name}-{n:02d}")
            # Seated: this guest's own session agreed and its CSS entered.
            if not g.wait_log(r"lobby: entering CSS", 90):
                print(f"[{g.name}] FAIL: never got into the lobby")
                return False
        if args.scenario == "details":
            insts = insts[:2]  # the 2-seat lobby's players
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
        # The doors' layout feeds the hand's tests: every player must have it.
        seats = [nt.re.findall(r"css: (\d) seats", i.text())[-1:] for i in insts]
        print(f"net_test_xn_menu: CSS seats {seats}", flush=True)
        if len({tuple(v) for v in seats}) != 1 or not seats[0]:
            print("FAIL: the players laid the doors out for different seats")
            return False
        if args.scenario == "ready":
            if not ready_scenario(insts, shots):
                return False
        if args.scenario == "leave-guest":
            if not leave_guest(insts, shots):
                return False
        if args.scenario == "leave-host":
            ok = leave_host(insts, shots)
            return ok
        if args.scenario == "css-b-hold" and not css_b_hold(insts, shots):
            return False
        pad = PadHand(host, 2) if args.scenario.startswith("multipad") else None
        if args.scenario == "multipad-full":
            ok = multipad_full(insts, pad, shots)
            return ok
        if args.scenario == "multipad":
            if not multipad_scenario(insts, pad, shots):
                return False
            insts = insts + [pad]  # its hand picks, readies and plays from here on
        if args.scenario == "teams" and not teams_scenario(insts, shots):
            return False
        if args.scenario != "ready" and not drive_css(insts):
            print("FAIL: no CSS pick reached the SSS")
            return False
        if args.scenario == "fighters" and not fighters_check(insts, shots):
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
        if args.scenario == "set":
            if not rematch(insts, shots, drive):
                return False
        else:
            time.sleep(args.seconds)
            drive.done()
        shot(host, shots, "12-match-later")
        texts = [i.text() for i in insts]
        ok = not any(nt.re.search(r"net: DESYNC", t) for t in texts)
        # The old netplay's quick chat (its D-pad prompt) has no place in a
        # XeleeNet lobby (a Quest find: XR's placement step left it on).
        if lan and not all("xeleenet: network LAN" in t for t in texts):
            print("FAIL: an instance never switched to LAN")
            ok = False
        if any("net: quick chat on" in t for t in texts):
            print("FAIL: quick chat came on in a XeleeNet lobby")
            ok = False
        for i, t in zip(insts, texts):
            print(f"[{i.name}] " + "; ".join(nt.re.findall(r"xeleenet: (P\d (?:direct|through the host))", t))
                  + ("  DESYNC" if "net: DESYNC" in t else ""))
    finally:
        for inst in everyone:
            inst.kill(5)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--players", type=int, choices=(2, 3, 4), default=3)
    ap.add_argument("--seconds", type=float, default=40, help="of match after it loads")
    ap.add_argument("--block", default="")
    ap.add_argument("--scenario", default="",
                    choices=("", "sss-host-b", "ready", "leave-guest", "leave-host", "sss-repick", "sss-timeout",
                             "sss-same", "css-b-hold", "leave-host-alone", "set", "teams", "multipad",
                             "multipad-full", "fighters", "details"),
                    help="sss-host-b: guest B and off-list picks refused, host B back to the CSS; "
                         "ready: Ready on the CSS (one START, the countdown, B stops it); "
                         "leave-guest: the last guest holds B and leaves alone, then joins again; "
                         "leave-host: the host holds B and closes the lobby; "
                         "sss-repick: picks change, one racing the last pick; "
                         "sss-timeout: P3 never picks, the 15 s timer ends it; "
                         "sss-same: everyone picks Battlefield, no draw; "
                         "css-b-hold: B held over the grid leaves the token and the pick alone; "
                         "leave-host-alone: the host alone holds B and leaves its lobby; "
                         "set: 1-minute matches, the results, back to the CSS and a rematch; "
                         "teams: Teams with a seat per player (doors moved unless 4), the host turns "
                         "its team on its door's button; "
                         "multipad: the host's second pad takes a seat, gives it up, takes it again, and "
                         "plays (use --players 2); multipad-full: a 2-seat lobby refuses it; "
                         "fighters: the host's lobby allows Low tiers only; off-list A is refused everywhere; "
                         "details: a 2-seat lobby (Low tiers, Final Dest. only): the first guest's Details shows "
                         "them and A joins; the next turns Show full on, and the full lobby's Details won't join "
                         "(use --players 3)")
    ap.add_argument("--join-shots", action="store_true", help="shots every 0.35 s through the first join")
    ap.add_argument("--url", default="", help="a running service instead of a local lobby service")
    ap.add_argument("--net", choices=("service", "lan"), default="service",
                    help="lan: Online Mode > LAN, no service")
    ap.add_argument("--no-discovery", action="store_true",
                    help="LAN on a network that drops multicast: every guest joins by address")
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "build", "linux", "melee"))
    ap.add_argument("--disc", default=os.path.expanduser(
        "~/Downloads/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso"))
    ap.add_argument("--port", type=int, default=42250)
    ap.add_argument("--work", default=os.path.join(HERE, "..", "build", "net_test_xn_menu"))
    ap.add_argument("--shots", default="")
    args = ap.parse_args()
    if args.no_discovery and args.net != "lan":
        ap.error("--no-discovery is a LAN run (--net lan)")
    service = None
    if not args.url and args.net == "service":
        proc, args.url, state = test_service.start_service(short=False)
        service = (proc, state)
    try:
        ok = run(args)
    finally:
        if service:
            test_service.stop_service(*service)
    print("net_test_xn_menu: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
