#!/usr/bin/env python3
"""The key FIFO's frame scripts (docs/fast-tests-plan.md T2), end to end: one
instance boots uncapped (MELEE_UNPACED=1, MELEE_VSYNC=0), a script waits for
the title, taps Start into the main menu and moves its cursor, and every
command logs the frame it ran on. Two runs must agree frame for frame.

    tools/script_smoke.py [--exe build/linux/melee] [--disc ...] [--runs 2]
                          [--work DIR]

Pass = both runs reach the end mark, and the frames of every "script:" line
are the same in each.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = """tapuntil Return 9f 60f HIT: MnMaAll
wait 30f
mark menu
tap Down 4f
tap Down 4f
X 7f
wait 20f
mark done
"""


def run_once(args, k):
    work = os.path.join(args.work, f"run{k}")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    fifo = os.path.join(work, "keys")
    os.mkfifo(fifo)
    log_path = os.path.join(work, "log.txt")
    env = dict(os.environ, MELEE_KEY_FIFO=fifo, MELEE_UNPACED="1", MELEE_VSYNC="0", MELEE_INPUT_TRACE="1",
               MELEE_WINDOW_TITLE=f"melee-script-smoke-{k}", MELEE_LOG_FILE=log_path)
    t0 = time.time()
    with open(os.path.join(work, "stderr.txt"), "w") as err:
        proc = subprocess.Popen([args.exe, "--no-card", args.disc], env=env, stdout=err, stderr=subprocess.STDOUT)
    try:
        with open(fifo, "w") as f:  # blocks until the game opens it
            f.write(SCRIPT)
        deadline = time.time() + args.timeout
        text = ""
        while time.time() < deadline and proc.poll() is None:
            time.sleep(0.2)
            text = open(os.path.join(work, "stderr.txt"), errors="replace").read()
            if "script: P1 mark done" in text or "script: P1 timeout" in text:
                break
    finally:
        proc.kill()
        proc.wait()
    wall = time.time() - t0
    lines = re.findall(r"script: P1 (.*) (?:at|from) frame (\d+)", text)
    print(f"run {k}: {wall:.1f} s wall, " + ", ".join(f"{what} @{n}" for what, n in lines), flush=True)
    return text, [(w, int(n)) for w, n in lines]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "build", "linux", "melee"))
    ap.add_argument("--disc", default=os.path.expanduser(
        "~/Downloads/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso"))
    ap.add_argument("--runs", type=int, default=2)
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--work", default=os.path.join(HERE, "..", "build", "script_smoke"))
    args = ap.parse_args()
    results = [run_once(args, k) for k in range(args.runs)]
    ok = True
    for k, (text, lines) in enumerate(results):
        if "script: P1 mark done" not in text:
            print(f"FAIL: run {k} never reached the end mark")
            ok = False
    if ok and len({tuple(r[1]) for r in results}) != 1:
        print("FAIL: the runs' script frames differ")
        ok = False
    print("script_smoke: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
