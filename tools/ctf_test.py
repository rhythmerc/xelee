#!/usr/bin/env python3
"""Capture the Flag on Hyrule Temple, by frame script on port 1, unpaced
(docs/xeleenet-modes-plan.md M1, M2). Each scenario boots a debug VS match and
reads the game's "ctf:" log lines.

    DISPLAY=:0 tools/ctf_test.py [--exe build/linux/melee] [--disc ...] [scenario ...]

The flag item (M1; one stray flag beside player 1, no CTF rules):
  carry      pick up with A; B while holding does nothing; walk/dash slow to 0.75
             while carrying and come back after the throw; it lands and stays.
  drop       a CPU Mario hits the carrier until the flag shakes loose.

The rules (M2; MELEE_DEBUG_CTF=1). Most put both stands on Temple's upper
floor (red -62, blue -30) so the script's runs are short; P1 is red.
  capture    dash to blue's flag, pick it up, run home: a carried capture.
  throw      the same, but throw it home: a thrown capture.
  gate       A beside red's own flag on its stand does not pick it up.
  own        red's own flag lying away from home goes home when P1 picks it up.
  timer      a free flag goes home when its return timer runs out.
  blast      a flag past the blast zone is put back on its stand.
  needhome   no capture while red's own flag is away from home.
  nohome     with "flag must be home" off, the same carry scores.
  win        first to 1: the capture ends the match, red wins.
  tie        a 1:00 match with no captures ends as no contest.
  start      on Temple's real layout P1 starts in red's cave.
  respawn    P1 walks off the cave's edge, is KO'd, and is reborn in the cave.
  swap       with the sides swapped, P1 (red) starts at the right edge instead.
  throwred   on the real layout, blue's flag picked up on the cave's tunnel and
             thrown back from the slope (about 39 units out) scores for red.
  throwblue  the same for the right edge (sides swapped): thrown from the main
             floor's left end (about 46 units out).
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# Link's first special motion id: anything at or above it is a special move.
SPECIAL_MOTION_MIN = 341
# A carrier's walk and dash, as a fraction (CTF_FLAG_CARRY_SPEED, itctfflag.h).
CARRY_SPEED = 0.75
NEAR = {"MELEE_DEBUG_CTF": "1", "MELEE_DEBUG_CTF_BASES": "-62,22.5,-30,22.5"}
GO = "until ctf: f170 p1 300f\n"  # inputs before GO! are ignored

CARRY = """until ctf: flag landed 1800f
wait 170f
tap X 3f
wait 20f
mark held
tap Z 3f
wait 40f
mark afterB
X+Right 3f
wait 90f
mark thrown
tap Z 3f
wait 40f
mark done
"""

DROP = """until ctf: flag landed 1800f
wait 170f
tap X 3f
wait 20f
mark held
until ctf: flag dropped 4000f
mark done
"""

# P1 dashes from x -70 to -40 (the idle blue player stops him), which is in
# reach of blue's flag at -30; then heads home (red's zone is -77 to -47).
FETCH = GO + "Right 22f\nwait 30f\ntap X 3f\nwait 25f\nmark held\n"
CAPTURE = FETCH + "Left 30f\nwait 40f\nmark done\n"
THROW = FETCH + "X+Left 3f\nwait 80f\nmark done\n"
TAP_A = GO + "tap X 3f\nwait 40f\nmark done\n"
WAIT = "until ctf: f300 p1 600f\nmark done\n"
SD = GO + "Left 60f\nwait 400f\nmark done\n"
# The real layout's throws home. Red: P1 dashes out of the cave to blue's
# flag on the tunnel floor (-58), carries it down the slope to about x -36,
# and throws it back left into the cave. Blue (sides swapped, so P1 defends
# the right edge): P1 dashes left to blue's flag near the main floor's left
# end (x 120) and throws it right from about x 123.
THROW_RED = GO + ("Right 40f\nwait 30f\ntap X 3f\nwait 25f\nmark held\nRight 30f\nwait 30f\n"
                  "X+Left 3f\nwait 100f\nmark done\n")
THROW_BLUE = GO + "Left 40f\nwait 30f\ntap X 3f\nwait 25f\nmark held\nX+Right 3f\nwait 100f\nmark done\n"


def run(args, name, script, extra_env):
    work = tempfile.mkdtemp(prefix=f"ctf_{name}_")
    fifo = os.path.join(work, "keys")
    os.mkfifo(fifo)
    out = os.path.join(work, "out.txt")
    env = dict(os.environ, MELEE_KEY_FIFO=fifo, MELEE_BOOT_SCENE="vs", MELEE_DEBUG_VS_STAGE="14",
               MELEE_DEBUG_CTF_LOG="1", MELEE_NO_CARD="1",
               MELEE_UNPACED="1", MELEE_VSYNC="0", MELEE_DRAW_EVERY="10",
               MELEE_WINDOW_TITLE=f"melee-ctf-test-{name}", MELEE_LOG_FILE=os.path.join(work, "log.txt"))
    for k in [k for k in env if k.startswith("MELEE_DEBUG_CTF") and k != "MELEE_DEBUG_CTF_LOG"]:
        env.pop(k)
    env.pop("MELEE_DEBUG_VS", None)
    env.update(extra_env)
    t0 = time.time()
    with open(out, "w") as f:
        proc = subprocess.Popen([args.exe, "--no-card", args.disc], env=env, stdout=f, stderr=subprocess.STDOUT)
    try:
        with open(fifo, "w") as f:
            f.write(script)
        deadline = time.time() + args.timeout
        while time.time() < deadline and proc.poll() is None:
            time.sleep(0.2)
            text = open(out, errors="replace").read()
            if "script: P1 mark done" in text or "script: P1 timeout" in text:
                break
    finally:
        proc.kill()
        proc.wait()
    text = open(out, errors="replace").read()
    print(f"{name}: {time.time() - t0:.1f} s, log {out}")
    return text


def frames_between(text, a, b):
    """The p1 log lines between two script marks."""
    seg = text.split(f"script: P1 mark {a}", 1)[-1].split(f"script: P1 mark {b}", 1)[0]
    return [tuple(map(float, m)) for m in
            re.findall(r"ctf: f\d+ p1 \S+ \S+ holding=(\d) motion=(\d+) walk=(\S+) dash=(\S+)", seg)]


def p1_positions(text):
    return [(int(f), float(x), float(y), int(m)) for f, x, y, m in
            re.findall(r"ctf: f(\d+) p1 (\S+) (\S+) holding=\d motion=(\d+)", text)]


def check_carry(text):
    fails = []
    for want in ["ctf: flag picked up", "ctf: flag thrown", "script: P1 mark done"]:
        if want not in text:
            fails.append(f"no '{want}'")
    held = frames_between(text, "held", "afterB")
    if not held or not all(h[0] == 1 for h in held):
        fails.append("not holding the flag after A")
    if any(h[1] >= SPECIAL_MOTION_MIN for h in held):
        fails.append("a special came out while carrying")
    after = frames_between(text, "thrown", "done")
    if not any(h[1] >= SPECIAL_MOTION_MIN for h in after):
        fails.append("B after the throw did not bring out a special")
    if held and after:
        walk_ratio = held[-1][2] / after[-1][2]
        dash_ratio = held[-1][3] / after[-1][3]
        if abs(walk_ratio - CARRY_SPEED) > 0.01 or abs(dash_ratio - CARRY_SPEED) > 0.01:
            fails.append(f"carry speed ratios walk {walk_ratio:.3f} dash {dash_ratio:.3f}, want {CARRY_SPEED}")
    if "ctf: flag landed" not in text.split("ctf: flag thrown", 1)[-1]:
        fails.append("the thrown flag never landed")
    if "ctf: flag destroyed" in text:
        fails.append("the flag was destroyed")
    return fails


def check_drop(text):
    fails = []
    if "ctf: flag picked up" not in text:
        fails.append("never picked up")
    if "script: P1 saw \"ctf: flag dropped\"" not in text:
        fails.append("the CPU never knocked the flag loose")
    return fails


def expect(*want, forbid=(), done=True):
    """A check: every regex in want appears, none in forbid."""
    def check(text):
        fails = []
        if done and "script: P1 mark done" not in text:
            fails.append("the script never finished")
        for w in want:
            if not re.search(w, text):
                fails.append(f"no /{w}/")
        for f in forbid:
            if re.search(f, text):
                fails.append(f"unexpected /{f}/")
        return fails
    return check


def check_start(text):
    pos = p1_positions(text)
    first = next(((x, y) for f, x, y, m in pos if f >= 60), None)
    if first is None:
        return ["no p1 positions"]
    if not (-135 < first[0] < -74 and -90 < first[1] < -50):
        return [f"P1 at {first} at frame 60, not in red's cave"]
    return []


def check_respawn(text):
    pos = p1_positions(text)
    reborn = [(f, x, y) for f, x, y, m in pos if m == 13]  # RebirthWait, on the platform
    if not any(y < -200 for f, x, y, m in pos):
        return ["P1 never fell out"]
    if not reborn:
        return ["P1 was never reborn"]
    f, x, y = reborn[0]
    if abs(x + 125) > 1 or abs(y + 70) > 1:
        return [f"reborn at {x:.1f} {y:.1f}, not red's spawn (-125, -70)"]
    landed = [(x, y) for ff, x, y, m in pos if ff > f + 300]
    if landed and not (-90 < landed[-1][1] < -85):
        return [f"after rebirth P1 ended at {landed[-1]}, not on the cave floor"]
    return []


def check_swap(text):
    pos = p1_positions(text)
    first = next(((x, y) for f, x, y, m in pos if f >= 60), None)
    if first is None:
        return ["no p1 positions"]
    if not (165 < first[0] < 226 and -6 < first[1] < 30):
        return [f"P1 at {first} at frame 60, not at the right edge"]
    return []


SCENARIOS = {
    "carry": (CARRY, {"MELEE_DEBUG_CTF_FLAG": "-90,30"}, check_carry),
    "drop": (DROP, {"MELEE_DEBUG_CTF_FLAG": "-90,30", "MELEE_DEBUG_VS": "cpu"}, check_drop),
    "capture": (CAPTURE, NEAR, expect(r"capture red \(carried\) red 1 blue 0", r"blue flag home \(captured\)")),
    "throw": (THROW, NEAR, expect(r"capture red \(thrown\) red 1 blue 0")),
    "gate": (TAP_A, dict(NEAR, MELEE_DEBUG_CTF_BASES="-66,22.5,-30,22.5"),
             expect(forbid=[r"ctf: flag picked up"])),
    "own": (TAP_A, dict(NEAR, MELEE_DEBUG_CTF_FREE="0,-66,30"),
            expect(r"ctf: flag picked up", r"red flag home \(returned by its team\)")),
    "timer": (WAIT, dict(NEAR, MELEE_DEBUG_CTF_FREE="0,-55,30", MELEE_DEBUG_CTF_RETURN="120"),
              expect(r"ctf: f12[0-9] red flag home \(timer\)")),
    "blast": (WAIT, dict(NEAR, MELEE_DEBUG_CTF_FREE="1,0,-300"),
              expect(r"ctf: f[2-9] blue flag home \(respawn\)")),
    "needhome": (CAPTURE, dict(NEAR, MELEE_DEBUG_CTF_FREE="0,-120,30", MELEE_DEBUG_CTF_RETURN="0"),
                 expect(r"ctf: flag picked up", forbid=[r"capture red"])),
    "nohome": (CAPTURE, dict(NEAR, MELEE_DEBUG_CTF_FREE="0,-120,30", MELEE_DEBUG_CTF_RETURN="0",
                             MELEE_DEBUG_CTF_HOME="0"),
               expect(r"capture red \(carried\)")),
    "win": (CAPTURE.replace("mark done", "wait 300f\nmark done"), dict(NEAR, MELEE_DEBUG_CTF_WIN="1"),
            expect(r"red wins", r"boot scene: state 3")),
    "tie": ("wait 9000f\nmark done\n", {"MELEE_DEBUG_CTF": "1", "MELEE_DEBUG_VS_TIME": "1"},
            expect(r"ctf: time up tied, no contest", done=False)),
    "start": (WAIT, {"MELEE_DEBUG_CTF": "1"}, check_start),
    "respawn": (SD, {"MELEE_DEBUG_CTF": "1"}, check_respawn),
    "swap": (WAIT, {"MELEE_DEBUG_CTF": "1", "MELEE_DEBUG_CTF_SWAP": "1"}, check_swap),
    "throwred": (THROW_RED, {"MELEE_DEBUG_CTF": "1", "MELEE_DEBUG_CTF_FREE": "1,-58,-80"},
                 expect(r"ctf: flag picked up", r"capture red \(thrown\) red 1 blue 0")),
    "throwblue": (THROW_BLUE, {"MELEE_DEBUG_CTF": "1", "MELEE_DEBUG_CTF_SWAP": "1", "MELEE_DEBUG_CTF_FREE": "1,120,0"},
                  expect(r"ctf: flag picked up", r"capture red \(thrown\) red 1 blue 0")),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "build", "linux", "melee"))
    ap.add_argument("--disc", default=os.path.expanduser(
        "~/Downloads/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso"))
    ap.add_argument("--timeout", type=float, default=150)
    ap.add_argument("scenario", nargs="*", default=list(SCENARIOS))
    args = ap.parse_args()
    bad = []
    for name in args.scenario:
        script, env, check = SCENARIOS[name]
        fails = check(run(args, name, script, env))
        print(f"{name}: {'PASS' if not fails else 'FAIL: ' + '; '.join(fails)}", flush=True)
        if fails:
            bad.append(name)
    print(f"{len(args.scenario) - len(bad)}/{len(args.scenario)} passed" + (f"; failed: {' '.join(bad)}" if bad else ""))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
