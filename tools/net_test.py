#!/usr/bin/env python3
"""Netplay integration test: two instances on one machine, a match, then asserts.

    tools/net_test.py [--loss PCT] [--delay MS] [--rxdelay MS] [--jitter] [--reorder]
                      [--burst] [--dup] [--minutes N] [--oom FRAME]
                      [--disconnect [a|b]] [--stall SECONDS] [--reconnect-ms MS]
                      [--fuzz] [--exe build/melee]
                      [--disc ../melee.ciso] [--port 42050] [--work /tmp/net_test]

Direct mode (default): MELEE_NET=127.0.0.1:<peer> + MELEE_DEBUG_VS=1, Start
pressed twice through MELEE_KEY_FIFO (title, then straight into Link vs Mario).
LAN and XeleeNet lobbies, N players: tools/net_test_n.py and
tools/net_test_xn_menu.py (--scenario set: a whole set through results and a
rematch).
--oom FRAME: MELEE_NET_SIM_OOM_FRAME on A, so its first
snapshot at/after FRAME fails like a realloc would; check_oom() asserts the
session drops to lockstep and finishes anyway. --disconnect: B SIGKILLed
mid-match (no BYE), asserting A times the peer out within the stall timeout
and keeps its frame loop running. --stall SECONDS: B SIGSTOPped mid-match,
asserting the reconnect window (net.c:639-832) survives an interruption
inside it and expires with status 2 past it. --fuzz: tools/net_fuzz.py
hammers instance A's port during the match.

Each instance runs until MELEE_NET_EXIT_AFTER_FRAMES (net.c) prints
"net: test done", or the test times out. Pass = both done, exit 0, no DESYNC,
no "peer silent", no rollback lost. Exit code 0 on pass, 1 on fail.
Logs: <work>/a.log, <work>/b.log. tools/net_acceptance.py imports parse_args()
and run() to sweep the link-simulator matrix.
"""
import argparse
import errno
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time

# net.frame counts every frame the loop runs, boot included, and a cold
# MELEE_CACHE_DIR spends ~150 s storing the disc's archives before the title
# appears (measured on this machine at load average 20, ~50 frames/s through
# boot). The exit frame is fixed at launch, so the budget covers the worst
# boot seen and check_match() fails a row whose match did not get its minutes.
BOOT_FRAMES = 9000
MM_RUN = False  # set by run() for --mm: frames count from the match, not from boot
CACHE_ROOT = os.path.expanduser("~/.cache/melee_net_cache")  # kept between runs; keyed by port, never shared
LOAD_STALL_FRAME = 900  # --load-stall: session up, menus lockstep, every frame waits
# A direct session derives a datagram key from its handshake nonces like any
# other (src/pc/net_handshake.c), but only once that handshake completes a few
# hundred frames into boot; before it there is nothing to authenticate with
# unless the peers were started with a shared secret. Every fixture here sets
# one, so the matrix exercises the authenticated path from the first datagram
# rather than the window no player should be exposed to; tools/net_fuzz.py
# reads this to tag its own datagrams. A LAN fixture leaves it unset and keys
# off the handshake.
NET_KEY = "melee-pc net fixture key"
SIM_ENV = {  # CLI flag -> (env knob, value) in src/pc/net.c's link simulator
    "jitter": ("MELEE_NET_SIM_JITTER_MS", "20"),
    "reorder": ("MELEE_NET_SIM_REORDER", "10"),
    "burst": ("MELEE_NET_SIM_BURST", "5"),
    "dup": ("MELEE_NET_SIM_DUP", "10"),
}

# The only per-scene marker the game emits: the PC file layer prints a line per
# disc read and every scene loads its own archive (mnmain.c, mncharsel.c:5472,
# mnstagesel.c:569, gmvsmode.c:169 asks for St_Kind_Last = GrNLa). It must be
# the HIT line: file_cache.cpp:432-486 prewarms GrNLa/GrSt/GrPs/GrOp/GrYs by
# name at boot, so a STORED line says nothing about the scene. LOOSE HIT is the
# same read served from an extracted files/ dir (file_cache.cpp:325). These
# lines carry no timestamp, so they are matched by presence and count.
SCENE_FILE = {
    "title": r"\[FileCache\] (?:LOOSE )?HIT: GmTtAll\.",
    "menu": r"\[FileCache\] (?:LOOSE )?HIT: MnMaAll\.",
    "css": r"\[FileCache\] (?:LOOSE )?HIT: MnSlChr\.",
    "sss": r"\[FileCache\] (?:LOOSE )?HIT: MnSlMap\.",
    "match": r"\[FileCache\] (?:LOOSE )?HIT: Gr[A-Za-z0-9]+\.(?:dat|usd)",
}


def fifo_write(path, line, tries=100):
    """One key line to a MELEE_KEY_FIFO; the game reopens it after every writer."""
    for _ in range(tries):
        try:
            fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
            break
        except OSError as e:
            if e.errno != errno.ENXIO:
                raise
            time.sleep(0.1)  # reader thread not up yet
    else:
        raise RuntimeError(f"no reader on {path}")
    with os.fdopen(fd, "w") as f:
        f.write(line + "\n")


def wait_port_free(port, timeout=30):
    """Block until `port` can be bound, or give up. The previous run's
    instances hold their UDP port through aurora's GPU teardown, which takes
    seconds; starting on top of that makes the new instances fail to bind and
    play unconnected offline games that look like a desync."""
    import socket
    deadline = time.time() + timeout
    while True:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("", port))
            return True
        except OSError:
            if time.time() > deadline:
                print(f"net_test: UDP {port} still in use after {timeout}s", flush=True)
                return False
            time.sleep(0.5)
        finally:
            s.close()


class Instance:
    def __init__(self, name, exe, disc, work, port, peer_port, env, lobby, slippi=False):
        self.name = name
        self.port = port
        self.log_path = os.path.join(work, f"{name}.log")
        self.fifo = os.path.join(work, f"{name}.keys")
        os.mkfifo(self.fifo)
        self.rec_path = os.path.join(work, f"{name}.rec")
        cache = os.path.join(CACHE_ROOT, str(port))
        os.makedirs(cache, exist_ok=True)
        e = dict(os.environ)
        # A prior direct/replay run must not silently change a LAN fixture.
        for key in ("MELEE_DEBUG_VS", "MELEE_NET", "MELEE_NET_PLAYER", "MELEE_NET_REPLAY",
                    "MELEE_NET_KEY"):
            e.pop(key, None)
        e.update({
            "SDL_VIDEO_DRIVER": os.environ.get("SDL_VIDEO_DRIVER", os.environ.get("SDL_VIDEODRIVER", "x11")),
            "MELEE_VSYNC": "0",
            "MELEE_NET_PORT": str(port),
            "MELEE_CACHE_DIR": cache,
            "MELEE_WINDOW_TITLE": f"melee-net-{name}",
            "MELEE_KEY_FIFO": self.fifo,
            "MELEE_SEED": os.environ.get("MELEE_SEED_" + name.upper(), "7"),  # _A/_B: per-instance
            "MELEE_FPS": "1",  # one line per second: the rate the drive scales its holds by
            "MELEE_NET_RECORD": self.rec_path,  # per-frame pads + checksum, for check_match()
        })
        if not lobby:  # a lobby (matchmaking, XeleeNet) starts the session itself
            e["MELEE_NET"] = f"127.0.0.1:{peer_port}"
            e["MELEE_NET_PLAYER"] = "0" if name == "a" else "1"
            e["MELEE_DEBUG_VS"] = "1"
            e["MELEE_NET_KEY"] = NET_KEY
            if slippi:
                # The Slippi ENet transport (docs/slippi-migration-plan.md).
                # No MELEE_NET_KEY: ENet carries its own reliability, and the
                # Slippi transport handshakes with PC_SLIPPI_MSG_SETUP instead
                # of the custom RULES/READY the key authenticates.
                e.pop("MELEE_NET_KEY", None)
                e["MELEE_NET_STACK"] = "slippi"
        e.update(env)
        e.pop("MELEE_LOG_FILE", None)  # stderr is captured below; avoid double lines
        self.log = open(self.log_path, "wb")
        self.proc = subprocess.Popen([exe, "--no-card", disc], env=e, stdout=self.log,
                                     stderr=subprocess.STDOUT)

    def key(self, line):
        fifo_write(self.fifo, line)

    def text(self):
        with open(self.log_path, "rb") as f:
            return f.read().decode("utf-8", "replace")

    def wait_log(self, pattern, timeout):
        """True once `pattern` (regex) appears in the log; False on timeout/exit."""
        deadline = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < deadline:
            if rx.search(self.text()):
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.25)
        return False

    def kill(self, grace=0.0):
        """SIGKILL, after `grace` seconds for a self-exit (aurora's GPU teardown
         takes a few seconds, and a killed instance reports exit code -9)."""
        deadline = time.time() + grace
        while self.proc.poll() is None and time.time() < deadline:
            time.sleep(0.25)
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()
        self.log.close()


def count(inst, pattern):
    return len(re.findall(pattern, inst.text()))


def frame_ms(inst):
    """Wall ms per simulated frame, from MELEE_FPS's per-second line (vi.c:97-128).
    Key holds are wall-clock but the cursor moves per frame, so a machine at
    20 fps moves it a third as far as the 60 fps the holds were written for:
    every hold below is given in frames and scaled through here."""
    fps = [float(x) for x in re.findall(r"^fps ([\d.]+) ", inst.text(), re.M)]
    recent = [x for x in fps[-3:] if x > 0]
    return 1000.0 / max(5.0, sum(recent) / len(recent)) if recent else 1000.0 / 60


def press(insts, key, frames, settle=0.6):
    """Hold `key` for `frames` simulated frames on each instance, then wait out
    the hold, the fifo thread's own 100 ms gap and `settle` seconds."""
    worst = 0.0
    for inst in insts:
        ms = frames * frame_ms(inst)
        worst = max(worst, ms)
        inst.key(f"{key} {int(ms)}")
    time.sleep(worst / 1000 + 0.1 + settle)


def press_until(insts, key, frames, pattern, want=1, tries=15, each=6.0, on=None):
    """Press until `pattern` has appeared `want` times in every instance's log.
    Fixed schedules are what left the old drive at the title screen for entire
    runs: how many frames a load takes depends on the machine's load, so every
    step waits for the scene's own archive instead. `on` narrows who is pressed
    - once the session is up both peers simulate both pads, so one is enough,
    and a stray Start inside a match would pause it."""
    def there():
        return all(count(i, pattern) >= want for i in insts)

    for _ in range(tries):
        if there():
            return True
        press(on or insts, key, frames, 0)
        deadline = time.time() + each
        while time.time() < deadline:
            if there():
                return True
            if any(i.proc.poll() is not None for i in insts):
                return False
            time.sleep(0.25)
    return there()


# Both players keep moving for as long as a match runs. Without this the matrix
# measures an idle link: an untouched Melee match has a *constant* frame
# checksum (frame_checksum folds position, motion id, percent, stocks and the
# RNG seed, and all of those stand still when two fighters stand still), and
# "repeat the last input" is only ever a wrong prediction when the input
# changes - so an idle match rolls back exactly zero times however bad the
# link is. Start is deliberately absent: it would pause the match.
WORKOUT = [("Right", 240), ("X", 100), ("Left", 240), ("C", 100), ("Right+X", 170),
           ("Z", 100), ("Up", 140), ("Down", 140)]


# --combat: the fighters close in and trade hits all match, so hurtboxes and
# hitboxes are exercised every few frames (WORKOUT mirrors both players, who
# then rarely meet). A starts on the left and walks right; B gets the mirror
# image. Holds differ a little per side so the two inputs do not stay in step.
COMBAT = [("Right", 160), ("X", 60), ("Right+X", 110), ("Up+X", 90), ("Down+X", 90),
          ("X", 60), ("Z", 90), ("Left", 70), ("Right+Z", 120), ("C", 80), ("Up", 90)]


def mirror(key):
    return key.replace("Right", "\0").replace("Left", "Right").replace("\0", "Left")


class Workout:
    """Cycles WORKOUT through both key fifos until done(). Holds are wall-clock
    (the exact frame count does not matter here, only that the input keeps
    changing) so the thread never has to read the logs."""

    def __init__(self, insts, combat=False):
        self.insts = insts
        self.n = 0
        self._stop = threading.Event()
        if combat:
            self._threads = [threading.Thread(target=self._combat, args=(inst, side), daemon=True)
                             for side, inst in enumerate(insts)]
        else:
            self._threads = [threading.Thread(target=self._run, daemon=True)]
        self._thread = self._threads[0]
        for t in self._threads:
            t.start()

    def _combat(self, inst, side):
        i = 0
        while not self._stop.is_set():
            key, ms = COMBAT[i % len(COMBAT)]
            i += 1
            key = mirror(key) if side else key
            ms += 13 * side + (i % 3) * 7
            if inst.proc.poll() is None:
                try:
                    inst.key(f"{key} {ms}")
                except Exception:
                    return
            self.n += 1
            self._stop.wait(ms / 1000 + 0.15)

    def _run(self):
        i = 0
        while not self._stop.is_set():
            key, ms = WORKOUT[i % len(WORKOUT)]
            i += 1
            for inst in self.insts:
                if inst.proc.poll() is None:
                    try:
                        inst.key(f"{key} {ms}")
                    except Exception:
                        return  # the run is being torn down
            self.n += 1
            self._stop.wait(ms / 1000 + 0.15)

    def done(self):
        """Stop and wait for the last line to be written, so a drive step that
        follows has the fifo to itself."""
        self._stop.set()
        for t in self._threads:
            t.join(10)
        return self.n


def drive_direct(a, b):
    """MELEE_DEBUG_VS: Start clears the card message box, skips the opening
    movie, and at the title jumps straight into Link vs Mario (gmtitlemode.c:61,
    gmvsmode.c:163-201). Both instances are in the session from boot, so A's pad
    is B's too and only A is driven - one Start per side would risk pausing the
    match it just started. Press until both logs show the match's own stage
    archive: waiting for the title first deadlocks, because the boot needs a
    Start of its own to get past the memory-card box."""
    if not press_until((a, b), "Return", 9, SCENE_FILE["match"], tries=40, each=8.0, on=(a,)):
        print("net_test: never got a stage archive: the match never loaded", flush=True)
        return False
    if wait_match((a, b), 150):
        return True
    # A Start that landed while the match was loading pauses it; one more
    # unpauses, and a paused match shows up as a frozen checksum either way.
    print("net_test: the match is not simulating, trying one unpause", flush=True)
    press((a,), "Return", 9, 2)
    return wait_match((a, b), 120)


def settle(insts, frames):
    """Wait out `frames` simulated frames of wall clock. Every menu hop plays
    an entry animation and ignores input while it runs, and an animation is
    counted in frames, so a fixed 1.5 s wait lands in the middle of one on a
    machine running at 20 fps and the press that follows is swallowed.
    Measured: /tmp/sf_scenes1, where b's Down at the main menu was dropped and
    the fixed schedule walked it into the vanilla CSS instead of the lobby
    while a reached the lobby from the identical key sequence."""
    time.sleep(max(frames * frame_ms(i) for i in insts) / 1000.0)


# From a *verified* main menu (cursor on SEL_MAIN_1P): Down = VS MODE, A enters
# the VS submenu, Up wraps to SEL_VS_ONLINE, A enters the ONLINE menu with
# SEL_ONLINE_LAN already selected, A takes it into the lobby. Every submenu
# entry resets the cursor to index 0 (mn_804A04F0.hovered_selection = 0,
# mnmain.c:2385) and the main table has 6 entries on TARGET_PC (mn_803EB6B0)
# with SEL_MAIN_ONLINE third (forward.h), so the walk
# is stateless: a retry starts from the same place. X = A button in the
# keyboard map, Z = B.
# Main Menu -> Online Mode (third, under VS. Mode) -> (Slippi is first; Down to) LAN
MENU_SETTLE = 90  # frames of animation a menu hop can eat a press during
CSS_BACK = 40     # frames of held B that leave a character-select screen (>30)


def both(a, b, key, pause):
    for inst in (a, b):
        inst.key(key)
    time.sleep(pause)


def to_main_menu(insts, tries=8):
    """Reach the main menu from anywhere the boot leaves us, and prove it.
    Start is a menu confirm as well as the boot's "dismiss" button
    (PAD_CONFIRM = A | START, gm_1A36.c:118), so pressing it through the card
    message box, the movie and the title walks on into whatever submenu the
    cursor sits on. B backs out one layer per press and the main menu's own B
    returns to the title (mnmain.c:2851-2857), so three B's reach the title
    from any of those depths. The title hands itself over to the opening movie
    after a few idle seconds and cycles back (gmtitlemode.c:73-75), so Start is
    then offered every couple of seconds until one lands inside a title window:
    that re-enters the menu scene, which reloads MnMaAll (a submenu hop does
    not) and always starts on SEL_MAIN_1P (gmmenumode.c:100-103). The rising
    archive count is the proof, and it is what makes the hops below
    deterministic instead of hopeful.

    B is held for CSS_BACK frames, not tapped: a character-select screen only
    leaves on 30 frames of held B (mncharsel.c:2579-2585), and a walk that
    dropped one press lands in exactly that screen, where every menu key is
    otherwise a no-op. A 7-frame tap left both instances stuck there for the
    rest of the run (/tmp/sf_scenes1)."""
    for _ in range(tries):
        want = [count(i, SCENE_FILE["menu"]) + 1 for i in insts]

        def there():
            return all(count(i, SCENE_FILE["menu"]) >= w for i, w in zip(insts, want))

        for _ in range(3):
            press(insts, "Z", CSS_BACK, 1.0)
        for _ in range(10):
            press(insts, "Return", 9, 2.0)
            if there():
                settle(insts, MENU_SETTLE)  # the menu is loading, not listening yet
                return True
            if any(i.proc.poll() is not None for i in insts):
                return False
    return False


def drive_mm(a, b):
    """Slippi's order: both instances boot into the Unranked flow, which opens on
    the CSS with a pick already placed (MELEE_CSS_PICK, different per seat);
    START on each starts the search, the synthesized ticket pairs them, and the
    selections that cross the link must give both the same match."""
    if not (a.wait_log(r"online: enter CSS", 180) and b.wait_log(r"online: enter CSS", 60)):
        print("net_test: the online CSS never opened", flush=True)
        return False
    settle((a, b), MENU_SETTLE)
    if not press_until((a, b), "Return", 9, r"match: Slippi ticket", tries=6, each=8.0,
                       on=(a, b)):
        print("net_test: START on the CSS never started a search", flush=True)
        return False
    rx = r"match: Slippi match P\d: chars (\d+)/(\d+) colors (\d+)/(\d+) stage (\d+) alt (\d+) rng (\d+)"
    if not (a.wait_log(rx, 60) and b.wait_log(rx, 60)):
        print("net_test: the selections never produced a match", flush=True)
        return False
    ma, mb = re.findall(rx, a.text()), re.findall(rx, b.text())
    if ma != mb or int(ma[-1][4]) not in (2, 3, 8, 28, 31, 32):
        print(f"net_test: the peers disagree on the match: a={ma} b={mb}", flush=True)
        return False
    print(f"net_test: agreed Slippi match {ma[-1]}", flush=True)
    return True


# CSS hand geometry, all read off src/melee/mn/mncharsel.c - measured
# constants, not a calibration:
#   speed   getStickDelta (:546) returns (|stick|^2 - 200) scaled onto the
#           stick's unit vector and CursorThink (:2595) adds 0.0002 of that
#           per frame. The keyboard map's full deflection is +-80
#           (keyboard.c:47-51), so a straight hold moves
#           0.0002*(80^2-200) = 1.24 units/frame and a diagonal 1.78 per axis.
#   clamps  x [-35, 26], y [-22, 25] (:2629-2640), so a long hold pins the
#           hand on a corner exactly, whatever frame rate the machine gives.
#   token   a hand inside y (0.2, 22) whose door has no pick takes its own
#           token automatically, no A needed (:3397-3446), and the token then
#           sits at hand + (2.7, -2.0) (:3433-3444). The hit test is on the
#           token, not the hand (:2729-2733).
#   grid    rows are token y (13,20) top, (6,13) middle, (-1,6) bottom
#           (:100-103); columns are 7 units wide from -30.0 to 30.2
#           (:111-120). Only icons with state >= 1 can be picked and state is
#           gm_IsCKindUnlocked() (:4473), so a locked character is a hole in
#           the grid: the middle row is swept because its columns 1-7 (Fox,
#           Ness, ICs, Kirby, Samus, Zelda, Link) are all starters.
#   pick    token in bounds + A sets players[].ckind and releases the token
#           (:2738-2780). It sticks: the hand can move on and press A again
#           without undoing it, as long as it stays 3 units clear of its own
#           dropped token (the re-grab test at :3336-3345).
#   Start   refused unless every door with p_kind != 3 has a pick AND no hand
#           is still holding a token (:3738-3785) - which is why the sweep
#           presses Start only between rows, never while a hand is in flight.
#   hazards A above y 22 is the rules panel: at x > 17.3 it leaves for the
#           lobby and at x < -25.5 it toggles teams (:2848-2853, :2994-3014),
#           and A below y 0.2 hits the HMN/CPU toggle (:3097-3131). The sweep
#           never presses A outside y (0.2, 22), and B is never held on the
#           CSS at all: 30 frames of it also leaves for the lobby (:2579-2585).
CSS_PIN = 40      # frames of Left+Up: 40*1.78 = 71 units, past both clamps
# Down frames from the y=25 clamp, one row of the grid per entry. A wall-clock
# hold delivers the frames the machine's current rate gives it, measured at
# 73-100% of the frames asked for (/tmp/sf_scenes2: 40 asked, 40 delivered at
# the pin; 11 asked, 8 delivered one press later), so the entries are spread
# far enough apart that both ends of that spread land inside the band, and a
# row that lands on a row *boundary* is followed by ones that do not.
CSS_ROWS = (11, 8, 14, 6, 17)
CSS_FIRST = 9     # frames of Right from x=-35: token x -21.1, column 1
CSS_STEP = 6      # frames of Right: 7.4 units, one column
CSS_COLS = 7      # columns 1..7 of the row


LRAS = "Q+E+X+Return"


# ---- was this a match at all? ------------------------------------------
# MELEE_NET_RECORD writes "MRC2" + seed, then per fresh tick the four PADStatus
# simulated and the frame checksum (net_snapshot.c:36-113). frame_checksum only
# folds fighter position, facing, percent, motion id and stocks while in_fight()
# (:117-139), so at a menu it is a pure function of the four pads and the RNG
# seed and moves only when a key is pressed, while a running match moves it
# every single frame. That is the one signal that separates a real row from the
# title screen - where this whole matrix used to pass with rollbacks 0.
# FrameRecord = four PADStatus + u32 checksum + u32 tick-start seed.
# PADStatus is 16 bytes in this
# build, not the 12 the GameCube header's 11 used bytes suggest (measured with
# the build's own flags: `sizeof(PADStatus)=16`), so the stride is 72 and
# record_stride_ok() refuses a file that does not divide by it rather than
# reading garbage checksums.
REC = 76
CK_OFF = 64
REC_FORMATS = {b"MRC1": 68, b"MRC2": 72, b"MRC3": 76, b"MRC4": REC, b"MRC5": REC}  # earlier captures stay readable
MATCH_WINDOW = 600
MATCH_RATIO = 0.5
MATCH_MIN = 1800  # frames of moving state a row has to get, i.e. 30 s of match


def record_stride_ok(size, stride=REC):
    """A recording has magic + seed once per session plus whole records."""
    return any((size - 8 * h) % stride == 0 for h in range(1, 13))


def record_base(data):
    """Offset of the first record. A session restart rewrites the header
    mid-file (session_reset takes net.frame back to 0), so the last header
    that leaves a whole number of records is the live one."""
    magic = data[:4]
    stride = REC_FORMATS.get(magic)
    if stride is None:
        raise RuntimeError(f"unknown recording magic {magic!r}")
    p = len(data)
    while True:
        p = data.rfind(magic, 0, p)
        if p < 0:
            return 0
        if (len(data) - p - 8) % stride == 0:
            return p + 8


def record_cks(path, tail=None):
    """Frame checksums, oldest first (index = net.frame within the session).
    `tail` returns the last N records of the latest session, excluding any
    earlier session header. MRC1 captures remain readable after MRC2 ships."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return []
    if not data:
        return []
    size = len(data)
    stride = REC_FORMATS.get(data[:4])
    if stride is None:
        raise RuntimeError(f"{path}: unknown recording magic {data[:4]!r}")
    if size > 8 and not record_stride_ok(size, stride):
        raise RuntimeError(f"{path}: {size} bytes is not a whole number of {stride}-byte records; "
                           "sizeof(PADStatus) changed and REC needs remeasuring")
    body = data[record_base(data):]
    if tail is not None:
        body = body[-tail * stride:] if tail > 0 else b""
    return [body[i * stride + CK_OFF:i * stride + CK_OFF + 4]
            for i in range(len(body) // stride)]


def moved(cks):
    return [cks[i] != cks[i - 1] for i in range(1, len(cks))]


def match_ratio(path):
    """Share of the last MATCH_WINDOW frames that moved the checksum."""
    m = moved(record_cks(path, MATCH_WINDOW))
    return sum(m) / len(m) if m else 0.0


MATCH_RUN = 60  # consecutive moving frames that only a running match produces


def match_entry(cks):
    """Frame the first real match starts on, or None: the first frame that
    begins MATCH_RUN consecutive frames of moving checksum. A menu moves it
    only when the pads change - a held key is the same pad every frame - so it
    never strings more than a handful together, while a match moves it every
    frame (measured: a driven title run 91/600 frames, a match 600/600)."""
    n = 0
    for i, x in enumerate(moved(cks)):
        n = n + 1 if x else 0
        if n == MATCH_RUN:
            return i - MATCH_RUN + 2  # m[i] compares frame i+1 against frame i
    return None


def in_match_scene(inst):
    """Count of evidence that this instance's scene is GS_VS. The stage
    archive is the obvious marker but it is not sufficient on its own: the
    file layer prewarms five stages by name at boot, and a stkind of 0 makes
    the loader ask for the empty name "Gr.dat", which STAGE_RX does not match
    either. The snapshot report is the one that cannot be fooled: a snapshot
    is only ever taken off the prediction path, prediction is refused unless
    in_fight() (net.c:1345-1362, in_fight() = scene_kind() is GS_VS or
    GS_SUDDEN_DEATH at :173), so `n` above zero IS "the sim is in a match"."""
    snaps = [int(n) for n in SNAP_RX.findall(inst.text())]
    # The scene hand-off line is the third witness: a matchmaking run picks its
    # stage through the SSS and may load an archive STAGE_RX does not name.
    vs = len(re.findall(r"net: scene \d+ -> 2 at frame", inst.text()))
    vs += count(inst, r"online: enter VS at frame")  # the matchmaking flow has no scene hand-offs
    return count(inst, SCENE_FILE["match"]) + sum(1 for n in snaps if n > 0) + vs


def wait_match(insts, timeout=240):
    """Block until every instance is simulating a match: its scene is GS_VS
    and the recent recording has a MATCH_RUN-long run of moving checksum.
    The scene alone would also accept an idle match, whose checksum is
    constant; the checksum alone would also accept the opening movie, which
    moves the RNG every frame."""
    deadline = time.time() + timeout

    def simulating(inst):
        return (in_match_scene(inst) > 0 and
                match_entry(record_cks(inst.rec_path, MATCH_WINDOW)) is not None)

    while time.time() < deadline:
        if all(simulating(i) for i in insts):
            return True
        if any(i.proc.poll() is not None for i in insts):
            break
        time.sleep(2)
    for inst in insts:
        print(f"net_test: [{inst.name}] {in_match_scene(inst)} GS_VS markers and "
              f"{match_ratio(inst.rec_path) * 100:.0f}% of the last {MATCH_WINDOW} frames moving:"
              " not a running match", flush=True)
    return False


def check_match(inst):
    """(fails, note) for the summary line. Every row inherits this: a run that
    never entered a match proves datagram plumbing and time sync only, because
    at a menu the checksum carries no game state at all."""
    cks = record_cks(inst.rec_path)
    scene = in_match_scene(inst)
    if len(cks) < MATCH_WINDOW:
        return [f"{len(cks)} recorded frames: too few to tell a match from a menu"], "no match"
    entry = match_entry(cks) if scene else None
    if entry is None:
        m = moved(cks)
        return ([f"never entered a match ({scene} GS_VS markers, "
                 f"{100 * sum(m) / len(m):.0f}% of {len(cks)} frames moved the checksum; "
                 "a menu barely moves it)"], "no match")
    n = sum(moved(cks[entry:]))
    fails = []
    if n < MATCH_MIN:
        fails.append(f"only {n} frames of match after frame {entry} (want {MATCH_MIN})")
    return fails, f"match at frame {entry}, {n} frames simulated"


def check_delay(a, b):
    """Auto delay is decided by one side and applied by frame number, so both
    peers must land every change on the same frame (net_sync.c delay_apply).
    A peer that applies one early or late writes its local sample into a
    different ring slot than the other expects, which is the fairness half of
    a desync and the half a checksum never sees. The menu/fight split makes
    this fire several times a match instead of never, so it is worth pinning."""
    applied = [re.findall(r"net: delay (\d+) -> (\d+) at frame (\d+)", i.text()) for i in (a, b)]
    if applied[0] != applied[1]:
        return [f"peers applied different delays: a {applied[0]} b {applied[1]}"]
    return []


def check_entry(a, b):
    """Both peers must have entered the match on the same frame. Inputs are
    synced and the clocks are locked together, so the scene hand-off is
    frame-exact; this is the frame-stamped stage anchor the direct rows have."""
    e = [match_entry(record_cks(i.rec_path)) for i in (a, b)]
    if None in e:
        return []  # check_match reports that with its own detail
    # A matchmaking session starts its frame count at the match, not at a lobby
    # both peers leave on one frame, so the recorded entry can sit a frame
    # apart; the per-frame checksum compare is what proves the sims agree.
    # (the peers enter the match when their own scenes finish, so Stage 2's
    # start sync, not this check, is what will tighten it)
    slack = 600 if MM_RUN else 0
    return [] if abs(e[0] - e[1]) <= slack else \
        [f"match entered on different frames: a {e[0]} b {e[1]}"]


# "net: frame N, rollbacks N (max depth N, lost N), stalls N (worst X ms), skips N,
# advances N, ping N ms (avg X, min Y, max Z, jitter J), loss L% (...)"; the
# ping parenthesis and loss are optional so older logs still parse.
STATS_RX = re.compile(r"net: frame (\d+), rollbacks (\d+) \(max depth (\d+), lost (\d+)\), "
                      r"stalls (\d+) \(worst ([\d.]+) ms\), skips (\d+), advances (\d+), "
                      r"ping (\d+) ms(?: \(avg [\d.]+, min \d+, max \d+, jitter ([\d.]+)\))?"
                      r"(?:, loss (\d+)%)?")

# The stage archive a match loads, the snapshot report (net_snapshot.c:382).
STAGE_RX = re.compile(r"\[FileCache\] (?:LOOSE )?HIT: (Gr[A-Za-z0-9]+)\.(?:dat|usd)")
OOM_RX = re.compile(r"net: out of memory for snapshots at frame (\d+), lockstep from here")
SNAP_RX = re.compile(r"net:\s+snapshot take [\d.]+ ms \(max [\d.]+, n (\d+)\)")


def check_load_stall(a, b):
    """--load-stall: B's game thread parks mid-run while its tx timer keeps
    sending (MELEE_NET_STALL_TEST, net.c). That is a load -- a stage, a
    character, a first-time shader compile on a phone -- not a lost peer, so
    the link must carry it with no reconnect phase at all. "peer silent" is
    already a failure pattern for every row; what this adds is that the
    session must not even be *interrupted*, because a reconnect that is
    entered on a talking peer is the bug that dropped phone<->PC sessions at
    the CSS->match hand-off (silence was timed from the start of the wait
    rather than from the last datagram).

    The stall must also actually have happened: an injection that silently
    did nothing would pass every other assertion in the row."""
    fails = []
    if "net: stall test:" not in b.text():
        fails.append("b never ran the stall injection (MELEE_NET_STALL_TEST did not fire)")
    for inst in (a, b):
        if "interrupted at frame" in inst.text():
            fails.append(f"{inst.name} opened a reconnect phase for a peer that never "
                         "stopped sending")
    return fails


def check_oom(inst, oom_frame):
    """--oom: the counters, not just the log line. A snapshot that cannot be
    taken disables further prediction while preserving earlier rollback
    snapshots, so snapshots must have been taken before the failure
    and none after it, and the run has to carry on to its exit frame in sync
    (summarize() and check_match() cover that half). Ordering is read off the
    log rather than frame numbers because the FileCache lines carry none."""
    text = inst.text()
    hits = OOM_RX.findall(text)
    if len(hits) != 1:
        return [f"{len(hits)}x 'out of memory for snapshots' (want exactly 1): "
                "MELEE_NET_SIM_OOM_FRAME only fires on a snapshot the engine really takes, "
                "and only a match past the barrier takes any"]
    fails = []
    f = int(hits[0])
    if f < oom_frame:
        fails.append(f"snapshot failed at frame {f}, before MELEE_NET_SIM_OOM_FRAME={oom_frame}")
    lines = text.splitlines()
    oom_at = next(i for i, ln in enumerate(lines) if OOM_RX.search(ln))
    stage_at = next((i for i, ln in enumerate(lines) if STAGE_RX.search(ln)), None)
    if stage_at is None or stage_at > oom_at:
        fails.append("no stage archive loaded before the failure: the snapshot that failed was "
                     "not one from a match")
    takes = [(i, int(m.group(1))) for i, ln in enumerate(lines) for m in [SNAP_RX.search(ln)] if m]
    before = [n for i, n in takes if i < oom_at]
    after = [n for i, n in takes if i > oom_at]
    if not before or max(before) == 0:
        fails.append("no snapshot was taken before the failure: nothing was there to lose")
    if len(after) < 2:
        fails.append(f"{len(after)} snapshot reports after the failure: too short to show "
                     "lockstep")
    elif any(after[1:]):  # the first report still counts takes from before the failure
        fails.append(f"snapshots still taken after the failure (n {after}): the session did not "
                     "drop to lockstep")
    # Snapshot failure must leave earlier rollback snapshots usable. The old
    # INT32_MAX barrier assertion required the very bug this test exercises.
    # No further takes above, and no lost rollback/desync in summarize(),
    # check the fallback's behavior without invalidating that history.
    return fails


def stats_line(inst):
    """(stats dict for tools/net_acceptance.py, one-line summary) from the
    periodic "net: frame" reports."""
    text = inst.text()
    stats = STATS_RX.findall(text)
    line = f"[{inst.name}] "
    st = {}
    if stats:
        s = stats[-1]
        pings = [int(x[8]) for x in stats]
        worst = max(float(x[5]) for x in stats)
        st = {"frame": s[0], "rollbacks": s[1], "max_depth": s[2], "lost": s[3], "stalls": s[4],
              "worst_ms": f"{worst:.1f}", "skips": s[6], "advances": s[7],
              "ping": f"{min(pings)}-{max(pings)}", "jitter": s[9] or "-", "loss": s[10] or "-"}
        line += (f"frame {s[0]}, rollbacks {s[1]} (max depth {s[2]}, lost {s[3]}), stalls {s[4]} "
                 f"(worst {worst:.1f} ms), skips {s[6]}, advances {s[7]}, "
                 f"ping {st['ping']} ms")
        if s[9]:
            line += f", jitter {s[9]} ms"
        if s[10]:
            line += f", loss {s[10]}%"
    else:
        line += "no stats line"
    q = re.findall(r"quality (\d+) \(worst (\d+)\)", text)
    if q:
        st["quality"] = f"{q[-1][0]} (worst {max(int(x[1]) for x in q)})"
        line += f", quality {st['quality']}"
    return st, line


def summarize(inst, need_match=True):
    """(fails, one-line summary, stats dict for tools/net_acceptance.py). The
    match precondition is in here, so every row inherits it and none can pass
    from a menu again. need_match=False is for a row whose flow cannot reach
    a match at all (net_test_n.py --kill): the note still says what the
    checksum stream saw, it just is not a failure."""
    text = inst.text()
    fails = []
    if not re.search(r"net: test done at frame (\d+)", text) and \
            not (MM_RUN and "online: set over" in text):
        fails.append("no 'net: test done'")
    # A session that never came up makes every other number in this row
    # meaningless: the instance plays a normal offline game, its checksum
    # stream moves, check_match() sees a match, and the row reads like a
    # netplay result. The usual cause is the previous run's processes still
    # holding the UDP port, which looks from the outside exactly like two
    # peers that went out of sync.
    if re.search(r"net: bind\(\d+\) failed", text):
        fails.append("netplay never started: bind failed (port still in use?)")
    elif not re.search(r"net: (rollback with |Slippi transport: peer |Slippi match: P)", text):
        fails.append("netplay never started: no session line")
    if inst.proc.returncode != 0 and not (MM_RUN and "online: set over" in text):
        fails.append(f"exit code {inst.proc.returncode}")
    # "peer silent" has to be the whole leaving-netplay line, not a substring:
    # the benign `net: interrupted at frame N (peer silent 3000 ms),
    # reconnecting for up to M ms` that opens the reconnect phase contains the
    # same two words, and matching loosely failed a `--stall` row that had
    # resumed correctly (measured, /tmp/sf_acc "resume 11 s").
    for pat in (r"net: DESYNC", r"peer silent for \d+ ms at frame \d+, leaving netplay",
                r"cannot roll back", r"REPLAY DIVERGED"):
        n = len(re.findall(pat, text))
        if n:
            fails.append(f"{n}x '{pat}'")
    match_fails, note = check_match(inst)
    if need_match:
        fails += match_fails
    st, line = stats_line(inst)
    st["match"] = note
    line += f", {note}"
    d = re.search(r"net: DESYNC.*", text)
    if d:
        line += "\n" + f"[{inst.name}] " + d.group(0)
    return fails, line, st


# STALL_TIMEOUT_MS (net_internal.h) plus room for the loaded machine to
# notice it. 7 s until faf888914 shortened it to 3 s.
PEER_GONE_S = 15


def run_disconnect(args):
    """B SIGKILLed mid-match, no BYE: A must report the peer gone within the
    stall timeout and carry on running instead of hanging. Same return shape
    as run(). MELEE_NET_RECONNECT_MS=0 pins the hard-drop path, MELEE_FPS=1
    gives the per-second line that shows the frame loop is still alive after
    the session is gone (a clean exit by BYE is tools/net_lan_test.py's
    `direct` case; this is the ungraceful half).

    `--disconnect a` kills A instead, i.e. the very instance every assertion
    below is made against. That is the injection this row is validated with:
    it must fail, and fail on "a exited on its own", because a row that
    passes whichever process it kills is not testing a survivor at all."""
    frames = int(args.minutes * 3600) + BOOT_FRAMES
    env = {"MELEE_NET_EXIT_AFTER_FRAMES": str(frames), "MELEE_NET_RECONNECT_MS": "0"}
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    a = Instance("a", args.exe, args.disc, args.work, args.port, args.port + 1, env, False)
    b = Instance("b", args.exe, args.disc, args.work, args.port + 1, args.port, env, False)
    victim = a if args.disconnect == "a" else b
    print(f"net_test: disconnect (killing {victim.name}), pids={a.proc.pid},{b.proc.pid} "
          f"logs={args.work}", flush=True)
    fails = []
    work = None
    try:
        if not drive_direct(a, b):
            fails.append("the drive never reached a match")
        elif not wait_match((a, b)):
            fails.append("no match to interrupt: the checksum stream says neither side was "
                         "simulating one")
        if not fails:
            work = Workout((a, b))  # a real match to interrupt, not two idle fighters
            time.sleep(20)
            entry = [match_entry(record_cks(i.rec_path)) for i in (a, b)]
            print(f"net_test: match entered at frame {entry[0]}/{entry[1]}, "
                  f"{work.done()} key lines played", flush=True)
            work = None
            fps_before = a.text().count("\nfps ")
            stats = STATS_RX.findall(victim.text())
            v_frame = stats[-1][0] if stats else "?"
            victim.kill()  # SIGKILL: no BYE, the survivor has to time the peer out
            t0 = time.time()
            print(f"net_test: SIGKILLed {victim.name} around frame {v_frame}", flush=True)
            if not a.wait_log(r"net: peer silent for \d+ ms at frame \d+, leaving netplay",
                              PEER_GONE_S):
                fails.append(f"a never reported the peer gone within {PEER_GONE_S} s")
            gone = time.time() - t0
            if not a.wait_log(r"net: disconnected at frame \d+ \(status 2\)", 5):
                fails.append("a has no 'net: disconnected ... (status 2)' (PEER_TIMEOUT)")
            time.sleep(10)  # a sane state means it keeps running, not that it survived one frame
            if a.proc.poll() is not None:
                fails.append(f"a exited on its own with code {a.proc.returncode}")
            elif a.text().count("\nfps ") - fps_before < 5:
                fails.append("a stopped printing its per-second fps line: the frame loop hung")
            for pat in ("net: DESYNC", "cannot roll back", "net: peer left"):
                if pat in a.text():
                    fails.append(f"a logged '{pat}'")
            print(f"net_test: a reported the peer gone {gone:.1f} s after the SIGKILL, "
                  f"{a.text().count(chr(10) + 'fps ') - fps_before} fps lines since", flush=True)
    finally:
        if work is not None:
            work.done()
        a.kill()
        b.kill()
    results = []
    for inst, f in ((a, fails), (b, [])):
        st, line = stats_line(inst)
        entry = match_entry(record_cks(inst.rec_path))
        st["match"] = f"match at frame {entry}" if entry is not None else "no match"
        line += f", {st['match']}"
        if inst is victim:
            line += ", SIGKILLed mid-match on purpose"
        print(line)
        if f:
            print(f"[{inst.name}] FAIL: " + ", ".join(f))
        results.append((inst.name, f, line, st))
    return not fails, results


# The reconnect phase (net.c:639-832): a silence past STALL_TIMEOUT_MS opens a
# bounded window instead of ending the session, and the frames predicted
# across the interruption roll back as usual when the peer's input arrives.
# MELEE_NET_RECONNECT_MS bounds the window (default 3 s, 0 = the hard drop
# the `disconnect` row covers). SIGSTOP is the honest interruption to test it
# with: the stopped instance keeps its socket, so the datagrams it missed are
# queued in its receive buffer when it continues, which is exactly what a
# peer whose Wi-Fi came back sees. A killed process cannot resume at all.
STALL_TIMEOUT_S = 3  # STALL_TIMEOUT_MS, src/pc/net_internal.h:114
RESUME_SLACK_S = 25  # room for a loaded machine to notice and report


def stall_expires(stall_s, window_ms):
    """Whether an interruption of `stall_s` outlives the reconnect phase: the
    stall timeout opens the phase and the window bounds it (net.c's
    resume_begin / resume_poll). Its own function because it is the seam the
    row is injection-validated through: patch it to the wrong answer and the
    row must fail, which is the only way to prove the assertions below are
    reading the real window and not just "a long stall kills the session"."""
    return stall_s > STALL_TIMEOUT_S + window_ms / 1000.0


def run_stall(args):
    """--stall SECONDS: B SIGSTOPped mid-match for SECONDS, then SIGCONT.
    Which half of the contract the row asserts follows from the arithmetic,
    not from a second flag: a stall inside STALL_TIMEOUT_S + the reconnect
    window must be survived (interrupted, then resumed, then both peers run
    on to their exit frame in sync), and a stall past it must expire and end
    both sessions with status 2. Same return shape as run().

    Both halves are needed to pin the window down: "survived" alone would
    also pass a build that never times anything out, and "expired" alone
    would also pass a build with no resume phase at all."""
    window_s = args.reconnect_ms / 1000.0
    expire = stall_expires(args.stall, args.reconnect_ms)
    frames = int(args.minutes * 3600) + BOOT_FRAMES
    env = {"MELEE_NET_EXIT_AFTER_FRAMES": str(frames),
           "MELEE_NET_RECONNECT_MS": str(args.reconnect_ms)}
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    a = Instance("a", args.exe, args.disc, args.work, args.port, args.port + 1, env, False)
    b = Instance("b", args.exe, args.disc, args.work, args.port + 1, args.port, env, False)
    print(f"net_test: stall {args.stall} s with a {window_s:.0f} s window -> must "
          f"{'EXPIRE' if expire else 'RESUME'}, pids={a.proc.pid},{b.proc.pid} "
          f"logs={args.work}", flush=True)
    fails = []
    work = None
    stopped = False
    try:
        if not drive_direct(a, b):
            fails.append("the drive never reached a match")
        elif not wait_match((a, b)):
            fails.append("no match to interrupt: the checksum stream says neither side was "
                         "simulating one")
        if not fails:
            work = Workout((a, b))  # a real match to interrupt, not two idle fighters
            time.sleep(20)
            entry = [match_entry(record_cks(i.rec_path)) for i in (a, b)]
            print(f"net_test: match entered at frame {entry[0]}/{entry[1]}, "
                  f"{work.done()} key lines played", flush=True)
            work = None
            fps_before = a.text().count("\nfps ")
            b.proc.send_signal(signal.SIGSTOP)
            stopped = True
            t0 = time.time()
            print(f"net_test: SIGSTOPped b at {time.strftime('%H:%M:%S')}", flush=True)
            # Every wait here is capped by the time left of the stall, and the
            # SIGCONT happens at the stall duration whatever was or was not
            # logged: waiting for the expiry line BEFORE continuing b made the
            # stall as long as the assertion needed, so the expectation became
            # self-fulfilling and the injection that forces the wrong
            # expectation passed (measured, /tmp/sf_stall_expiry: an 11 s stall
            # ran 22.3 s and duly expired).
            def left():
                return max(0.0, args.stall - (time.time() - t0))

            if not a.wait_log(r"net: interrupted at frame \d+ \(peer silent \d+ ms\), "
                              r"reconnecting for up to \d+ ms", left()):
                fails.append(f"a never opened a reconnect phase within {args.stall} s of the "
                             "SIGSTOP")
            else:
                print(f"net_test: a interrupted {time.time() - t0:.1f} s after the SIGSTOP",
                      flush=True)
            if expire and not a.wait_log(r"net: resume window of \d+ ms expired at frame \d+",
                                         left()):
                fails.append(f"a's {window_s:.0f} s resume window never expired inside the "
                             f"{args.stall} s stall")
            time.sleep(left())
            b.proc.send_signal(signal.SIGCONT)
            stopped = False
            print(f"net_test: SIGCONTed b after {time.time() - t0:.1f} s", flush=True)
            if expire:
                # A ended the session on its own; B learns it from A's BYE
                # (net.c:539-545) or, if that datagram was dropped while it
                # was stopped, from its own expiry. Either way: status 2.
                for inst in (a, b):
                    if not inst.wait_log(r"net: disconnected at frame \d+ \(status 2\)",
                                         window_s + RESUME_SLACK_S):
                        fails.append(f"{inst.name} has no 'net: disconnected ... (status 2)' "
                                     "after the window expired")
                if not re.search(r"net: peer silent for \d+ ms at frame \d+, leaving netplay",
                                 a.text()):
                    fails.append("a never logged 'peer silent ... leaving netplay': the expiry "
                                 "did not fall through to the timeout path")
                time.sleep(10)
                if a.proc.poll() is not None:
                    fails.append(f"a exited on its own with code {a.proc.returncode}")
                elif a.text().count("\nfps ") - fps_before < 5:
                    fails.append("a stopped printing its per-second fps line: the frame loop "
                                 "hung after the expiry")
                for inst in (a, b):
                    for pat in ("net: DESYNC", "cannot roll back"):
                        if pat in inst.text():
                            fails.append(f"{inst.name} logged '{pat}'")
            else:
                if not a.wait_log(r"net: resumed at frame \d+ after [\d.]+ s", RESUME_SLACK_S):
                    fails.append(f"a never resumed within {RESUME_SLACK_S} s of the SIGCONT")
                for pat in (r"net: resume window of \d+ ms expired",
                            r"net: disconnected at frame"):
                    if re.search(pat, a.text() + b.text()):
                        fails.append(f"the session ended anyway ('{pat}')")
                # Both peers now have to finish the run they were in the
                # middle of: the resume is only proven by the frames after it.
                deadline = time.time() + frames / 12.0 + 60
                while time.time() < deadline and (a.proc.poll() is None or
                                                  b.proc.poll() is None):
                    time.sleep(1)
                    if any(t in a.text() + b.text() for t in
                           ("net: DESYNC", "net: disconnected", "cannot roll back")):
                        break
        r = re.findall(r"net: resumed at frame (\d+) after ([\d.]+) s", a.text())
        if r:
            print(f"net_test: a resumed at frame {r[-1][0]} after {r[-1][1]} s", flush=True)
    finally:
        if work is not None:
            work.done()
        if stopped:
            b.proc.send_signal(signal.SIGCONT)  # SIGKILL lands either way, but do not
        a.kill(20)                              # leave a stopped melee behind on a crash
        b.kill(20)
    results = []
    for inst in (a, b):
        if expire:  # no self-exit: net.active is false, so nothing counts frames to the end
            st, line = stats_line(inst)
            entry = match_entry(record_cks(inst.rec_path))
            st["match"] = f"match at frame {entry}" if entry is not None else "no match"
            line += f", {st['match']}"
            f = []
        else:
            f, line, st = summarize(inst)
        if inst is a:
            f = f + fails
        print(line)
        if f:
            print(f"[{inst.name}] FAIL: " + ", ".join(f))
        results.append((inst.name, f, line, st))
    return not any(f for _, f, _, _ in results), results


def parse_args(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--loss", type=int, default=0, help="MELEE_NET_SIM_LOSS percent")
    ap.add_argument("--delay", type=int, default=0, help="MELEE_NET_SIM_DELAY_MS one way")
    ap.add_argument("--rxdelay", type=int, default=0, help="MELEE_NET_SIM_DELAY_RX_MS (asymmetric)")
    ap.add_argument("--jitter", action="store_true")
    ap.add_argument("--reorder", action="store_true")
    ap.add_argument("--burst", action="store_true")
    ap.add_argument("--dup", action="store_true")
    ap.add_argument("--minutes", type=float, default=2)
    ap.add_argument("--oom", type=int, default=0, metavar="FRAME",
                    help="MELEE_NET_SIM_OOM_FRAME=FRAME on A (snapshot allocation failure)")
    ap.add_argument("--disconnect", nargs="?", const="b", choices=("a", "b"), default=None,
                    metavar="WHO",
                    help="SIGKILL B mid-match; assert A times the peer out and keeps running. "
                         "'--disconnect a' kills the instance the row asserts on instead: the "
                         "injection that proves the row is not passing on a dead process")
    ap.add_argument("--stall", type=float, default=0, metavar="SECONDS",
                    help="SIGSTOP B mid-match for SECONDS, then SIGCONT: inside the reconnect "
                         "window the session must survive, past it it must expire with status 2")
    ap.add_argument("--combat", action="store_true",
                    help="fighters close in and trade hits (mirrored inputs per side)")
    ap.add_argument("--state-log", action="store_true",
                    help="write per-frame state dumps to <work>/a.state and b.state")
    ap.add_argument("--cold-cache", action="store_true",
                    help="disable archive prewarm on A and enable it on B; delay A's DVD "
                         "reads by 1.5 ms. Loose-file cache reads bypass the DVD delay, "
                         "so this does not guarantee different scene-entry frames")
    ap.add_argument("--load-stall", type=float, default=0, metavar="SECONDS",
                    help="park B's game thread for SECONDS mid-run while its sender keeps "
                         "running (a load, not a lost peer): the session must carry it with no "
                         "reconnect phase, however long it is")
    ap.add_argument("--hitch", default=None, metavar="MS:EVERY",
                    help="MELEE_NET_HITCH_TEST on B: park its game thread for MS every EVERY "
                         "frames of the fight (a phone's frame freezes); A must ride them out")
    ap.add_argument("--cpu", type=int, default=0, metavar="LEVEL",
                    help="the direct match is two CPUs at LEVEL (1-9) fighting")
    ap.add_argument("--stage", type=int, default=None, metavar="STKIND",
                    help="MELEE_DEBUG_VS_STAGE for the direct match (3: Pokemon Stadium, "
                         "whose transformations load mid-match)")
    ap.add_argument("--reconnect-ms", type=int, default=3000,
                    help="MELEE_NET_RECONNECT_MS for --stall (net.c's own default is 3000)")
    ap.add_argument("--fuzz", action="store_true", help="run tools/net_fuzz.py against A")
    ap.add_argument("--fuzz-seconds", type=int, default=30)
    ap.add_argument("--slippi", action="store_true",
                    help="use the Slippi ENet transport (MELEE_NET_STACK=slippi) instead of the "
                         "custom UDP path; direct mode only")
    ap.add_argument("--mm", action="store_true",
                    help="Slippi matchmaking path: boot into the Unranked lobby with a synthesized "
                         "local-peer ticket (MELEE_SLIPPI_LOCAL_PEER) and a fake account, then CSS/SSS")
    ap.add_argument("--mm-server", action="store_true",
                    help="with --mm: queue both instances on a real local matchmaking server "
                         "(build-container/slippi_mm_server) instead of the synthesized ticket")
    ap.add_argument("--mm-decider", choices=("a", "b"), default="a",
                    help="with --mm: which instance the ticket names the decider (a is P1, b is P2)")
    ap.add_argument("--exe", default=os.path.join(here, "..", "build", "melee"))
    ap.add_argument("--disc", default=os.path.join(here, "..", "..", "melee.ciso"))
    ap.add_argument("--port", type=int, default=42050, help="A's UDP port; B uses +1")
    ap.add_argument("--work", default="/tmp/net_test")
    ap.add_argument("--env-a", action="append", default=[], metavar="VAR=VALUE",
                    help="an extra environment variable for A alone (repeatable), e.g. AURORA_XR=1 "
                         "to run one side in XR")
    ap.add_argument("--env-b", action="append", default=[], metavar="VAR=VALUE",
                    help="the same for B alone")
    return ap.parse_args(argv)


def run(args):
    """One two-instance run. Returns (ok, [(name, fails, line, stats), ...])."""
    here = os.path.dirname(os.path.abspath(__file__))
    if args.disconnect:
        return run_disconnect(args)
    if args.stall:
        return run_stall(args)
    global MM_RUN
    if args.mm_server:
        args.mm = True
    MM_RUN = bool(args.mm)
    # A matchmaking session counts frames from the match start, so there is no
    # boot to budget for; and under Slippi's rules (4 stocks) a match can end
    # in about 4000 frames.
    frames = int(args.minutes * 3600) + (0 if args.mm else BOOT_FRAMES)
    sim = {"MELEE_NET_EXIT_AFTER_FRAMES": str(frames)}
    if args.loss:
        sim["MELEE_NET_SIM_LOSS"] = str(args.loss)
    if args.delay:
        sim["MELEE_NET_SIM_DELAY_MS"] = str(args.delay)
    if args.rxdelay:
        sim["MELEE_NET_SIM_DELAY_RX_MS"] = str(args.rxdelay)
    for flag, (var, val) in SIM_ENV.items():
        if getattr(args, flag):
            sim[var] = val

    sim_a = dict(sim)
    if args.oom:
        sim_a["MELEE_NET_SIM_OOM_FRAME"] = str(args.oom)
    if args.cold_cache:
        # Exercise prewarm asymmetry, even if the shell disabled it globally.
        # OS caches remain warm; loose-file reads bypass the DVD delay below.
        # Verify actual scene transitions instead of assuming different timing.
        sim_a["MELEE_PREWARM"] = "0"
        sim["MELEE_PREWARM"] = "1"
        # Aurora's DVD readFromHandle hook delays reads that reach that layer.
        sim_a["MELEE_DISC_READ_DELAY_US"] = "1500"
    if args.load_stall:
        # B only, and well past boot so the session is established: the wait
        # this exercises is the one a scene hand-off makes, not the connect.
        sim["MELEE_NET_STALL_TEST"] = f"{LOAD_STALL_FRAME}:{int(args.load_stall * 1000)}"
    if args.hitch:
        sim["MELEE_NET_HITCH_TEST"] = args.hitch  # B only: sim_a was copied above
    if args.cpu:
        # Two CPUs at this level fight while the workout keeps the pads (and
        # so the predictions and rollbacks) moving underneath them.
        sim_a["MELEE_DEBUG_VS"] = sim["MELEE_DEBUG_VS"] = "cpu2"
        sim_a["MELEE_DEBUG_VS_CPU_LEVEL"] = sim["MELEE_DEBUG_VS_CPU_LEVEL"] = str(args.cpu)
    if args.stage is not None:
        sim_a["MELEE_DEBUG_VS_STAGE"] = sim["MELEE_DEBUG_VS_STAGE"] = str(args.stage)
        # A frozen Stadium (the launcher default on many machines) never
        # transforms, which is the load this option exists to reach.
        sim_a["MELEE_FROZEN_STADIUM"] = sim["MELEE_FROZEN_STADIUM"] = "0"
    for side, extra in ((sim_a, args.env_a), (sim, args.env_b)):
        for kv in extra:
            var, sep, val = kv.partition("=")
            if not sep or not var:
                raise SystemExit(f"--env-a/--env-b want VAR=VALUE, not {kv!r}")
            side[var] = val
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    mm_server = None
    if args.mm_server:
        args.mm = True
        exe = os.path.join(os.path.dirname(os.path.abspath(args.exe)), "slippi_mm_server")
        mm_port = args.port + 100
        mm_server = subprocess.Popen([exe, "-p", str(mm_port), "-v"],
                                     stdout=open(os.path.join(args.work, "mm_server.log"), "w"),
                                     stderr=subprocess.STDOUT)
        time.sleep(0.5)
    if args.mm:
        for name, env, port, peer in (("a", sim_a, args.port, args.port + 1),
                                      ("b", sim, args.port + 1, args.port)):
            slp_dir = os.path.join(args.work, f"slippi-{name}")
            os.makedirs(slp_dir)
            with open(os.path.join(slp_dir, "slippi-account.json"), "w") as f:
                json.dump({"uid": f"test{name}", "playKey": "k", "displayName": f"Test {name}",
                           "connectCode": f"TST#{1 if name == 'a' else 2}"}, f)
            env.update({
                "MELEE_NET_STACK": "slippi",
                "MELEE_BOOT_SCENE": "unranked",
                "MELEE_SLIPPI_DIR": slp_dir,
                "MELEE_SLIPPI_LOCAL_PORT": str(port),
                "MELEE_CSS_PICK": "2,0" if name == "a" else "9,1",  # Fox vs Marth
            })
            if mm_server is not None:
                env.update({"MELEE_SLIPPI_MM_HOST": "127.0.0.1",
                            "MELEE_SLIPPI_MM_PORT": str(mm_port)})
            else:
                env.update({
                    "MELEE_SLIPPI_LOCAL_PEER": "1",
                    "MELEE_SLIPPI_PEER_PORT": str(peer),
                    "MELEE_SLIPPI_LOCAL_INDEX": "0" if name == "a" else "1",
                    "MELEE_SLIPPI_LOCAL_HOST": "1" if name == args.mm_decider else "0",
                })
    if args.state_log:
        # Per-frame state dumps beside the logs, so a DESYNC can be diffed
        # field by field (src/pc/net_snapshot.c).
        sim_a["MELEE_NET_STATE_LOG"] = os.path.join(args.work, "a.state")
        sim["MELEE_NET_STATE_LOG"] = os.path.join(args.work, "b.state")
    wait_port_free(args.port)
    wait_port_free(args.port + 1)
    a = Instance("a", args.exe, args.disc, args.work, args.port, args.port + 1, sim_a, bool(args.mm),
                 args.slippi)
    b = Instance("b", args.exe, args.disc, args.work, args.port + 1, args.port, sim, bool(args.mm),
                 args.slippi)
    print(f"net_test: {'mm' if args.mm else 'direct'} loss={args.loss}% delay={args.delay}ms "
          f"rxdelay={args.rxdelay}ms {' '.join(k for k in SIM_ENV if getattr(args, k))} "
          f"minutes={args.minutes} frames={frames} pids={a.proc.pid},{b.proc.pid} "
          f"logs={args.work}", flush=True)
    ok = True
    work = None
    try:
        if args.mm:
            ok = drive_mm(a, b)
            if not ok:
                print("net_test: matchmaking drive failed (see logs)", flush=True)
        else:
            ok = drive_direct(a, b)
            if not ok:
                print("net_test: never got into the match (see logs)", flush=True)
        # Every row, not just the new ones: a run that never entered a match
        # measures the transport at a menu, which is how this matrix used to
        # pass green from the title screen.
        if ok:
            ok = wait_match((a, b))
        if ok:
            work = Workout((a, b), args.combat)  # keep both players moving for the rest of the run
        if ok and args.fuzz:
            sys.path.insert(0, here)
            import net_fuzz
            time.sleep(5)  # into the match first
            f = net_fuzz.run(args.port, a.proc.pid, a.log_path, args.fuzz_seconds)
            print(f"net_test: fuzz {'pass' if f else 'FAIL'}", flush=True)
            ok = ok and f
        # net.c self-exits at `frames`; this is a safety net. Slow links and a
        # loaded machine run well below 60 fps, so budget by frame progress
        # (assume a floor of 12 fps) rather than wall-clock minutes. A run
        # that stops advancing its "net: frame N" for 25 s is stuck (a
        # disconnect leaves the title looping but never self-exits), and a
        # terminal marker means it already failed: kill early either way.
        deadline = time.time() + frames / 12.0 + 60
        # Terminal markers only. A reconnect phase that OPENS is not terminal
        # (it may resume), so this matches the leaving-netplay line rather
        # than the two words "peer silent", which the interrupted line shares:
        # a 7 s hiccup on a loaded machine must not end a 60-minute soak.
        term = (r"net: DESYNC", r"peer silent for \d+ ms at frame \d+, leaving netplay",
                r"net: disconnected", r"cannot roll back")
        # A deliberate load stall is exactly "no frame progress", so the
        # watchdog has to outlast it or the row kills the run it is measuring.
        quiet_budget = 25 + args.load_stall
        last_frame, last_progress = -1, time.time()
        while time.time() < deadline and (a.proc.poll() is None or b.proc.poll() is None):
            time.sleep(1)
            texts = a.text() + b.text()
            if any(re.search(t, texts) for t in term):
                break  # let summarize() report it; do not burn the whole budget
            if MM_RUN and "online: set over" in a.text() and \
                    "online: set over" in b.text():
                break  # the match ended on its own, which is a complete run
            fr = max([int(x) for x in re.findall(r"net: frame (\d+)", texts)] or [-1])
            if fr > last_frame:
                last_frame, last_progress = fr, time.time()
            elif time.time() - last_progress > quiet_budget and last_frame >= 0:
                print(f"net_test: no frame progress for {quiet_budget:.0f} s, giving up",
                      flush=True)
                break
    finally:
        if work is not None:
            print(f"net_test: workout wrote {work.done()} key lines", flush=True)
        # An instance that reached its exit frame (or took its peer's BYE that
        # close to it) is already tearing the GPU down, which takes seconds;
        # SIGKILL only what is still running after that.
        a.kill(20)
        b.kill(20)
        if mm_server is not None:
            mm_server.terminate()
        if a.proc.returncode == -9 or b.proc.returncode == -9:
            print("net_test: killed a run that would not exit (per-instance FAIL below)",
                  flush=True)
    # Cross-instance assertions belong to neither log; they ride on A's row.
    extra = check_entry(a, b) + check_delay(a, b)
    extra += check_oom(a, args.oom) if args.oom else []
    if args.load_stall:
        extra += check_load_stall(a, b)
    if args.hitch:
        n = count(b, r"net: hitch test:")
        print(f"net_test: b injected {n} hitches of {args.hitch}", flush=True)
        if n == 0:
            extra.append("b never ran the hitch injection (MELEE_NET_HITCH_TEST did not fire)")
    results = []
    for inst in (a, b):
        fails, line, st = summarize(inst, need_match=True)
        if inst is a:
            fails = fails + extra
        print(line)
        if fails:
            ok = False
            print(f"[{inst.name}] FAIL: " + ", ".join(fails))
        results.append((inst.name, fails, line, st))
    return ok, results


def main():
    ok, _ = run(parse_args())
    print("net_test: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
