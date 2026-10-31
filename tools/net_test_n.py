#!/usr/bin/env python3
"""Netplay integration test for three or four players: N instances on one
machine in one direct session, a match, then the same per-instance asserts as
tools/net_test.py.

    tools/net_test_n.py [--players 3|4] [--loss PCT] [--delay MS] [--jitter]
                        [--minutes N] [--no-key] [--block A:B,...] [--kill X@SECONDS]
                        [--exe build/melee]
                        [--disc ../melee.ciso] [--port 42150] [--work /tmp/net_test_n]

Every instance gets MELEE_NET_PEERS listing all of them by controller port
(src/pc/net.c), MELEE_NET_PLAYER its own, and MELEE_DEBUG_VS=human<N>: the
title's Start goes straight into an N-human match (gmvsmode.c). Player 1
hosts the handshake. Each instance drives its own keyboard workout, offset
from the others, so every remote input keeps changing and the rollback loop
has something to correct on every link.

--block 1:2 cuts those controller ports (0 is P1) off from each other in the
link simulator, as two guests behind strict NATs: they must find out, tell
the host, and get each other's input and messages through it (net.c, routes).

--kill c@20 SIGKILLs instance c 20 s into the match: no goodbye, just
silence. The others must agree where its input ends, void the match on one
frame and play on (net.c, drops); the killed instance is not judged.

--xn connects through XeleeNet instead of listing addresses: a local
lobby service, built and started locally (tools/xeleenet; or --xn-url), player 1
hosts (MELEE_XN=host), and once its log names the lobby code the others join
with it (MELEE_XN=join:CODE). Each has its own key (MELEE_XN_IDENTITY), STUN
and its LAN address for candidates, and the punch (net_punch.c) decides every
link and keys it before the engine starts. With --block, the blocked pairs
must come out of the punch "through the host".

--owners 0,1,1,2 lets one instance play several ports, as a PC with more than
one pad does (docs/lan-multipad-plan.md): entry k is the instance that plays
port k, here b plays P2 and P3. Its entry in MELEE_NET_PEERS lists them all
("1+2=..."), and each extra port reads a pad of its own, driven through
MELEE_KEY_FIFO_P2.. with a workout of its own. Direct sessions only.

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
    """One workout thread per port, each starting at a different point of
    WORKOUT and holding a little longer than the last, so no two players'
    inputs move in step. `pads` is an instance (its keyboard FIFO) or an
    (instance, fifo) pair per port."""

    def __init__(self, pads):
        self._stop = threading.Event()
        self.n = 0
        pads = [p if isinstance(p, tuple) else (p, p.fifo) for p in pads]
        self._threads = [threading.Thread(target=self._run, args=(inst, fifo, k), daemon=True)
                         for k, (inst, fifo) in enumerate(pads)]
        for t in self._threads:
            t.start()

    def _run(self, inst, fifo, k):
        i = 2 * k
        while nt.UNPACED and not self._stop.is_set():
            batch = []
            for _ in range(12):
                key, ms = nt.WORKOUT[i % len(nt.WORKOUT)]
                i += 1
                batch.append((key, ms + 17 * k))
            if inst.proc.poll() is not None or not nt.script_batch(inst, fifo, batch, self._stop):
                return
            self.n += len(batch)
        while not self._stop.is_set():
            key, ms = nt.WORKOUT[i % len(nt.WORKOUT)]
            i += 1
            ms += 17 * k
            if inst.proc.poll() is None:
                try:
                    nt.fifo_write(fifo, f"{key} {ms}")
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
    owners = [int(x) for x in args.owners.split(",")] if args.owners else list(range(args.players))
    if len(owners) != args.players or sorted(set(owners)) != list(range(max(owners) + 1)) or \
            any(owners[k] > k for k in range(len(owners))) or args.xn_url and args.owners:
        print("net_test_n: --owners wants one instance per port, numbered in port order, "
              "direct sessions only", flush=True)
        return False
    machines = max(owners) + 1
    plays = [[k for k in range(args.players) if owners[k] == m] for m in range(machines)]
    names = "abcd"[:machines]
    ports = [args.port + m for m in range(machines)]
    frames = int(args.minutes * 3600) + nt.BOOT_FRAMES
    peers = ",".join("+".join(str(k) for k in plays[m]) + f"=127.0.0.1:{ports[m]}"
                     for m in range(machines))
    shutil.rmtree(args.work, ignore_errors=True)
    os.makedirs(args.work)
    for p in ports:
        if not nt.wait_port_free(p):
            return False
    insts = []
    code = None
    pad_fifos = {}  # port -> the FIFO of the pad that drives it
    for k, name in enumerate(names):
        env = {
            "MELEE_NET_PEERS": peers,
            "MELEE_NET_PLAYER": str(plays[k][0]),
            "MELEE_DEBUG_VS": f"human{args.players}",
            "MELEE_NET_EXIT_AFTER_FRAMES": str(frames),
        }
        if args.xn_url:
            ident = os.path.join(args.work, f"{name}.id")
            os.makedirs(ident)
            env = {
                "MELEE_XN": f"join:{'@127.0.0.1' if args.by_address else code}" if k else "host",
                "MELEE_XN_URL": "" if args.lan else args.xn_url,
                "MELEE_XN_SEATS": str(args.players),
                "MELEE_XN_NAME": f"PLAYER {k + 1}",
                "MELEE_XN_IDENTITY": ident,
                "MELEE_DEBUG_VS": f"human{args.players}",
                "MELEE_NET_EXIT_AFTER_FRAMES": str(frames),
            }
            if args.stun:
                env["MELEE_XN_STUN"] = args.stun
            if args.lan:
                env["MELEE_XN_NET"] = "lan"  # the host's game is the lobby service (xeleenet_lan.c)
        elif not args.no_key:
            env["MELEE_NET_KEY"] = nt.NET_KEY
        if args.loss:
            env["MELEE_NET_SIM_LOSS"] = str(args.loss)
        if args.delay:
            env["MELEE_NET_SIM_DELAY_MS"] = str(args.delay)
        if args.jitter:
            env["MELEE_NET_SIM_JITTER_MS"] = "20"
        if args.block:
            env["MELEE_NET_SIM_BLOCK"] = args.block
        for pad, port in enumerate(plays[k][1:], start=2):
            fifo = os.path.join(args.work, f"{name}.pad{pad}.keys")
            os.mkfifo(fifo)
            env[f"MELEE_KEY_FIFO_P{pad}"] = fifo
            pad_fifos[port] = fifo
        # lobby=True: Instance sets no MELEE_NET of its own; the env above is the session.
        insts.append(nt.Instance(name, args.exe, args.disc, args.work, ports[k], 0, env, True))
        pad_fifos[plays[k][0]] = insts[-1].fifo
        if args.xn_url and k == 0:
            # The others join with the code the host's log names.
            if not insts[0].wait_log(r"xeleenet: lobby code [A-Z0-9]{6}", 60):
                print("net_test_n: the host never opened a lobby", flush=True)
                insts[0].kill()
                return False
            code = nt.re.search(r"xeleenet: lobby code ([A-Z0-9]{6})", insts[0].text()).group(1)
            print(f"net_test_n: lobby code {code}", flush=True)
    print(f"net_test_n: {args.players} players loss={args.loss}% delay={args.delay}ms "
          f"{'jitter ' if args.jitter else ''}{'derived keys ' if args.no_key else ''}"
          f"minutes={args.minutes} frames={frames} logs={args.work}", flush=True)
    ok = True
    drive = None
    try:
        if args.xn_url:
            # Boot waits in the connect phase until the host starts everyone;
            # nothing reads the key FIFO before that.
            for inst in insts:
                if not inst.wait_log(r"xeleenet: session \d+ up", 180):
                    print(f"[{inst.name}] the XeleeNet connect phase did not finish", flush=True)
                    ok = False
            if not ok:
                return False
        # The session is up from boot: player 1's Start reaches every sim.
        ok = nt.press_until(insts, "Return", 9, nt.SCENE_FILE["match"], tries=40, each=8.0,
                            on=(insts[0],))
        if not ok:
            print("net_test_n: the match never loaded", flush=True)
        if ok and not nt.wait_match(insts, 150):
            nt.press((insts[0],), "Return", 9, 2)  # a Start that landed mid-load paused it
            ok = nt.wait_match(insts, 120)
        if ok:
            drive = Drive([(insts[owners[k]], pad_fifos[k]) for k in range(args.players)])
        kill_at = None
        if ok and args.kill:
            who, _, secs = args.kill.partition("@")
            # Seconds of match: of wall time paced, of frames (60 a second)
            # unpaced, where the wall runs several times faster.
            now = max([int(x) for x in nt.re.findall(r"net: frame (\d+)", "".join(i.text() for i in insts))] or [0])
            kill_at = (names.index(who), now + float(secs or 20) * 60 if nt.UNPACED else time.time() + float(secs or 20))
            print(f"net_test_n: {who} plays " + ", ".join(f"P{k + 1}" for k in plays[names.index(who)]),
                  flush=True)
        deadline = time.time() + frames / 12.0 + 60
        term = (r"net: DESYNC", r"peer silent for \d+ ms at frame \d+, leaving netplay",
                r"net: disconnected", r"cannot roll back")
        last_frame, last_progress = -1, time.time()
        while time.time() < deadline and any(i.proc.poll() is None for i in insts):
            time.sleep(0.25 if nt.UNPACED else 1)
            fr_now = max([int(x) for x in nt.re.findall(r"net: frame (\d+)", "".join(i.text() for i in insts))] or [0])
            if kill_at is not None and (fr_now if nt.UNPACED else time.time()) >= kill_at[1]:
                victim = insts[kill_at[0]]
                print(f"net_test_n: killing {victim.name} (P{plays[kill_at[0]][0] + 1})", flush=True)
                victim.proc.kill()
                kill_at = None
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
    judged = [i for k, i in enumerate(insts)
              if not (args.kill and names[k] == args.kill.partition("@")[0])]
    if args.xn_url:
        for inst in judged:
            if not nt.re.search(rf"xeleenet: session \d+ up as P\d with {args.players - 1} others", inst.text()):
                print(f"[{inst.name}] FAIL: the XeleeNet connect phase did not finish")
                ok = False
            print(f"[{inst.name}] " + "; ".join(nt.re.findall(
                r"xeleenet: (P\d (?:direct|through the host)) at", inst.text())))
        for pair in filter(None, args.block.split(",")):
            a, b = sorted(int(x) for x in pair.split(":"))
            if a and not nt.re.search(rf"xeleenet: P{a + 1} and P{b + 1} go through the host",
                                      insts[0].text()):
                print(f"FAIL: the punch did not route P{a + 1} and P{b + 1} through the host")
                ok = False
    # Every instance must have linked to every other one.
    for inst in judged:
        seen = len(nt.re.findall(r"net: peer \d+ is P\d at ", inst.text()))
        if seen != machines - 1:
            print(f"[{inst.name}] FAIL: session names {seen} peers, want {machines - 1}")
            ok = False
    for k, inst in enumerate(insts):
        mask = sum(1 << p for p in plays[k])
        if len(plays[k]) > 1 and inst in judged and \
                not nt.re.search(rf"net: this machine plays ports {mask:x} ", inst.text()):
            print(f"[{inst.name}] FAIL: it does not say it plays ports {mask:x}")
            ok = False
    if args.kill:
        # Every survivor agreed on the same last frame and the same void frame.
        ends = {i.name: nt.re.findall(r"input ends at frame (\d+)", i.text()) for i in judged}
        voids = {i.name: nt.re.findall(r"online: match void at frame (\d+)", i.text())
                 for i in judged}
        print(f"net_test_n: input ends {ends}, void {voids}", flush=True)
        if any(len(v) != 1 for v in ends.values()) or len({v[0] for v in ends.values()}) != 1:
            print("FAIL: the survivors did not agree once where the input ends")
            ok = False
        if any(len(v) < 1 for v in voids.values()) or len({v[0] for v in voids.values()}) != 1:
            print("FAIL: the survivors did not void the match on one frame")
            ok = False
    if args.block:
        for inst in judged:
            if not nt.re.search(r"cannot hear P\d; relaying|link to P\d is dead", inst.text()):
                continue
            print(f"[{inst.name}] " + "; ".join(sorted(set(
                nt.re.findall(r"(link to P\d is dead \([a-z ]+\)|P\d cannot hear P\d)", inst.text())))))
    for inst in judged:
        fails, line, _ = nt.summarize(inst, need_match=not args.kill)
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
    ap.add_argument("--paced", action="store_true",
                    help="the old way: 60 Hz, key holds in milliseconds (docs/fast-tests-plan.md)")
    ap.add_argument("--owners", default="", help="the instance that plays each port, e.g. 0,1,1,2")
    ap.add_argument("--block", default="", help="port pairs that cannot reach each other, e.g. 1:2")
    ap.add_argument("--kill", default="", help="instance@seconds into the match to SIGKILL, e.g. c@20")
    ap.add_argument("--xn", action="store_true", help="join by code through a local XeleeNet service")
    ap.add_argument("--xn-url", default="", help="an XeleeNet service already running (implies --xn)")
    ap.add_argument("--stun", default="", help="MELEE_XN_STUN for every instance, e.g. off")
    ap.add_argument("--lan", action="store_true", help="XeleeNet on this network: no service, the guests "
                                                       "find the host's lobby over mDNS")
    ap.add_argument("--by-address", action="store_true", help="with --lan: the guests join by the host's "
                                                              "address, not the list")
    ap.add_argument("--exe", default=os.path.join(HERE, "..", "build", "melee"))
    ap.add_argument("--disc", default=os.path.join(HERE, "..", "..", "melee.ciso"))
    ap.add_argument("--port", type=int, default=42150, help="P1's UDP port; P2.. use +1..")
    ap.add_argument("--work", default="/tmp/net_test_n")
    args = ap.parse_args()
    nt.UNPACED = not args.paced  # docs/fast-tests-plan.md T4: as fast as it runs
    nt.claim_slot(exclusive=args.paced, lan=args.lan)
    args.port = nt.slot_port(args.port)
    if nt.SLOT and args.work == ap.get_default("work"):
        args.work += f"-{nt.SLOT}"
    service = None
    if args.lan:
        args.xn, args.xn_url = True, "lan"  # no service: the code path stays --xn's
    if args.xn and not args.xn_url:
        sys.path.insert(0, os.path.join(HERE, "xeleenet"))
        import test_service  # noqa: E402
        proc, args.xn_url, state = test_service.start_service(short=False)
        service = (test_service, proc, state)
        print(f"net_test_n: XeleeNet at {args.xn_url}", flush=True)
    try:
        ok = run(args)
    finally:
        if service:
            service[0].stop_service(service[1], service[2])
    print("net_test_n: " + ("PASS" if ok else "FAIL"))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
