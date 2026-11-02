#!/usr/bin/env python3
"""Capture the Flag's flag item on Hyrule Temple, by frame script on port 1,
unpaced (docs/xeleenet-modes-plan.md M1). Each scenario boots a debug VS
match with one flag beside player 1 and reads the game's "ctf:" log lines.

    DISPLAY=:0 tools/ctf_test.py [--exe build/linux/melee] [--disc ...] [scenario ...]

Scenarios:
  carry   pick up with A; B while holding does nothing; walk/dash slow to 0.8
          while carrying and come back after the throw; it lands and stays.
  drop    a CPU Mario hits the carrier until the flag shakes loose.
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


def run(args, name, script, extra_env):
    work = tempfile.mkdtemp(prefix=f"ctf_{name}_")
    fifo = os.path.join(work, "keys")
    os.mkfifo(fifo)
    out = os.path.join(work, "out.txt")
    env = dict(os.environ, MELEE_KEY_FIFO=fifo, MELEE_BOOT_SCENE="vs", MELEE_DEBUG_VS_STAGE="14",
               MELEE_DEBUG_CTF_FLAG="-90,30", MELEE_DEBUG_CTF_LOG="1", MELEE_NO_CARD="1",
               MELEE_UNPACED="1", MELEE_VSYNC="0", MELEE_DRAW_EVERY="10",
               MELEE_WINDOW_TITLE=f"melee-ctf-test-{name}", MELEE_LOG_FILE=os.path.join(work, "log.txt"))
    env.pop("MELEE_DEBUG_VS", None)
    env.update(extra_env)
    t0 = time.time()
    with open(out, "w") as f:
        proc = subprocess.Popen([args.exe, "--no-card", args.disc], env=env, stdout=f, stderr=subprocess.STDOUT)
    text = ""
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
        if abs(walk_ratio - 0.8) > 0.01 or abs(dash_ratio - 0.8) > 0.01:
            fails.append(f"carry speed ratios walk {walk_ratio:.3f} dash {dash_ratio:.3f}, want 0.8")
    landed = text.split("ctf: flag thrown", 1)[-1]
    if "ctf: flag landed" not in landed:
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


SCENARIOS = {
    "carry": (CARRY, {}, check_carry),
    "drop": (DROP, {"MELEE_DEBUG_VS": "cpu"}, check_drop),
}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "build", "linux", "melee"))
    ap.add_argument("--disc", default=os.path.expanduser(
        "~/Downloads/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso"))
    ap.add_argument("--timeout", type=float, default=150)
    ap.add_argument("scenario", nargs="*", default=list(SCENARIOS))
    args = ap.parse_args()
    bad = 0
    for name in args.scenario:
        script, env, check = SCENARIOS[name]
        fails = check(run(args, name, script, env))
        print(f"{name}: {'PASS' if not fails else 'FAIL: ' + '; '.join(fails)}", flush=True)
        bad += bool(fails)
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
