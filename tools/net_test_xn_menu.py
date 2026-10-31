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


# How often a wait checks the logs for its event (Instance.text() reads on
# from where it stopped, so a short poll costs little).
POLL = 0.05
SHOTS = True  # --no-shots: regression runs skip the pictures, and look()'s waits for them


def look(seconds):
    """A wait that only lets the screen settle for the next shot: none
    without shots."""
    if SHOTS:
        nap(seconds)


def shot(inst, shots, name):
    if not SHOTS:
        return
    out = os.path.join(shots, f"{name}.png")
    env = dict(os.environ, MELEE_WINDOW_TITLE=nt.window_title(inst.name))
    r = subprocess.run([sys.executable, os.path.join(HERE, "devctl.py"), "shot", out], env=env,
                       capture_output=True, text=True)
    print(f"net_test_xn_menu: shot {name}" + ("" if r.returncode == 0 else f" FAILED {r.stderr}"),
          flush=True)


# Every instance of the run, for nap() (set in run()).
RUNNING = []


def nap(seconds, insts=None):
    """Let `seconds` of game time pass: paced, of wall clock; unpaced, as
    frames on every instance still running (docs/fast-tests-plan.md T4)."""
    if not nt.UNPACED:
        time.sleep(seconds)
        return
    live = [i for i in (insts or RUNNING) if i.proc.poll() is None]
    if live:
        nt.step(live, [f"wait {max(1, int(seconds * 60))}f"])


def walk(insts, keys):
    """Menu hops. Unpaced: each press waits for the menu's own event (its
    highlight moved, or the next menu or page took input: MELEE_TEST_EVENTS),
    armed before the press so one on its first frame isn't missed; a press
    that changes scene instead gives up after 120 frames."""
    insts = insts if isinstance(insts, (list, tuple)) else (insts,)
    for key, frames in keys:
        if nt.UNPACED:
            nt.step(insts, [f"tapuntil {key} {frames}f 121f menu: 120f"])
            continue
        nt.press(insts, key, frames, 0)
        nt.settle(insts, nt.MENU_SETTLE)


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
    keys = []
    for ch in code:
        want = next(i for i, it in enumerate(items) if it[0] == ch)
        for _ in range(12):
            if cur == want:
                break
            r, c = items[cur][1], items[cur][2]
            wr, wc = items[want][1], items[want][2]
            key = "Down" if wr > r else "Up" if wr < r else "Right" if wc > c else "Left"
            keys.append(key)
            cur = step(items, cur, key)
        keys.append("X")
    if nt.UNPACED:
        # The path is worked out here, so the whole code goes in one batch
        # rather than a round trip a press.
        nt.step((inst,), [line for key in keys for line in (f"{key} 7f", f"wait {int(STEP * 60)}f")])
        return
    for key in keys:
        nt.press((inst,), key, 7, STEP)


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
            nap(0.3)
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
    # From the second row: on XeleeNet's CSS, drawn smaller about the centre
    # (PC_XN_CSS_ZOOM), the first row's Down lands short of the portraits and
    # its whole sweep picked nothing, every run.
    for row, down in enumerate(nt.CSS_ROWS[1:], start=1):
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
        time.sleep(POLL)
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
    nap(1.5)
    shot(host, shots, "mp-1-css-spare")
    seated = "(players 07"
    for attempt in range(2):
        before = [i.text().count(seated) for i in insts]
        loads = [css_loads(i) for i in insts]
        nt.hold(pad, "Return", 9)
        for i, n in zip(insts, before):
            deadline = time.time() + 40
            while time.time() < deadline and i.text().count(seated) <= n:
                time.sleep(POLL)
            if i.text().count(seated) <= n:
                print(f"[{i.name}] FAIL: no session with the second pad's seat {seated})")
                return False
        if not host.wait_log(r"net: this machine plays ports 5 \(P1<-pad 1 P3<-pad 2\)", 30):
            print("FAIL: the host doesn't play P3 from its second pad")
            return False
        if not reloaded(insts, loads):
            return False
        nap(1.5)
        shot(host, shots, f"mp-{2 + 3 * attempt}-css-added-host")
        shot(insts[1], shots, f"mp-{2 + 3 * attempt}-css-added-guest")
        print(f"net_test_xn_menu: the second pad plays P3 ({'again' if attempt else 'first'})", flush=True)
        if attempt:
            break
        # The second pad gives its seat up: a hold of B on it. The host stays.
        menus = [menu_loads(i) for i in insts]
        before = [i.text().count("(players 03") for i in insts]
        loads = [css_loads(i) for i in insts]
        nt.hold(pad, "Z", 90)
        look(40 / 60)
        shot(host, shots, "mp-3-css-giving-up")
        for i, n in zip(insts, before):
            deadline = time.time() + 40
            while time.time() < deadline and i.text().count("(players 03") <= n:
                time.sleep(POLL)
            if i.text().count("(players 03") <= n:
                print(f"[{i.name}] FAIL: the seat was never given up")
                return False
        if any(menu_loads(i) > n for i, n in zip(insts, menus)):
            print("FAIL: giving up the second pad's seat took someone out of the lobby")
            return False
        if not reloaded(insts, loads):
            return False
        nap(1.5)
        shot(host, shots, "mp-4-css-given-up")
        print("net_test_xn_menu: the second pad gave its seat up; the host stayed", flush=True)
    return True


def reloaded(insts, loads, timeout=60):
    """Every CSS has loaded again since `loads` (css_loads per instance)."""
    for i, n in zip(insts, loads):
        deadline = time.time() + timeout
        while time.time() < deadline and css_loads(i) <= n:
            time.sleep(POLL)
        if css_loads(i) <= n:
            print(f"[{i.name}] FAIL: the CSS never reloaded")
            return False
    return True


def multipad_full(insts, pad, shots):
    """A spare pad's START with no seat open: a notice on that machine only,
    and no session change."""
    host = insts[0]
    nt.settle(insts, nt.MENU_SETTLE)
    nt.hold(pad, "Return", 9)
    if not host.wait_log(r"css: controller 2 asks for a seat; the lobby is full", 15):
        print("FAIL: the full lobby's START wasn't refused")
        return False
    nap(0.8)
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
        time.sleep(POLL)
    if not all(css_loads(i) > n for i, n in zip(insts, before)):
        print("FAIL: the host's B didn't take everyone back to the CSS")
        return False
    print("net_test_xn_menu: the host's B took everyone back to the CSS", flush=True)
    nap(3)
    if not drive_css(insts):
        print("FAIL: no CSS pick reached the SSS the second time")
        return False
    nap(2)
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
    if nt.UNPACED and over(inst) != cell:
        # The game taps until it logs the cell, and stops on that frame: the
        # stage select's pick timer runs in frames, faster than a round trip
        # a tap. The gap outlasts the input delay (8 frames at sss-repick's
        # 80 ms): a tap still in flight when the cell is logged carries the
        # cursor past it.
        nt.step((inst,), [f"tapuntil {key} 3f 20f sss: over cell {cell} {tries * 24}f"])
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
    nap(1.0)
    if scenario == "sss-same":
        for i in insts:
            if not steer(i, "Up", BF):
                return False
            nt.press((i,), "X", 7, 0.3)
        look(0.6)
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
        nt.hold(b, "X", 7)
        nt.hold(c, "Return", 7)
        look(0.9)
        shot(host, shots, "10e-sss-draw")
        look(1.1)
        shot(host, shots, "10f-sss-landed")
    elif scenario == "sss-timeout":
        if c is None:
            print("FAIL: sss-timeout wants three players")
            return False
        for i in (host, b):
            if not steer(i, "Up", BF):
                return False
        nt.press((host,), "X", 7, 0.2)
        # A frame of the upper grid's shape: along to Dream Land, then up.
        if not all(steer(b, key, cell) for key, cell in (("Right", FD), ("Right", DL), ("Up", PS))):
            return False
        nt.press((b,), "X", 7, 0.5)
        look(4)
        shot(c, shots, "10c-sss-card-timer")
        if not host.wait_log(r"sss: final: ", 30):
            print("FAIL: the pick timer never ended the picking")
            return False
        # In frames, the way the timer counts: unpaced, its 15 s go by in 2.
        timer = nt.re.search(r"sss: the pick timer runs from frame (\d+) to (\d+)", host.text())
        draw = nt.re.search(r"sss: final: .* the draw at frame (\d+)", host.text())
        took = (int(draw.group(1)) - int(timer.group(1))) / 60 if timer and draw else -1
        print(f"net_test_xn_menu: the timer ended the picking {took:.2f} s after the first pick", flush=True)
        if not 15 <= took <= 16:
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
    nt.hold(leaver, "Z", 90)
    look(40 / 60)
    shot(leaver, shots, "9c-css-leaving")
    nap(4.0)
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
    nap(3)
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
            time.sleep(POLL)
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
    nap(4)
    shot(host, shots, "9f-css-rejoined")
    return True


def leave_host(insts, shots):
    """The host holding B closes the lobby (P4): every player lands on the
    XeleeNet pages, the guests on the lobby list with a note."""
    host, guests = insts[0], insts[1:]
    nt.settle(insts, nt.MENU_SETTLE)
    nt.hold(host, "Z", 90)
    nap(4.0)
    if not all("css: P1 left the lobby" in i.text() for i in insts):
        print("FAIL: not everyone saw the host's hold end")
        return False
    if not all("online: XeleeNet the host closed the lobby" in g.text() for g in guests):
        print("FAIL: a guest didn't take it as the lobby closing")
        return False
    if any("DESYNC" in i.text() or "peer left" in i.text() for i in insts):
        print("FAIL: the close went through as a failure")
        return False
    look(2)
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
    nt.hold(holder, "Z", 40)  # 40 frames: short of the 60 that leave
    nap(40 / 60 + 1.0)
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
    nap(1.5)
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
    nt.hold(host, "Z", 90)
    look(40 / 60)
    shot(host, shots, "4b-host-alone-leaving")
    deadline = time.time() + 15
    while time.time() < deadline and menu_loads(host) == menus:
        time.sleep(POLL)
    if "css: P1 left the lobby" not in host.text():
        print("FAIL: the host's hold never ended")
        return False
    if menu_loads(host) == menus:
        print("FAIL: the host's hold ended, but it never left the lobby")
        return False
    look(2)
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
    nap(1.0)
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
    nap(1.0)
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
        time.sleep(0.05)  # unpaced, half a second is a hundred frames
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
    look(3)
    shot(insts[0], shots, "13-results")
    css = [nt.count(i, r"online: enter CSS") for i in insts]
    for _ in range(12):
        if wait_more(insts, r"online: enter CSS", css, 5):
            break
        nt.press(insts, "Return", 9, 0)  # START leaves the results
    else:
        print("FAIL: the results never went back to the CSS")
        return False
    nap(3)
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
    nap(15)
    shot(insts[0], shots, "15-rematch")
    again.done()
    return True


# ---- Lobby Settings (docs/xeleenet-settings-overlay-plan.md) ------------------
# The overlay's items, top to bottom, as Up and Down walk them on the host's
# page (xeleenet_menu.cpp build_overlay_host): Friendly fire only with Teams
# on; Visibility and Hide code not on LAN.
def settings_rows(teams, lan):
    rows = ["teams", "stocks", "time", "items"] + (["ff"] if teams else []) + ["more", "flist", "slist", "frozen",
                                                                             "seats"]
    return rows + ([] if lan else ["vis", "hide"]) + ["apply", "cancel"]


def settings_file(work):
    return os.path.join(work, "apply.txt")


def commits(inst):
    """Each "RULES in force" line: (frame, hash)."""
    return nt.re.findall(r"net: RULES in force at frame (\d+) hash ([0-9a-f]+)", inst.text())


def wait_commit(insts, before, timeout=60):
    """Every instance has committed RULES more often than `before` (counts)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if all(len(commits(i)) > n for i, n in zip(insts, before)):
            return True
        time.sleep(POLL)
    for i, n in zip(insts, before):
        if len(commits(i)) <= n:
            print(f"[{i.name}] FAIL: no new RULES commit")
    return False


def same_commit(insts):
    last = {i.name: commits(i)[-1:] for i in insts}
    print(f"net_test_xn_menu: last commits {last}", flush=True)
    if len({tuple(v) for v in last.values()}) != 1 or not last[insts[0].name]:
        print("FAIL: the players committed different RULES, or on different frames")
        return False
    return True


def tap(inst, key, times=1):
    for _ in range(times):
        nt.press((inst,), key, 7, 0.35)


def open_settings(inst):
    """Z (Tab) opens Lobby Settings on that player's own game, and returns
    once the page takes input. Its load is wall time: unpaced, a settle in
    frames is over before it is."""
    n = nt.count(inst, r"xeleenet: Lobby Settings open")
    r = nt.count(inst, r"menu: page settings ready")
    nt.press((inst,), "Tab", 7, 0.1 if nt.UNPACED else 0.9)
    if nt.count(inst, r"xeleenet: Lobby Settings open") <= n:
        print(f"[{inst.name}] FAIL: Z didn't open Lobby Settings")
        return False
    if not wait_more((inst,), r"menu: page settings ready", (r,), 10):
        print(f"[{inst.name}] FAIL: Lobby Settings never came up")
        return False
    return True


def to_row(inst, rows, row):
    """Down from the first row (where the overlay opens) to `row`."""
    tap(inst, "Down", rows.index(row))


def portrait(inst, ckind):
    """A portrait's middle, as the game logs them (MELEE_XN_TEST_HAND)."""
    m = nt.re.search(r"css: portraits(.*)", inst.text())
    for k, x, y in nt.re.findall(r" (\d+):(-?[\d.]+),(-?[\d.]+)", m.group(1) if m else ""):
        if int(k) == ckind:
            return float(x), float(y)
    return None


def token(inst, port):
    """Where port's token is (it picks by that, not by the hand)."""
    m = nt.re.findall(rf"css: P{port} hand at -?[\d.]+ -?[\d.]+ token (-?[\d.]+) (-?[\d.]+)", inst.text())
    return (float(m[-1][0]), float(m[-1][1])) if m else None


def hand_at(inst, port):
    m = nt.re.findall(rf"css: P{port} hand at (-?[\d.]+) (-?[\d.]+)", inst.text())
    return (float(m[-1][0]), float(m[-1][1])) if m else None


def steer_to(inst, port, target, where=token, tries=10):
    """Move port's hand until `where` (its token, or the hand itself) is
    within a unit of `target`, by what the game logs (MELEE_XN_TEST_HAND)."""
    for _ in range(tries):
        nap(0.7)
        at = where(inst, port)
        if at is None:
            continue
        dx, dy = target[0] - at[0], target[1] - at[1]
        if abs(dx) < 1.0 and abs(dy) < 1.0:
            return True
        for d, plus, minus in ((dx, "Right", "Left"), (dy, "Up", "Down")):
            frames = round(abs(d) / HAND_SPEED)
            if frames > 0:
                nt.press((inst,), plus if d > 0 else minus, frames, 0.2)
    return False


BANNER_AT = (0.0, 23.5)  # the top strip, under XeleeNet's banner


def pick_high(inst, port, ckind=2):
    """Port's hand puts its token on `ckind` (Fox: High tiers, off the Low
    tiers list), steered by where the game says the token is. Returns the
    fighter picked if it's a High tier, else None."""
    target = None
    for _ in range(20):
        target = target or portrait(inst, ckind)
        if target is not None:
            break
        nap(0.5)
    if target is None:
        print(f"[{inst.name}] FAIL: the game never logged the portraits")
        return None
    steer_to(inst, port, target)
    before = nt.count(inst, rf"css: P{port} picked")
    nt.press((inst,), "X", 7, 0.6)
    picks = nt.re.findall(rf"css: P{port} picked (\d+)", inst.text())
    if len(picks) <= before:
        print(f"[{inst.name}] FAIL: P{port} never picked (token at {token(inst, port)}, portrait {target})")
        return None
    got = int(picks[-1])
    print(f"net_test_xn_menu: P{port} picked {got}", flush=True)
    if not (FIGHTERS_HIGH >> got) & 1:
        print(f"[{inst.name}] FAIL: P{port} picked {got}, not a High tier")
        return None
    return got


def pick_rows(insts):
    """Everyone sweeps two rows: a pick each, as ready_scenario does."""
    for down in nt.CSS_ROWS[:2]:
        nt.press(insts, "Left+Up", nt.CSS_PIN, 1.2)
        nt.press(insts, "Down", down, 0.8)
        nt.press(insts, "Right", nt.CSS_FIRST, 0.6)
        for _ in range(nt.CSS_COLS):
            nt.press(insts, "X", 7, 0.5)
            nt.press(insts, "Right", nt.CSS_STEP, 0.4)


def applied_everywhere(insts, loads, before, shots, name):
    """An Apply landed: every CSS reloaded on one commit, the same everywhere."""
    if not wait_commit(insts, before) or not reloaded(insts, loads) or not same_commit(insts):
        return False
    nap(1.5)
    shot(insts[0], shots, f"{name}-host")
    if len(insts) > 1:
        shot(insts[1], shots, f"{name}-guest")
    return True




def settings_engine(insts, shots, args):
    """O2 with no UI (MELEE_XN_TEST_APPLY): P2 holds Fox; the host's game
    applies Low tiers and 3 stocks. Every player commits the same RULES on
    the same frame, and P2's Fox comes back empty everywhere."""
    host, p2 = insts[0], insts[1]
    nt.settle(insts, nt.MENU_SETTLE)
    if pick_high(p2, 2) is None:
        return False
    nap(1.0)
    loads, before = [css_loads(i) for i in insts], [len(commits(i)) for i in insts]
    with open(settings_file(args.work), "w") as f:
        f.write(f"stock=1\nstocks=3\nfighters={FIGHTERS_LOW}\n")
    if not host.wait_log(r"xeleenet: settings applied", 20):
        print("FAIL: the test knob never applied")
        return False
    if not applied_everywhere(insts, loads, before, shots, "s-engine"):
        return False
    return settings_landed(insts, stocks=3, fighters=FIGHTERS_LOW, dropped=2)


def settings_landed(insts, stocks=None, fighters=None, dropped=None):
    """The last RULES each player put in force has these, and P`dropped`'s
    pick came back empty everywhere."""
    ok = True
    for i in insts:
        t = i.text()
        applied = nt.re.findall(r"net: RULES (?:applied|in force) mode=(.*)", t)
        last = " " + applied[-1] if applied else ""
        if stocks is not None and f" stock={stocks} " not in last:
            print(f"[{i.name}] FAIL: stocks={stocks} never came in force")
            ok = False
        if fighters is not None and f"fighters={fighters:07x}" not in last:
            print(f"[{i.name}] FAIL: the fighter list {fighters:07x} never came in force")
            ok = False
        if dropped is not None and not nt.re.search(rf"online: XeleeNet P{dropped}'s pick \d+ is off the list", t):
            print(f"[{i.name}] FAIL: P{dropped}'s pick wasn't dropped")
            ok = False
    if ok:
        print("net_test_xn_menu: the new settings landed on every player", flush=True)
    return ok


def settings_hold(insts, shots, args):
    """O3: with the overlay open, the host's stick, A and START reach no
    player's CSS (its port rests); after B closes it, its A does again."""
    host = insts[0]
    nt.settle(insts, nt.MENU_SETTLE)
    if not open_settings(host):
        return False
    a_at = [nt.count(i, r"css: P1 A at") for i in insts]
    ready = [nt.count(i, r"css: P1 ready|css: P1 START") for i in insts]
    # Down to More rules, A into it and B out: nothing changes, and none of
    # it may reach the CSS. Then START (nothing to apply) closes it.
    rows = settings_rows(teams=True, lan=args.net == "lan")
    to_row(host, rows, "more")
    shot(host, shots, "s-hold-open")
    tap(host, "X")
    tap(host, "Z")
    tap(host, "Return")
    nap(1.0)
    ok = True
    if [nt.count(i, r"css: P1 A at") for i in insts] != a_at:
        print("FAIL: the host's A reached the CSS with the overlay open")
        ok = False
    if [nt.count(i, r"css: P1 ready|css: P1 START") for i in insts] != ready:
        print("FAIL: the host's START reached the CSS with the overlay open")
        ok = False
    if not host.wait_log(r"net: P1 rests", 1):
        print("FAIL: the host's port never rested")
        ok = False
    # START on a page applies: here it went to the overlay (nothing changed).
    if not host.wait_log(r"xeleenet: Lobby Settings closed \(nothing changed\)", 5):
        print("FAIL: START didn't close the unchanged overlay")
        ok = False
    nap(0.5)
    nt.press((host,), "X", 7, 1.0)
    if not all(nt.count(i, r"css: P1 A at") > n for i, n in zip(insts, a_at)):
        print("FAIL: after the overlay, the host's A didn't reach the CSS")
        ok = False
    if ok:
        print("net_test_xn_menu: the host rested while the overlay was open", flush=True)
    return ok


def settings_open(insts, shots, args):
    """O5: the host's hand on the banner and A opens Lobby Settings, with
    Melee's rules bar under it shut (the CSS stays); the guests hear it
    (editing); B closes it. A guest's Z opens a read-only one. A Ready host's
    Z opens nothing."""
    host, guest = insts[0], insts[1]
    nt.settle(insts, nt.MENU_SETTLE)
    loads = [css_loads(i) for i in insts]
    # The hand onto the banner's middle, where Melee's rules bar opened its
    # rules menu.
    steer_to(host, 1, BANNER_AT, where=hand_at)
    shot(host, shots, "s-open-hot")
    nt.press((host,), "X", 7, 1.0)
    ok = True
    if not host.wait_log(r"css: P1 opens Lobby Settings \(the banner\)", 3) or \
            not host.wait_log(r"xeleenet: Lobby Settings open \(host\)", 3):
        print("FAIL: A on the banner didn't open Lobby Settings")
        return False
    if not all(g.wait_log(r"xeleenet: the host opened Lobby Settings", 10) for g in insts[1:]):
        print("FAIL: a guest never heard the host was editing")
        ok = False
    nap(1.0)
    shot(host, shots, "s-open-host")
    shot(guest, shots, "s-open-guest-c3")
    tap(host, "Z")  # B: cancel
    if not host.wait_log(r"xeleenet: Lobby Settings closed", 5):
        print("FAIL: B didn't close Lobby Settings")
        ok = False
    if not all(g.wait_log(r"xeleenet: the host closed Lobby Settings", 10) for g in insts[1:]):
        print("FAIL: a guest never heard the host was done")
        ok = False
    nap(1.0)
    if [css_loads(i) for i in insts] != loads or any("online: enter SSS" in i.text() for i in insts):
        print("FAIL: A over the rules bar left the CSS")
        ok = False
    if not open_settings(guest) or not guest.wait_log(r"xeleenet: Lobby Settings open \(read-only\)", 2):
        print("FAIL: the guest's Z didn't open a read-only Lobby Settings")
        return False
    nap(0.8)
    shot(guest, shots, "s-open-guest-read-only")
    tap(guest, "Z")
    # A Ready host: Z opens nothing.
    pick_rows((host,))
    nt.press((host,), "Return", 9, 1.0)
    if not host.wait_log(r"css: P1 ready", 5):
        print("FAIL: the host never got Ready")
        return False
    n = nt.count(host, r"xeleenet: Lobby Settings open")
    nt.press((host,), "Tab", 7, 1.0)
    if nt.count(host, r"xeleenet: Lobby Settings open") != n:
        print("FAIL: a Ready host's Z opened Lobby Settings")
        ok = False
    tap(host, "Z")  # not Ready again
    if ok:
        print("net_test_xn_menu: Lobby Settings opened from the banner and with Z, read-only for a guest",
              flush=True)
    return ok


def settings_walk(insts, shots, args):
    """The full UI walk: P2 holds Fox; the host opens Lobby Settings with Z,
    Stocks right, Fighter list > X twice (Low tiers), START. Every player
    commits on the same frame, P2's Fox comes back empty and Ready is 0."""
    host, p2 = insts[0], insts[1]
    lan = args.net == "lan"
    nt.settle(insts, nt.MENU_SETTLE)
    if pick_high(p2, 2) is None:
        return False
    nt.press((p2,), "Return", 9, 1.0)  # P2 Ready: the Apply clears it
    if not p2.wait_log(r"css: P2 ready", 5):
        print("FAIL: P2 never got Ready")
        return False
    rows = settings_rows(teams=True, lan=lan)
    if not open_settings(host):
        return False
    to_row(host, rows, "stocks")
    tap(host, "Right")  # 4 -> 5
    shot(host, shots, "s-walk-stocks")
    tap(host, "Down", rows.index("flist") - rows.index("stocks"))
    tap(host, "X")  # A: the fighter page
    tap(host, "C", 2)  # X: All -> High -> Low
    shot(host, shots, "s-walk-fighters")
    tap(host, "Z")  # B: back to the rows
    nap(0.5)
    shot(host, shots, "s-walk-changes")
    loads, before = [css_loads(i) for i in insts], [len(commits(i)) for i in insts]
    ready = [nt.count(i, r"css: P\d ready") for i in insts]
    tap(host, "Return")  # START: apply
    if not host.wait_log(r"xeleenet: Lobby Settings applied", 5):
        print("FAIL: START didn't apply")
        return False
    nap(0.8)
    shot(p2, shots, "s-walk-applying-guest")
    if not applied_everywhere(insts, loads, before, shots, "s-walk-after"):
        return False
    if not settings_landed(insts, stocks=5, fighters=FIGHTERS_LOW, dropped=2):
        return False
    if [nt.count(i, r"css: P\d ready") for i in insts] != ready:
        print("FAIL: someone was Ready after the reload")
        return False
    return True


def settings_cancel(insts, shots, args):
    """B: the host changes Stocks and cancels; no restart, no reload, and
    P2's Ready stands."""
    host, p2 = insts[0], insts[1]
    nt.settle(insts, nt.MENU_SETTLE)
    pick_rows((p2,))
    nt.press((p2,), "Return", 9, 1.0)
    if not p2.wait_log(r"css: P2 ready", 5):
        print("FAIL: P2 never got Ready")
        return False
    loads = [css_loads(i) for i in insts]
    starts = nt.count(host, r"xeleenet: starting")
    if not open_settings(host):
        return False
    tap(host, "Down")
    tap(host, "Right")
    tap(host, "Z")  # B: cancel
    if not host.wait_log(r"xeleenet: Lobby Settings closed", 5):
        print("FAIL: B didn't close Lobby Settings")
        return False
    nap(6)
    ok = True
    if nt.count(host, r"xeleenet: starting") != starts or [css_loads(i) for i in insts] != loads:
        print("FAIL: a cancelled Lobby Settings restarted the session or reloaded the CSS")
        ok = False
    if any(nt.re.search(r"css: P2 not ready", i.text()) for i in insts):
        print("FAIL: P2's Ready went")
        ok = False
    if ok:
        print("net_test_xn_menu: cancel changed nothing; P2 still Ready", flush=True)
    return ok


def join_one(g, to_menu, lan, code):
    """A latecomer joins: by address on LAN, by code on the service."""
    walk(g, to_menu + [("X", 7), ("Down", 7), ("X", 7)])
    nap(2.5)
    if lan:
        nt.press((g,), "C", 7, 1.0)
        type_code(g, "127.0.0.1", address_keypad())
    else:
        nt.press((g,), "V", 7, 1.0)
        type_code(g, code)
    nt.press((g,), "Return", 9, 0.4)


def settings_seats(insts, extra, shots, args, code):
    """Seats: 3 players in 4 seats. Lowering to 3 works and 2 can't be
    reached; then a 4th is refused. Raising to 4 lets the 4th join."""
    host = insts[0]
    lan = args.net == "lan"
    rows = settings_rows(teams=True, lan=lan)
    nt.settle(insts, nt.MENU_SETTLE)
    for want, key, times in ((3, "Left", 2), (4, "Right", 1)):
        if not open_settings(host):
            return False
        to_row(host, rows, "seats")
        tap(host, key, times)
        shot(host, shots, f"s-seats-{want}")
        loads, before = [css_loads(i) for i in insts], [len(commits(i)) for i in insts]
        tap(host, "Return")
        if not applied_everywhere(insts, loads, before, shots, f"s-seats-{want}-after"):
            return False
        if not all(i.wait_log(rf"seats={want} unlock_all", 5) for i in insts) or \
                not all(nt.re.findall(r"css: (\d) seats", i.text())[-1:] == [str(want)] for i in insts):
            print(f"FAIL: the lobby didn't come to {want} seats everywhere")
            return False
        print(f"net_test_xn_menu: {want} seats everywhere", flush=True)
        if want == 3:
            join_one(extra, TO_ONLINE + (TO_LAN_ROW if lan else []), lan, code)
            if not extra.wait_log(r"xeleenet pages: join failed", 30):
                print("FAIL: a 4th player joined a 3-seat lobby")
                return False
            nap(1.0)
            shot(extra, shots, "s-seats-4th-refused")
            print("net_test_xn_menu: the 4th was refused", flush=True)
    nt.press((extra,), "X", 7, 0.5)  # TRY AGAIN
    if not extra.wait_log(r"lobby: entering CSS", 90):
        print("FAIL: the 4th never got in once there were 4 seats")
        return False
    for i in insts + [extra]:
        if not i.wait_log(nt.re.escape("(players 0f)"), 60):
            print(f"[{i.name}] FAIL: no session with the 4th")
            return False
    print("net_test_xn_menu: 4 seats let the 4th in", flush=True)
    nap(4)
    return True


def team_of(inst, port):
    m = nt.re.findall(rf"css: P{port}'s team (\d)", inst.text())
    return int(m[-1]) if m else None


def settings_teams(insts, shots, args):
    """Teams: off -> on deals the teams again on every player; a team the
    host then sets on its door survives an unrelated Apply."""
    host = insts[0]
    lan = args.net == "lan"
    nt.settle(insts, nt.MENU_SETTLE)
    if not open_settings(host):
        return False
    tap(host, "Right")  # Teams: Off -> On
    loads, before = [css_loads(i) for i in insts], [len(commits(i)) for i in insts]
    tap(host, "Return")
    if not applied_everywhere(insts, loads, before, shots, "s-teams-on"):
        return False
    dealt = nt.re.escape(f"teams dealt for {(1 << len(insts)) - 1:02x} (Teams on)")
    if not all(i.wait_log(dealt, 5) for i in insts):
        print("FAIL: Teams on didn't deal the teams everywhere")
        return False
    print("net_test_xn_menu: Teams on dealt the teams", flush=True)
    if not teams_scenario(insts, shots):
        return False
    team = team_of(host, 1)
    rows = settings_rows(teams=True, lan=lan)
    if not open_settings(host):
        return False
    to_row(host, rows, "stocks")
    tap(host, "Right")
    loads, before = [css_loads(i) for i in insts], [len(commits(i)) for i in insts]
    deals = [nt.count(i, r"teams dealt") for i in insts]
    tap(host, "Return")
    if not applied_everywhere(insts, loads, before, shots, "s-teams-kept"):
        return False
    kept = f"teams kept {team}"
    if [nt.count(i, r"teams dealt") for i in insts] != deals or \
            not all(i.wait_log(nt.re.escape(kept), 5) for i in insts):
        print(f"FAIL: the host's team ({team}) didn't survive the Apply")
        return False
    print(f"net_test_xn_menu: P1's team {team} survived an Apply", flush=True)
    return True


def settings_alone(host, shots):
    """The host alone: Apply reloads its own CSS, with no session."""
    nt.settle((host,), nt.MENU_SETTLE)
    if not open_settings(host):
        return False
    tap(host, "Down")
    tap(host, "Right")
    loads = css_loads(host)
    tap(host, "Return")
    if not host.wait_log(r"the host alone reloads its CSS", 5) or not reloaded((host,), [loads]):
        print("FAIL: the host alone's Apply didn't reload its CSS")
        return False
    look(2)
    shot(host, shots, "s-alone-after")
    if not host.wait_log(r"xeleenet: the CSS has new rules", 5):
        print("FAIL: the CSS never showed the new rules")
        return False
    print("net_test_xn_menu: the host alone reloaded with the new settings", flush=True)
    return True


def settings_list(insts, shots, args):
    """Visibility alone: the service hears, nothing reloads, nobody's
    Ready goes. LAN has no listing-only row: there it changes Hide code's
    neighbour... nothing, so the scenario checks the listing call only."""
    host, p2 = insts[0], insts[1]
    lan = args.net == "lan"
    nt.settle(insts, nt.MENU_SETTLE)
    if lan:
        print("net_test_xn_menu: LAN lists every lobby: no listing-only row to change (skipped)", flush=True)
        return True
    rows = settings_rows(teams=True, lan=lan)
    loads = [css_loads(i) for i in insts]
    starts = nt.count(host, r"xeleenet: starting")
    if not open_settings(host):
        return False
    to_row(host, rows, "vis")
    tap(host, "Right")  # Public -> Private
    tap(host, "Return")
    if not host.wait_log(r"xeleenet: settings applied \(the listing only\)", 5) or \
            not host.wait_log(r"xeleenet: settings taken \(HTTP 200\)", 10):
        print("FAIL: the service never took the new visibility")
        return False
    nap(5)
    if nt.count(host, r"xeleenet: starting") != starts or [css_loads(i) for i in insts] != loads:
        print("FAIL: a listing-only change restarted the session")
        return False
    print("net_test_xn_menu: visibility went to the service alone", flush=True)
    return True


def settings_race(insts, shots, args, to_menu, code):
    """Apply while a join is in flight: the third player starts joining, and
    the host's game applies (the knob) at once. The session that seats the
    third carries the new rules, and every player's last commit is equal."""
    host, late = insts[0], insts[-1]
    lan = args.net == "lan"
    join_one(late, to_menu, lan, code)
    if not late.wait_log(r"xeleenet pages: joining", 20):
        print("FAIL: the latecomer never started joining")
        return False
    with open(settings_file(args.work), "w") as f:
        f.write(f"stock=1\nstocks=2\nfighters={FIGHTERS_LOW}\n")
    if not host.wait_log(r"xeleenet: settings applied", 20):
        print("FAIL: the test knob never applied")
        return False
    if not late.wait_log(r"lobby: entering CSS", 90):
        print("FAIL: the latecomer never got in")
        return False
    want = f"(players {(1 << len(insts)) - 1:02x})"
    if not all(i.wait_log(nt.re.escape(want), 60) for i in insts):
        print("FAIL: no session with everyone")
        return False
    nap(8)  # whichever session comes last
    if not same_commit(insts) or not settings_landed(insts, stocks=2, fighters=FIGHTERS_LOW):
        return False
    shot(late, shots, "s-race-late")
    return True


def run(args):
    # settings-seats: a 4th player, kept out of the lobby until it has 4 seats.
    extra_seat = args.scenario == "settings-seats"
    # settings-race: the last guest joins once the host's Apply is under way.
    late = args.scenario == "settings-race"
    names = "abcd"[:args.players + (1 if extra_seat else 0)]
    shots = args.shots or os.path.join(args.work, "shots")
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(shots)
    ports = [args.port + k for k in range(len(names))]
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
        if args.scenario == "settings-teams" and k == 0:
            # Teams off, 4 seats: the scenario turns Teams on from Lobby Settings.
            with open(os.path.join(ident, "xeleenet-lobby.txt"), "w") as f:
                f.write(f"preset=3\nname=Teams test\nopen=1\nseats=4\nteams=0\nstock=1\nstocks=4\nminutes=8\n"
                        f"items=-1\nff=0\nhandicap=0\ndamage=10\npause=1\nstages={COMPETITIVE}\nfighters={FIGHTERS_ALL}\n"
                        f"frozen=1\nhide=0\n")
        env = {
            "MELEE_XN_URL": args.url,
            "MELEE_XN_NAME": NAMES[k],
            "MELEE_XN_IDENTITY": ident,
            # A safety exit: some 8 minutes paced; unpaced runs at 300+ fps, so
            # the same margin is several times the frames.
            "MELEE_NET_EXIT_AFTER_FRAMES": str(int(args.seconds * 60) + (30000 if args.paced else 250000)),
            "MELEE_XN_TEST_HAND": "1",  # where the hands are (pick_high)
        }
        if args.block:
            env["MELEE_NET_SIM_BLOCK"] = args.block
        if args.no_discovery:
            env["MELEE_NET_SIM_NO_MCAST"] = "1"
        if args.scenario == "sss-repick" and k == 1:
            env["MELEE_NET_SIM_DELAY_MS"] = "80"  # P2's last change races P3's pick
        if args.scenario in ("settings-engine", "settings-race") and k == 0:
            env["MELEE_XN_TEST_APPLY"] = f"30:{settings_file(args.work)}"
        if args.scenario.startswith("multipad") and k == 0:
            fifo = os.path.join(args.work, f"{name}.pad2.keys")
            os.mkfifo(fifo)
            env["MELEE_KEY_FIFO_P2"] = fifo  # the host's second pad
        insts.append(nt.Instance(name, args.exe, args.disc, args.work, ports[k], 0, env, True))
    everyone = list(insts)  # killed at the end, whoever plays on
    RUNNING[:] = everyone
    ok = False
    try:
        # Booted into the menu, every instance at once (nt.BOOT_MENU).
        if not nt.to_main_menu(insts):
            print("FAIL: an instance never reached the main menu")
            return False
        extra = insts.pop() if extra_seat else None
        host, guests = insts[0], insts[1:]
        lan = args.net == "lan"
        to_menu = TO_ONLINE + (TO_LAN_ROW if lan else [])
        walk(host, to_menu)
        shot(host, shots, "1-online-mode")
        walk(host, [("X", 7)])
        shot(host, shots, "2-xeleenet-menu")
        walk(host, [("X", 7)])
        nap(1.5)
        if (args.players > 2 or args.scenario == "multipad") and \
                args.scenario not in ("set", "teams", "fighters", "details", "settings-teams"):
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
        nap(3)
        shot(host, shots, "4-host-css-alone")
        if args.scenario == "leave-host-alone":
            ok = leave_host_alone(host, shots)
            return ok
        if args.scenario == "settings-alone":
            ok = settings_alone(host, shots)
            return ok
        joiners = list(guests[:-1] if late else guests)
        # Every guest walks to Join's lobby browser (B1) together; only the
        # joins themselves go one after another.
        walk(joiners, to_menu + [("X", 7), ("Down", 7), ("X", 7)])
        nap(2.5)
        for k, g in enumerate(joiners):
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
            nap(0.6)
            shot(g, shots, f"8-joining-{g.name}")
            if args.join_shots and k == 0:
                # What the joiner sees until the CSS (a Quest find: the list
                # came back for a few seconds).
                for n in range(16):
                    nap(0.35)
                    shot(g, shots, f"8-joining-{g.name}-{n:02d}")
            # Seated: this guest's own session agreed and its CSS entered.
            if not g.wait_log(r"lobby: entering CSS", 90):
                print(f"[{g.name}] FAIL: never got into the lobby")
                return False
        if args.scenario == "details":
            insts = insts[:2]  # the 2-seat lobby's players
        if late and not settings_race(insts, shots, args, to_menu, code):
            return False
        want = f"(players {(1 << len(insts)) - 1:02x})"
        for inst in insts:
            if not inst.wait_log(nt.re.escape(want), 90):
                print(f"[{inst.name}] FAIL: never in a session with everyone {want}")
                return False
        if not all(i.wait_log(nt.SCENE_FILE["css"], 120) for i in insts):
            print("FAIL: the CSS never loaded everywhere")
            return False
        nap(4)
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
        scenes = {"settings-engine": settings_engine, "settings-hold": settings_hold, "settings-open": settings_open,
                  "settings": settings_walk, "settings-cancel": settings_cancel, "settings-teams": settings_teams,
                  "settings-list": settings_list}
        if args.scenario in scenes and not scenes[args.scenario](insts, shots, args):
            return False
        if args.scenario == "settings-seats":
            if not settings_seats(insts, extra, shots, args, code):
                return False
            insts = insts + [extra]
            extra = None
        if args.scenario != "ready" and not drive_css(insts):
            print("FAIL: no CSS pick reached the SSS")
            return False
        if args.scenario == "fighters" and not fighters_check(insts, shots):
            return False
        nap(2)
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
        nap(12)
        shot(host, shots, "11-match")
        if args.scenario == "set":
            if not rematch(insts, shots, drive):
                return False
        else:
            nap(args.seconds)
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
            # Unpaced, nothing asks them to exit (the frame cap is far off):
            # a grace is only dead time.
            inst.kill(0 if nt.UNPACED else 5)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--players", type=int, choices=(2, 3, 4), default=3)
    ap.add_argument("--seconds", type=float, default=None,
                    help="of match after it loads (40 paced, 10 unpaced: game time either way)")
    ap.add_argument("--block", default="")
    ap.add_argument("--scenario", default="",
                    choices=("", "sss-host-b", "ready", "leave-guest", "leave-host", "sss-repick", "sss-timeout",
                             "sss-same", "css-b-hold", "leave-host-alone", "set", "teams", "multipad",
                             "multipad-full", "fighters", "details", "settings-engine", "settings-hold",
                             "settings-open", "settings", "settings-cancel", "settings-seats", "settings-teams",
                             "settings-alone", "settings-race", "settings-list"),
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
                         "(use --players 3); "
                         "settings-*: Lobby Settings (docs/xeleenet-settings-overlay-plan.md): -engine applies "
                         "with no UI (MELEE_XN_TEST_APPLY), -hold rests the host's port, -open opens it from the "
                         "banner and with Z, settings walks the UI and applies, -cancel changes nothing, -seats "
                         "lowers and raises the seats (a 4th joins), -teams deals and keeps teams, -alone applies "
                         "with nobody in, -race applies while a join is in flight, -list changes the listing only")
    ap.add_argument("--paced", action="store_true",
                    help="the old way: 60 Hz, key holds in milliseconds (docs/fast-tests-plan.md)")
    ap.add_argument("--no-shots", action="store_true",
                    help="no screenshots, and none of the waits that only let the screen settle for one")
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
    nt.UNPACED = not args.paced  # docs/fast-tests-plan.md T4: as fast as it runs
    nt.BOOT_MENU = True  # every instance boots into the main menu
    nt.claim_slot(exclusive=args.paced, lan=args.net == "lan")
    args.port = nt.slot_port(args.port)
    if nt.SLOT and args.work == ap.get_default("work"):
        args.work += f"-{nt.SLOT}"
    global SHOTS
    SHOTS = not args.no_shots
    if args.seconds is None:
        args.seconds = 40.0 if args.paced else 10.0
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
