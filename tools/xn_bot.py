#!/usr/bin/env python3
"""A desktop player to test the Quest (or anyone) against in XeleeNet: it
hosts a lobby and plays on by itself, game after game.

    tools/xn_bot.py [--url http://127.0.0.1:8790] [--name DESKBOT] [--idle] [--lan]

1. Hosts a public 1v1 lobby: 1-minute time matches, no items, the
   Competitive stage list (written to its profile's xeleenet-lobby.txt, which
   the Host page loads).
2. In the CSS, once someone has joined: picks a character and readies (START).
   A roster change clears Ready; it readies again.
3. In the stage select: picks Battlefield, Final Destination and Dream Land
   in turn (Random if the cursor can't get there).
4. In the match: moves and attacks (net_test.py's workout), unless --idle.
5. On the results: START, back to the CSS, and round again from 2.

It runs the build tag the Quest's melee-env.txt names (xn-trial, as
tools/xn_trial.py's menu/quest-menu set), so the two see each other's
lobbies on the same service: `tools/xn_trial.py serve` (or one already on
8790, behind the Funnel). The game's log goes to build/xn-bot/bot.log; the
bot prints what it does. Ctrl-C stops it.

--lan hosts on LAN instead (Online Mode > LAN, no service): the Quest finds
the lobby on the local network, or joins by the address the bot prints.
"""
import argparse
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "xeleenet"))
import net_test as nt  # noqa: E402
import net_test_xn_menu as xm  # noqa: E402

DEFAULT_DISC = os.path.expanduser("~/Downloads/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso")
REV = "xn-trial"  # tools/xn_trial.py's tag: the Quest's melee-env.txt runs it
# The Competitive list (xeleenet_menu.cpp kCompetitive): Random Stage Switch ids.
COMPETITIVE = sum(1 << b for b in (24, 25, 26, 4, 5, 7))
STAGES = [(xm.BF, "Right"), (xm.FD, "Right"), (xm.DL, "Right")]  # the bottom row, left to right
REHOST = 2  # run()'s answer: start the game again and host a new lobby
ENTER = re.compile(r"online: enter (CSS|SSS|VS splash|VS|RESULTS) at frame")


class Bot:
    """The game, its key fifo and its log (net_test.Instance's interface,
    which net_test's helpers drive)."""

    def __init__(self, args):
        self.name = "bot"
        work = args.work
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        ident = os.path.join(work, "id")
        os.makedirs(ident)
        with open(os.path.join(ident, "xeleenet-lobby.txt"), "w") as f:
            f.write(f"preset=3\nname={args.name}'s 1v1\nopen=1\nseats=2\nteams=0\nstock=0\nstocks=4\n"
                    f"minutes=1\nitems=-1\nff=0\nhandicap=0\ndamage=10\npause=0\nstages={COMPETITIVE}\n"
                    f"frozen=1\nhide=0\n")
        self.log_path = os.path.join(work, "bot.log")
        self.fifo = os.path.join(work, "bot.keys")
        os.mkfifo(self.fifo)
        env = dict(os.environ,
                   SDL_VIDEO_DRIVER=os.environ.get("SDL_VIDEO_DRIVER", "x11"),
                   MELEE_VSYNC="0", MELEE_KEY_FIFO=self.fifo, MELEE_FPS="1", MELEE_NO_CARD="1",
                   MELEE_WINDOW_TITLE="xn-bot", MELEE_XN_URL=args.url, MELEE_APP_REV=REV,
                   MELEE_XN_NAME=args.name, MELEE_XN_IDENTITY=ident,
                   MELEE_CACHE_DIR=os.path.join(nt.CACHE_ROOT, "xn-bot"))
        for k in ("MELEE_XN", "MELEE_NET", "MELEE_DEBUG_VS", "MELEE_NET_RECORD"):
            env.pop(k, None)
        os.makedirs(env["MELEE_CACHE_DIR"], exist_ok=True)
        self.log = open(self.log_path, "wb")
        self.proc = subprocess.Popen([args.exe, "--no-card", args.disc], env=env, stdout=self.log,
                                     stderr=subprocess.STDOUT)

    def key(self, line):
        nt.fifo_write(self.fifo, line)

    def text(self):
        with open(self.log_path, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def wait_log(self, pattern, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline and self.proc.poll() is None:
            if re.search(pattern, self.text()):
                return True
            time.sleep(0.25)
        return False


def say(msg):
    print(f"[{time.strftime('%H:%M:%S')}] xn_bot: {msg}", flush=True)


class Workout:
    """net_test.py's workout on the bot's pad, until stopped."""

    def __init__(self, bot):
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, args=(bot,), daemon=True)
        self._t.start()

    def _run(self, bot):
        i = random.randrange(len(nt.WORKOUT))
        while not self._stop.is_set() and bot.proc.poll() is None:
            key, ms = nt.WORKOUT[i % len(nt.WORKOUT)]
            i += 1
            try:
                bot.key(f"{key} {ms}")
            except Exception:
                return
            self._stop.wait(ms / 1000 + 0.15)

    def stop(self):
        self._stop.set()
        self._t.join(5)


def last_index(text, pattern):
    m = None
    for m in re.finditer(pattern, text):
        pass
    return m.start() if m else -1


def steer(bot, key, cell, since, tries=40):
    """net_test_xn_menu.steer, reading only this stage select's log (`since`:
    where it began): the last one's cursor isn't this one's."""
    for _ in range(tries):
        m = re.findall(r"sss: over cell (\d+)", bot.text()[since:])
        if m and int(m[-1]) == cell:
            return True
        nt.press((bot,), key, 3, 0.15)
    return False


def pick(bot, column):
    """A character on the CSS's second row (the harness's sweep, one press)."""
    nt.press((bot,), "Left+Up", nt.CSS_PIN, 1.0)
    nt.press((bot,), "Down", nt.CSS_ROWS[1], 0.6)
    nt.press((bot,), "Right", nt.CSS_FIRST + column * nt.CSS_STEP, 0.6)
    nt.press((bot,), "X", 7, 0.6)


def run(args):
    bot = Bot(args)
    say(f"log: {bot.log_path}")
    try:
        if not nt.to_main_menu((bot,)):
            say("never reached the main menu")
            return 1
        rows = xm.TO_LAN_ROW if args.lan else []
        xm.walk(bot, xm.TO_ONLINE + rows + [("X", 7), ("X", 7)])  # Online Mode > XeleeNet (or LAN) > Host
        time.sleep(1.5)
        nt.press((bot,), "Return", 9, 1.0)  # START: open the lobby
        if not bot.wait_log(r"xeleenet: lobby code [A-Z0-9]{6}", 60):
            say("the lobby never opened (is the service up? tools/xn_trial.py serve)")
            return 1
        code = re.search(r"xeleenet: lobby code ([A-Z0-9]{6})", bot.text()).group(1)
        say(f"hosting {args.name}'s 1v1, code {code[:3]} {code[3:]}: 1-minute time, no items")
        if args.lan:
            m = re.search(r"xeleenet: hosting lobby \S+, code \S+, \d+ seats on LAN (\S+)", bot.text())
            say(f"on LAN at {m.group(1) if m else '?'}")
        scene, games, stage_i, column = None, 0, 0, random.randrange(nt.CSS_COLS)
        workout, last_try, last_start, lost_since = None, 0.0, 0.0, 0.0
        need_pick, no_picks = True, 0  # a START with no pick says the pick went
        while bot.proc.poll() is None:
            text = bot.text()
            # The lobby went (the guest dropped, the session failed): the
            # game is out of the CSS and nothing comes after. Host again.
            lost = last_index(text, r"net: disconnected|xeleenet pages: begin") > last_index(text, r"online: enter ")
            lost_since = (lost_since or time.time()) if lost else 0.0
            if lost and time.time() - lost_since > 10:
                say("the lobby went; hosting again")
                return REHOST
            entered = ENTER.findall(text)
            now_scene = entered[-1] if entered else None
            if now_scene != scene:
                if workout is not None:
                    workout.stop()
                    workout = None
                scene = now_scene
                say(f"in the {scene}")
                if scene == "VS" and not args.idle:
                    workout = Workout(bot)
                if scene == "SSS":
                    time.sleep(1.5)
                    cell, key = STAGES[stage_i % len(STAGES)]
                    stage_i += 1
                    since = last_index(bot.text(), r"online: enter SSS")
                    if steer(bot, "Up", xm.BF, since) and (cell == xm.BF or steer(bot, key, cell, since)):
                        nt.press((bot,), "X", 7, 0.3)
                        say(f"picked cell {cell}")
                    else:
                        nt.press((bot,), "Return", 9, 0.3)
                        say("picked Random")
                if scene == "RESULTS":
                    games += 1
                    say(f"game {games} over")
                continue
            if scene == "CSS":
                css_at = last_index(text, r"online: enter CSS")
                # Someone else is in: the last session agreed has two players.
                agreed = re.findall(r"XeleeNet session \d+ agreed, CSS at frame -?\d+ \(players (\w+)\)", text)
                alone_at = last_index(text, r"host alone again")
                joined = (agreed and bin(int(agreed[-1], 16)).count("1") > 1 and
                          last_index(text, r"XeleeNet session \d+ agreed") > alone_at)
                ready = last_index(text, r"css: P1 ready") > max(css_at, last_index(text, r"css: P1 not ready"))
                if joined and not ready and time.time() - last_try > 4:
                    n = len(re.findall(r"css: P1 START with no pick", text))
                    if need_pick or n > no_picks:
                        pick(bot, column)
                        column = (column + 1) % nt.CSS_COLS
                    need_pick, no_picks = False, n
                    nt.press((bot,), "Return", 9, 0.3)
                    say("ready")
                    last_try = time.time()
            elif scene == "RESULTS" and time.time() - last_start > 2:
                nt.press((bot,), "Return", 9, 0.3)
                last_start = time.time()
            time.sleep(0.5)
        say("the game exited")
        return 0
    except KeyboardInterrupt:
        say("stopping")
        return 0
    finally:
        if bot.proc.poll() is None:
            bot.proc.terminate()
            try:
                bot.proc.wait(10)
            except subprocess.TimeoutExpired:
                bot.proc.kill()
        bot.log.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--url", default="http://127.0.0.1:8790", help="the lobby service (tools/xn_trial.py serve)")
    ap.add_argument("--name", default="DESKBOT", help="the bot's XeleeNet name (up to 12)")
    ap.add_argument("--idle", action="store_true", help="stand still in matches")
    ap.add_argument("--lan", action="store_true", help="host on LAN (Online Mode > LAN), not the service")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "linux", "melee"))
    ap.add_argument("--disc", default=DEFAULT_DISC)
    ap.add_argument("--work", default=os.path.join(ROOT, "build", "xn-bot"))
    args = ap.parse_args()
    if not os.path.exists(args.disc):
        sys.exit(f"no disc at {args.disc}; pass --disc")
    # A SIGTERM ends it as Ctrl-C does, so the game goes with it.
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    while True:
        rc = run(args)
        if rc != REHOST:
            sys.exit(rc)


if __name__ == "__main__":
    main()
