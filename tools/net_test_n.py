#!/usr/bin/env python3
"""Netplay integration test for three or four players: N instances on one
machine in one direct session, a match, then the same per-instance asserts as
tools/net_test.py.

    tools/net_test_n.py [--players 3|4] [--loss PCT] [--delay MS] [--jitter]
                        [--minutes N] [--no-key] [--exe build/melee]
                        [--disc ../melee.ciso] [--port 42150] [--work /tmp/net_test_n]

Every instance gets MELEE_NET_PEERS listing all of them by controller port
(src/pc/net.c), MELEE_NET_PLAYER its own, and MELEE_DEBUG_VS=human<N>: the
title's Start goes straight into an N-human match (gmvsmode.c). Player 1
hosts the handshake. Each instance drives its own keyboard workout, offset
from the others, so every remote input keeps changing and the rollback loop
has something to correct on every link.

--no-key leaves MELEE_NET_KEY unset, so the links run on keys derived from
the handshake: host<->guest from RULES/READY, guest<->guest from the host's
REL_NONCES (net_handshake.c). The link simulator's knobs apply to every link
of every instance.

Pass = every instance done, exit 0, a session line naming every peer, no
DESYNC, no "peer silent", no "cannot roll back", and a match that simulated
(net_test.py summarize/check_match). Logs: <work>/a.log, b.log, ...
"""
import argparse
import os
import shutil
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import net_test as nt  # noqa: E402


class Drive:
    """One workout thread per instance, each starting at a different point of
    WORKOUT and holding a little longer than the last, so no two players'
    inputs move in step."""

    def __init__(self, insts):
        self._stop = threading.Event()
        self.n = 0
        self._threads = [threading.Thread(target=self._run, args=(inst, k), daemon=True)
                         for k, inst in enumerate(insts)]
        for t in self._threads:
            t.start()

    def _run(self, inst, k):
        i = 2 * k
        while not self._stop.is_set():
            key, ms = nt.WORKOUT[i % len(nt.WORKOUT)]
            i += 1
            ms += 17 * k
            if inst.proc.poll() is None:
                try:
                    inst.key(f"{key} {ms}")
                except Exception:
                    return
            self.n += 1
            self._stop.wait(ms / 1000 + 0.15)

    def done(self):
        self._stop.set()
        for t in self._threads:
            t.join(10)
        return self.n


def run(args):
    names = "abcd"[:args.players]
    ports = [args.port + k for k in range(args.players)]
    frames = int(args.minutes * 3600) + nt.BOOT_FRAMES
    peers = ",".join(f"{k}=127.0.0.1:{p}" for k, p in enumerate(ports))
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    for p in ports:
        if not nt.wait_port_free(p):
            return False
    insts = []
    for k, name in enumerate(names):
        env = {
            "MELEE_NET_PEERS": peers,
            "MELEE_NET_PLAYER": str(k),
            "MELEE_DEBUG_VS": f"human{args.players}",
            "MELEE_NET_EXIT_AFTER_FRAMES": str(frames),
        }
        if not args.no_key:
            env["MELEE_NET_KEY"] = nt.NET_KEY
        if args.loss:
            env["MELEE_NET_SIM_LOSS"] = str(args.loss)
        if args.delay:
            env["MELEE_NET_SIM_DELAY_MS"] = str(args.delay)
        if args.jitter:
            env["MELEE_NET_SIM_JITTER_MS"] = "20"
        # lan=True: Instance sets no MELEE_NET of its own; the env above is the session.
        insts.append(nt.Instance(name, args.exe, args.disc, args.work, ports[k], 0, env, True))
    print(f"net_test_n: {args.players} players loss={args.loss}% delay={args.delay}ms "
          f"{'jitter ' if args.jitter else ''}{'derived keys ' if args.no_key else ''}"
          f"minutes={args.minutes} frames={frames} logs={args.work}", flush=True)
    ok = True
    drive = None
    try:
        # The session is up from boot: player 1's Start reaches every sim.
        ok = nt.press_until(insts, "Return", 9, nt.SCENE_FILE["match"], tries=40, each=8.0,
                            on=(insts[0],))
        if not ok:
            print("net_test_n: the match never loaded", flush=True)
        if ok and not nt.wait_match(insts, 150):
            nt.press((insts[0],), "Return", 9, 2)  # a Start that landed mid-load paused it
            ok = nt.wait_match(insts, 120)
        if ok:
            drive = Drive(insts)
        deadline = time.time() + frames / 12.0 + 60
        term = (r"net: DESYNC", r"peer silent for \d+ ms at frame \d+, leaving netplay",
                r"net: disconnected", r"cannot roll back")
        last_frame, last_progress = -1, time.time()
        while time.time() < deadline and any(i.proc.poll() is None for i in insts):
            time.sleep(1)
            texts = "".join(i.text() for i in insts)
            if any(nt.re.search(t, texts) for t in term):
                break
            fr = max([int(x) for x in nt.re.findall(r"net: frame (\d+)", texts)] or [-1])
            if fr > last_frame:
                last_frame, last_progress = fr, time.time()
            elif time.time() - last_progress > 25 and last_frame >= 0:
                print("net_test_n: no frame progress for 25 s, giving up", flush=True)
                break
    finally:
        if drive is not None:
            print(f"net_test_n: workout wrote {drive.done()} key lines", flush=True)
        for inst in insts:
            inst.kill(20)
    # Every instance must have linked to every other one.
    for inst in insts:
        seen = len(nt.re.findall(r"net: peer \d+ is P\d at ", inst.text()))
        if seen != args.players - 1:
            print(f"[{inst.name}] FAIL: session names {seen} peers, want {args.players - 1}")
            ok = False
    for inst in insts:
        fails, line, _ = nt.summarize(inst, need_match=True)
        print(line)
        if fails:
            ok = False
            print(f"[{inst.name}] FAIL: " + ", ".join(fails))
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--players", type=int, choices=(3, 4), default=4)
    ap.add_argument("--loss", type=int, default=0)
    ap.add_argument("--delay", type=int, default=0)
    ap.add_argument("--jitter", action="store_true")
    ap.add_argument("--minutes", type=float, default=1)
    ap.add_argument("--no-key", action="store_true", help="links keyed by the handshake only")
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "build", "melee"))
    ap.add_argument("--disc", default=os.path.join(HERE, "..", "..", "melee.ciso"))
    ap.add_argument("--port", type=int, default=42150, help="P1's UDP port; P2.. use +1..")
    ap.add_argument("--work", default="/tmp/net_test_n")
    ok = run(ap.parse_args())
    print("net_test_n: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
