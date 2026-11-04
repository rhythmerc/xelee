#!/usr/bin/env python3
"""The Quest's frame rate under 4-player rollback: the headset as one
player of a direct netplay session whose other players run on this PC.

    tools/quest_net_bench.py [--minutes 3] [--warmup 40] [--label NAME]
                             [--link SPEC] [--quest-port 2] [--ctf | --vs]
                             [--cpu-level 3 --gpu-level 2] [--env K=V ...]

Players: P1 is instance a on this PC (it hosts and presses Start), the
Quest plays --quest-port (P2 by default), and instance c plays the rest
(two seats, as a PC with two pads: net_test_n --owners). Two game instances
on the PC, the most its 16 GB holds with the headset's peers.

Every PC seat runs net_test_n's keyboard workout, so the Quest keeps
predicting their input wrong and rolling back, as in a real match. The
Quest's own seat is idle. The link simulator (MELEE_NET_SIM_LINK, on every
player) shapes the mesh: by default one player (P4) is at ~95 ms with
+-40 ms jitter to everyone, the 2026-11-02 playtest's worst case, and the
others ~20 ms.

The match is Capture the Flag on Temple (MELEE_DEBUG_CTF) unless --vs. The
headset's clocks can be pinned (--cpu-level 3 --gpu-level 2: the caps for
apps showing passthrough, the benchmark standard; quest_perf.py).

Out: the VrApi frame stats over the measured window (quest_perf
summarize), the Quest's net and snapshot stat lines, and its game log at
build/quest-perf/<label>-game.log.
"""
import argparse
import os
import re
import shutil
import socket
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import net_test as nt  # noqa: E402
import net_test_n as ntn  # noqa: E402
import quest_perf as qp  # noqa: E402

GAME_LOG = f"{qp.FILES}/bench-game.log"
# By machine (each machine's first port): {a} is the laggy player at ~95 ms
# +-40 to everyone, the 2026-11-02 playtest's worst; the rest ~20 ms.
DEFAULT_LINK = "{a}:{q}=95~40,{a}:{c}=95~40,{q}:{c}=20~5"


def lan_ip(towards):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((towards, 9))
        return s.getsockname()[0]
    finally:
        s.close()


def quest_ip():
    out = qp.adb("shell", "ip -4 addr show wlan0", check=False)
    m = re.search(r"inet (\d+\.\d+\.\d+\.\d+)/", out)
    if not m:
        sys.exit("no wlan0 address on the headset")
    return m.group(1)


def net_lines(text):
    rb = [(int(a), int(b)) for a, b in re.findall(r"net: frame \d+, rollbacks (\d+) \(max depth (\d+)", text)]
    snap = re.findall(r"net:   snapshot take ([\d.]+) ms \(max ([\d.]+), n (\d+)\), restore ([\d.]+) ms "
                      r"\(max ([\d.]+), n (\d+)\)", text)
    stalls = re.findall(r"stalls (\d+) \(worst ([\d.]+) ms\)", text)
    lines = []
    if rb:
        n = [a for a, _ in rb]
        lines.append(f"  rollbacks per 10 s: mean {sum(n) / len(n):.0f}, max {max(n)}; "
                     f"deepest {max(b for _, b in rb)} ({len(rb)} windows)")
    if snap:
        take = [float(s[0]) for s in snap]
        takes = [int(s[2]) for s in snap]
        rest = [float(s[3]) for s in snap]
        rests = [int(s[5]) for s in snap]
        lines.append(f"  snapshot take {sum(take) / len(take):.2f} ms x {sum(takes) / len(takes) / 10:.0f}/s "
                     f"(max {max(float(s[1]) for s in snap):.2f}); restore {sum(rest) / len(rest):.2f} ms x "
                     f"{sum(rests) / len(rests) / 10:.1f}/s (max {max(float(s[4]) for s in snap):.2f})")
    if stalls:
        lines.append(f"  stalls per 10 s: mean {sum(int(s[0]) for s in stalls) / len(stalls):.1f}, "
                     f"worst {max(float(s[1]) for s in stalls):.0f} ms")
    return "\n".join(lines) or "  no net stat lines"


def run(args):
    pc = lan_ip("192.168.0.1")
    qip = quest_ip()
    q = args.quest_port
    if not 0 <= q <= 3:
        sys.exit("--quest-port must be 0-3 (P1-P4)")
    # Three machines: the Quest plays its port, PC instance a the lowest of
    # the others, instance c the remaining two. Whoever plays P1 hosts.
    # MELEE_NET_SIM_LINK shapes links by machine (a machine's first port),
    # so a laggy player must be a machine of its own: a, by default.
    others = [k for k in range(4) if k != q]
    seats = {"q": [q], "a": others[:1], "c": others[1:]}
    order = sorted(seats, key=lambda m: seats[m][0])  # machines in port order
    plays = [seats[m] for m in order]
    owners = [next(i for i, m in enumerate(order) if k in seats[m]) for k in range(4)]
    base = args.port
    ports = [base, base + 1, base + 2]
    addr = [qip if m == "q" else pc for m in order]
    qi, ai, ci = order.index("q"), order.index("a"), order.index("c")
    peers = ",".join("+".join(str(k) for k in plays[m]) + f"={addr[m]}:{ports[m]}" for m in range(3))
    link = args.link.format(q=q, a=seats["a"][0], c=seats["c"][0])
    frames = int((args.minutes + 3) * 3600) + nt.BOOT_FRAMES
    common = {
        "MELEE_NET_PEERS": peers,
        "MELEE_DEBUG_VS": "human4",
        "MELEE_NET_EXIT_AFTER_FRAMES": str(frames),
        "MELEE_NET_KEY": nt.NET_KEY,
        "MELEE_SEED": "7",
        # Straight into the match on every machine: the title is a scene the
        # headset left on its own at frame 4 (2026-11-04, cause not found;
        # lobbies never pass through it), and nobody needs to press Start.
        "MELEE_BOOT_SCENE": "vs",
        "MELEE_NET_SIM_LINK": link,
    }
    if not args.vs:
        common["MELEE_DEBUG_CTF"] = "1"
        common["MELEE_DEBUG_CTF_NOTIME"] = "1"
    for kv in args.env:
        k, _, v = kv.partition("=")
        common[k] = v

    # The headset first: it boots slower than the PC's instances.
    qenv = dict(common)
    qenv.update({
        "MELEE_NET_PLAYER": str(q),
        "MELEE_NET_PORT": str(ports[qi]),
        "MELEE_LOG_FILE": GAME_LOG,
        "AURORA_XR_TIMING": "1",
        "MELEE_XR_MODE": args.mode,
        "AURORA_XR_ARENA_AT_HEAD": "2",
        "AURORA_XR_ARENA_SCALE": str(args.arena_scale),
    })
    for kv in args.quest_env:
        k, _, v = kv.partition("=")
        qenv[k] = v
    disc = qp.disc_uri()
    qp.wake_headset()
    qp.save_player_env()
    qp.shell_write(qp.ENV_FILE, qp.RUN_ENV_MARK + "".join(f"{k}={v}\n" for k, v in qenv.items()))
    qp.adb("shell", "rm", "-f", GAME_LOG, check=False)
    qp.adb("shell", "am", "broadcast", "-a", "com.oculus.vrpowermanager.prox_close")
    qp.adb("shell", "settings", "put", "system", "screen_off_timeout", str(qp.RUN_SCREEN_TIMEOUT_MS))
    qp.adb("shell", "am", "force-stop", qp.PKG)
    qp.grant_permissions()
    qp.adb("logcat", "-G", qp.LOG_BUFFER, check=False)
    qp.adb("logcat", "-c")
    qp.adb("shell", "am", "start", "-n", f"{qp.PKG}/dev.melee.MeleeXrActivity", "--es", "disc", disc)
    print(f"{args.label}: Quest {qip} plays P{q + 1}{' (host)' if q == 0 else ''}; PC {pc}; peers {peers}; "
          f"link {link}", flush=True)

    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    insts, pad_fifos = [], {}
    for m, name in ((ai, "a"), (ci, "c")):
        if not nt.wait_port_free(ports[m]):
            return False
        env = dict(common)
        env["MELEE_NET_PLAYER"] = str(plays[m][0])
        for pad, port in enumerate(plays[m][1:], start=2):
            fifo = os.path.join(args.work, f"{name}.pad{pad}.keys")
            os.mkfifo(fifo)
            env[f"MELEE_KEY_FIFO_P{pad}"] = fifo
            pad_fifos[port] = fifo
        inst = nt.Instance(name, args.exe, args.disc, args.work, ports[m], 0, env, True)
        pad_fifos[plays[m][0]] = inst.fifo
        insts.append(inst)
    a = insts[0]
    drive = None
    ok = False
    try:
        # Everyone must be in the session before Start means anything: the
        # handshake waits for the headset.
        # Start only once every machine is past the handshake and in step: a
        # press in the frames before (start_frame) reached the headset's title
        # but not the PC's, and the headset left the session (2026-11-04).
        if not all(i.wait_log(r"net: handshake done", 180) for i in insts):
            print("quest_net_bench: the PC players never finished the handshake", flush=True)
            return False
        deadline = time.time() + 180
        while "net: handshake done" not in qp.adb("shell", "cat", GAME_LOG, check=False):
            if time.time() > deadline:
                print("quest_net_bench: the headset never finished the handshake", flush=True)
                return False
            time.sleep(2)
        time.sleep(8)
        ok = nt.wait_match(insts, 240)
        if not ok:
            print("quest_net_bench: the match never started", flush=True)
            return False
        drive = ntn.Drive([(insts[0 if k in seats["a"] else 1], pad_fifos[k]) for k in range(4) if k != q])
        print(f"{args.label}: match on; warming up {args.warmup} s", flush=True)
        time.sleep(args.warmup)
        qp.adb("logcat", "-c")
        t0 = time.time()
        time.sleep(args.minutes * 60)
        text = qp.adb("logcat", "-d", "-v", "time")
        game = qp.adb("shell", "cat", GAME_LOG, check=False)
        os.makedirs(qp.OUT_DIR, exist_ok=True)
        with open(os.path.join(qp.OUT_DIR, f"{args.label}.log"), "w") as f:
            f.write(text)
        with open(os.path.join(qp.OUT_DIR, f"{args.label}-game.log"), "w") as f:
            f.write(game)
        # The game log's lines from the measured window only.
        window = "\n".join(qp.game_lines(text))
        print(f"[{args.label}] {time.time() - t0:.0f} s measured\n{qp.summarize(text)}\n{net_lines(window)}")
        bad = [p for p in (r"net: DESYNC", r"peer silent", r"cannot roll back") if re.search(p, game)]
        if bad:
            print(f"  WARNING: the Quest's log has {', '.join(bad)}")
        for i in insts:
            if re.search(r"net: DESYNC", i.text()):
                print(f"  WARNING: [{i.name}] DESYNC")
        return True
    finally:
        if drive is not None:
            drive.done()
        for i in insts:
            i.kill(20)
        qp.adb("shell", "am", "force-stop", qp.PKG, check=False)
        qp.remove_run_files()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--minutes", type=float, default=3)
    ap.add_argument("--warmup", type=int, default=40)
    ap.add_argument("--label", default="net4")
    ap.add_argument("--link", default=DEFAULT_LINK)
    ap.add_argument("--quest-port", type=int, default=1, help="0-based: 0 = P1, the host")
    ap.add_argument("--vs", action="store_true", help="a VS match on the debug stage, not CTF")
    ap.add_argument("--mode", default="mr", choices=("mr", "vr"))
    ap.add_argument("--arena-scale", type=float, default=0.0125)
    ap.add_argument("--cpu-level", type=int)
    ap.add_argument("--gpu-level", type=int)
    ap.add_argument("--env", action="append", default=[], help="K=V for every player")
    ap.add_argument("--quest-env", action="append", default=[], help="K=V for the headset only")
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "build", "linux", "melee"))
    ap.add_argument("--disc", default=os.path.join(HERE, "..", "..", "melee.ciso"))
    ap.add_argument("--port", type=int, default=42150)
    ap.add_argument("--work", default=os.path.join(HERE, "..", "build", "quest_net_bench"))
    ap.add_argument("--hand-back", action="store_true", help="hand the headset back after the run")
    args = ap.parse_args()
    nt.UNPACED = False
    qp.pick_device()
    try:
        if args.cpu_level is not None or args.gpu_level is not None:
            qp.set_levels(args.cpu_level, args.gpu_level)
        ok = run(args)
    finally:
        if args.cpu_level is not None or args.gpu_level is not None:
            qp.set_levels(None, None)
        if args.hand_back:
            qp.hand_back()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
