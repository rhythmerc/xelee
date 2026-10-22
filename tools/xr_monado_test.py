#!/usr/bin/env python3
"""OpenXR presentation tests on a desktop, against Monado, without a headset.

    python3 tools/xr_monado_test.py                  # every case
    python3 tools/xr_monado_test.py --case frame-sim
    python3 tools/xr_monado_test.py --list

Each case starts a fresh monado-service (null compositor: no window, 20 Hz),
runs the XR build of the game (MELEE_BIN, AURORA_XR=1) into a scene reached
with MELEE_BOOT_SCENE and bounded by MELEE_EXIT_AFTER_FRAMES, and judges the
exit status, the game's log, the frame dumps (AURORA_XR_DUMP) and, where a
case loads it, the summary of aurora's test layer (extern/aurora/tests/
xr_sim_layer, XR_APILAYER_AURORA_frame_sim), which gives Monado the Steam
Frame's SteamVR personality: alpha-blend passthrough, its controller profile,
16 swapchains, a refresh rate reported or not. See docs/testing.md.

Needs: a disc image (MELEE_DISC), the XR build (MELEE_BIN, default
build/linux-xr/melee, configured with -DAURORA_ENABLE_OPENXR=ON), Monado
(MONADO_SERVICE and XR_RUNTIME_JSON, or MONADO=<prefix>), Vulkan's validation
layer for the queue cases (VK_LAYER_PATH), and a display: the real one, or
--headless for a Weston of its own (headless backend), so nothing shows.

stdlib only, like tools/smoke_test.py.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
MELEE = Path(os.environ.get("MELEE_BIN") or ROOT / "build" / "linux-xr" / "melee")
# The Steam Frame's build configured for this machine (-DMELEE_STEAM_FRAME=ON),
# for the cases about its launcher; skipped without one.
FRAME_MELEE = Path(os.environ.get("MELEE_FRAME_BIN") or ROOT / "build" / "linux-frame-x86" / "melee")
LAYER_DIR = Path(os.environ.get("XR_SIM_LAYER_DIR") or MELEE.parent / "extern" / "aurora" / "tests" / "xr_sim_layer")

FIGHT = {"MELEE_BOOT_SCENE": "vs", "MELEE_DEBUG_VS": "cpu4", "MELEE_XR_PLACE": "0", "MELEE_SEED": "1",
         "AURORA_XR_ARENA_AT_HEAD": "1"}
SIM = {"XR_API_LAYER_PATH": str(LAYER_DIR), "XR_ENABLE_API_LAYERS": "XR_APILAYER_AURORA_frame_sim"}
FRAME = dict(SIM, AURORA_SIM_BLEND="alpha", AURORA_SIM_FRAME="1", AURORA_SIM_HANDS="1",
             AURORA_SIM_MAX_SWAPCHAINS="16")
VALIDATION = {"VK_INSTANCE_LAYERS": "VK_LAYER_KHRONOS_validation",
              # Thread safety only: core validation would bury it and crawl.
              "VK_KHRONOS_VALIDATION_VALIDATE_CORE": "false", "VK_KHRONOS_VALIDATION_STATELESS_PARAM": "false",
              "VK_KHRONOS_VALIDATION_OBJECT_LIFETIME": "false", "VK_KHRONOS_VALIDATION_THREAD_SAFETY": "true"}

# A case: environment, frames, then what its log (and stderr) must and must not
# say, and checks on the run (a function of the run directory, returning a
# failure message or None).


def dump_coverage(lo, hi):
    """The 3D dump's alpha: share of covered pixels within [lo, hi]."""
    def check(run):
        pgm = run / "dump" / "xr_3d_alpha.pgm"
        if not pgm.exists():
            return "no 3D dump"
        data = pgm.read_bytes()
        # P5\n<w> <h>\n255\n<pixels>
        header = data.split(b"\n", 3)
        pixels = header[3]
        covered = sum(1 for b in pixels if b > 8) / max(1, len(pixels))
        if not lo <= covered <= hi:
            return "3D coverage %.1f%%, expected %.0f-%.0f%%" % (covered * 100, lo * 100, hi * 100)
        return None
    return check


def sim_summary(pattern, message):
    def check(run):
        text = (run / "stdout.log").read_text(errors="replace")
        m = re.search(r"frame_sim: (\d+) frames; (\d+) blended, (\d+) of them without a projection layer first; "
                      r"swapchains at most (\d+) alive, (\d+) refused; Frame bindings accepted (\d+), refused (\d+)",
                      text)
        if not m:
            return "no frame_sim summary"
        frames, blended, unprojected, peak, refused, accepted, rejected = map(int, m.groups())
        return None if pattern(frames, blended, unprojected, peak, refused, accepted, rejected) else message
    return check


CASES = {
    "shared-queue": dict(
        what="The runtime shares Dawn's one queue (RADV, Turnip) and Vulkan's validation layer sees no race",
        env=dict(FIGHT, **VALIDATION), frames=1500,
        need=[r"OpenXR queue: (Dawn's queue 0 of family \d+, shared with Dawn|a spare one)", r"3D eyes render"],
        forbid=[r"THREADING ERROR"], checks=[dump_coverage(0.02, 0.95)]),
    "shared-queue-unguarded": dict(
        what="Negative control: the same with the queue lock off must show races (the check can see them)",
        env=dict(FIGHT, AURORA_XR_SHARED_QUEUE="1", AURORA_XR_QUEUE_GUARD="0", **VALIDATION), frames=1500,
        need=[r"THREADING ERROR"], forbid=[]),
    "no-room": dict(
        what="A runtime with no passthrough (Monado): full VR, whole stages, not a black room",
        env=FIGHT, frames=900,
        need=[r"passthrough off", r"xr: the headset shows no room: full VR", r"3D eyes render"],
        forbid=[], checks=[dump_coverage(0.5, 1.0)]),
    "frame-sim": dict(
        what="The Steam Frame's personality: alpha-blend room, Frame controllers, 16 swapchains, D-pad/shoulder/A",
        env=dict(FIGHT, **FRAME, AURORA_XR_INPUT_LOG="1",
                 AURORA_SIM_PRESS="/user/hand/left/input/dpad_up/click@300-310;"
                                  "/user/hand/right/input/bumper/click@320-330;"
                                  "/user/hand/right/input/a/click@350-352"), frames=1500,
        need=[r"Environment blend modes: .*alpha blend", r"passthrough on \(alpha blend\)",
              r"Steam Frame controller bindings", r"Right hand: /interaction_profiles/valve/frame_controller_valve",
              r"XR pad buttons 0x0008", r"XR pad buttons 0x0010", r"XR pad buttons 0x0100", r"3D eyes render"],
        forbid=[r"a tracked hand \(pinches\)"],
        checks=[dump_coverage(0.02, 0.95),
                sim_summary(lambda f, b, u, p, r, a, j: b > 0 and u == 0 and p <= 16 and r == 0 and a == 1,
                            "the room needs a projection layer first in every blended frame, within 16 swapchains")]),
    "frame-shoulder": dict(
        what="A SteamVR that only knows the shoulder buttons' new name: the bumper bindings are refused, shoulder taken",
        env=dict(FIGHT, **FRAME, AURORA_SIM_FRAME_SHOULDER="shoulder"), frames=600,
        need=[r"Steam Frame controller bindings: /user/hand/left/input/shoulder/click"], forbid=[],
        checks=[sim_summary(lambda f, b, u, p, r, a, j: a == 1 and j >= 1, "expected one refusal, then shoulder")]),
    "pacing-90-unreported": dict(
        what="90 Hz without a reported rate: the game keeps its own 60 Hz timer (it used to step at 90)",
        env=dict(FIGHT, **SIM, AURORA_SIM_PERIOD_MS="11.111", AURORA_SIM_REPORTED_HZ="0"), frames=600,
        need=[r"Display at 90\.0 Hz.*game keeps its own timer"], forbid=[r"Display at 90\.0 Hz: game paced"]),
    "pacing-120-unreported": dict(
        what="120 Hz without a reported rate: each game frame is two display frames",
        env=dict(FIGHT, **SIM, AURORA_SIM_PERIOD_MS="8.333", AURORA_SIM_REPORTED_HZ="0"), frames=300,
        need=[r"Display at 120\.0 Hz: game paced to every 2 display frame"], forbid=[]),
    "pacing-granted-90-runs-72": dict(
        what="A rate granted but not run at (SteamVR: 90 granted, 72 shown): the frames' period decides",
        env=dict(FIGHT, **SIM, AURORA_SIM_PERIOD_MS="13.889", AURORA_SIM_REPORTED_HZ="90", AURORA_SIM_RATES="90"),
        frames=600,
        need=[r"Display at 72\.0 Hz \(the runtime reports 90 Hz", r"keeping the headset's"], forbid=[]),
    "placing-capped": dict(
        what="The placing hold under a swapchain cap of 11: the runtime's limit learned, pictures wait, no failure",
        env={"MELEE_BOOT_SCENE": "place", "AURORA_XR_ARENA_AT_HEAD": "1", **FRAME, "AURORA_SIM_MAX_SWAPCHAINS": "11"},
        frames=900,
        need=[r"The runtime allows 11 swapchains", r"No swapchain to spare for placing picture"], forbid=[],
        checks=[sim_summary(lambda f, b, u, p, r, a, j: p == 11 and r == 1, "one refusal, at 11")]),
    "frame-launcher": dict(
        what="The Steam Frame's build: its launcher in the headset, the disc found in ~/Games, Menu presses Play",
        frame_build=True, no_disc=True,
        env=dict(FIGHT, **FRAME, AURORA_XR_INPUT_LOG="1",
                 # Play, and again to skip the wait for pipelines (a fresh HOME has none).
                 AURORA_SIM_PRESS="/user/hand/right/input/menu/click@200-203;"
                                  "/user/hand/right/input/menu/click@260-263"), frames=600,
        need=[r"Launcher: (starting the game|pipelines built, starting the game|Play waits)",
              r"passthrough on \(alpha blend\)", r"XR pad buttons 0x1000", r"3D eyes render"], forbid=[]),
    "placing-frame": dict(
        what="The placing hold within the Steam Frame's 16 swapchains, with its own cards",
        env={"MELEE_BOOT_SCENE": "place", "AURORA_XR_ARENA_AT_HEAD": "1", **FRAME}, frames=900,
        need=[r"Swapchains alive: 14"], forbid=[r"No swapchain to spare"],
        checks=[sim_summary(lambda f, b, u, p, r, a, j: p <= 16 and r == 0, "over 16 swapchains")]),
}

FORBID_ALWAYS = [r"PANIC", r"\[FATAL\]", r"Segmentation fault", r"AddressSanitizer",
                 r"OpenXR unavailable", r"no spare queue"]


def monado_paths():
    prefix = os.environ.get("MONADO")
    service = os.environ.get("MONADO_SERVICE") or (prefix and str(Path(prefix) / "bin" / "monado-service")) \
        or shutil.which("monado-service")
    runtime = os.environ.get("XR_RUNTIME_JSON") or (prefix and str(Path(prefix) / "share" / "openxr" / "1" /
                                                                   "openxr_monado.json"))
    return service, runtime


def run_case(name, case, disc, out, headless, timeout):
    run = out / name
    shutil.rmtree(run, ignore_errors=True)
    (run / "dump").mkdir(parents=True)
    service, runtime = monado_paths()
    env = dict(os.environ)
    env.update(XR_RUNTIME_JSON=runtime, AURORA_XR="1", SDL_AUDIO_DRIVER="dummy",
               MELEE_LOG_FILE=str(run / "melee.log"), MELEE_EXIT_AFTER_FRAMES=str(case["frames"]),
               AURORA_XR_DUMP=str(run / "dump"), AURORA_XR_DUMP_AFTER="200")
    env.update(case["env"])
    binary, argv = (FRAME_MELEE if case.get("frame_build") else MELEE), [disc]
    if case.get("no_disc"):
        # A fresh home with the disc in ~/Games: found there, not remembered.
        home = run / "home"
        (home / "Games").mkdir(parents=True)
        (home / "Games" / Path(disc).name).symlink_to(Path(disc).resolve())
        env["HOME"] = str(home)
        env.pop("XDG_DATA_HOME", None)
        argv = []
    procs = []
    try:
        mon_env = dict(os.environ, XRT_NO_STDIN="1", XRT_COMPOSITOR_NULL="1", QWERTY_ENABLE="1",
                       XR_RUNTIME_JSON=runtime)
        subprocess.run(["pkill", "-x", "monado-service"], check=False)
        time.sleep(0.5)
        procs.append(subprocess.Popen([service], env=mon_env, stdout=open(run / "monado.log", "w"),
                                      stderr=subprocess.STDOUT))
        if headless:
            sock = "xr-monado-test-%d" % os.getpid()
            procs.append(subprocess.Popen(["weston", "--backend=headless", "--renderer=gl", "--socket=" + sock,
                                           "--width=1920", "--height=1080"],
                                          stdout=open(run / "weston.log", "w"), stderr=subprocess.STDOUT))
            env.update(WAYLAND_DISPLAY=sock, SDL_VIDEO_DRIVER="wayland")
            env.pop("DISPLAY", None)
        time.sleep(2)
        with open(run / "stdout.log", "w") as log:
            try:
                status = subprocess.run([str(binary)] + argv, env=env, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=timeout).returncode
            except subprocess.TimeoutExpired:
                status = "timeout"
    finally:
        for p in reversed(procs):
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
    text = (run / "stdout.log").read_text(errors="replace")
    if (run / "melee.log").exists():
        text += (run / "melee.log").read_text(errors="replace")
    failures = []
    if status != 0:
        failures.append("exit status %s" % status)
    for pattern in case["need"]:
        if not re.search(pattern, text):
            failures.append("missing: " + pattern)
    for pattern in case["forbid"] + FORBID_ALWAYS:
        if pattern in case["need"]:
            continue
        if re.search(pattern, text):
            failures.append("forbidden: " + pattern)
    for check in case.get("checks", []):
        message = check(run)
        if message:
            failures.append(message)
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--case", action="append", help="run only this case (repeatable)")
    ap.add_argument("--list", action="store_true", help="list the cases")
    ap.add_argument("--headless", action="store_true", help="run on a headless Weston of its own")
    ap.add_argument("--out", default=None, help="where runs are kept (default: a temporary directory)")
    ap.add_argument("--timeout", type=int, default=600, help="seconds per run")
    args = ap.parse_args()
    if args.list:
        for name, case in CASES.items():
            print("%-26s %s" % (name, case["what"]))
        return 0
    disc = os.environ.get("MELEE_DISC") or str(ROOT.parent / "melee.ciso")
    service, runtime = monado_paths()
    for what, path in (("disc image (MELEE_DISC)", disc), ("XR build (MELEE_BIN)", str(MELEE)),
                       ("monado-service (MONADO_SERVICE or MONADO)", service),
                       ("Monado's runtime manifest (XR_RUNTIME_JSON or MONADO)", runtime)):
        if not path or not Path(path).exists():
            print("%s not found: %s" % (what, path), file=sys.stderr)
            return 2
    out = Path(args.out) if args.out else Path(tempfile.mkdtemp(prefix="xr-monado-test-"))
    names = args.case or list(CASES)
    failed = 0
    for name in names:
        case = CASES[name]
        if case.get("frame_build") and not FRAME_MELEE.exists():
            print("SKIP %-26s no Steam Frame build for this machine (MELEE_FRAME_BIN)" % name)
            continue
        uses_layer = "XR_ENABLE_API_LAYERS" in case["env"]
        if uses_layer and not (LAYER_DIR / "XrApiLayer_aurora_frame_sim.json").exists():
            print("SKIP %-26s the test layer isn't built (ninja XrApiLayer_aurora_frame_sim)" % name)
            continue
        start = time.time()
        failures = run_case(name, case, disc, out, args.headless, args.timeout)
        took = time.time() - start
        if failures:
            failed += 1
            print("FAIL %-26s %4.0fs  %s" % (name, took, "; ".join(failures)))
        else:
            print("PASS %-26s %4.0fs  %s" % (name, took, case["what"]))
    print("runs in %s" % out)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
