#!/usr/bin/env python3
"""CPU players in Capture the Flag (docs/ctf-ai-plan.md §7): seeded, unpaced,
CPU-only matches on Hyrule Temple, turned into metrics.

    DISPLAY=:0 tools/ctf_ai_test.py --mode 2v2 --level 9 --seeds 10 --minutes 3 --out DIR

Each run boots the debug VS match (MELEE_DEBUG_VS, MELEE_DEBUG_CTF=1) with
MELEE_SEED=<seed> and MELEE_DEBUG_CTF_AI_LOG=1, reads the game's "ctf:",
"ctfev:" and "ctfpos:" lines, and writes DIR/<tag>-s<seed>.json; the means over
the seeds go to DIR/<tag>.json and stdout.

Modes (ports 1-4 are 0-3 here; red is team 0, blue team 1):
  1v1  ports 0 red, 1 blue          2v2  0,2 red; 1,3 blue (the default)
  2v1  0,2 red; 1 blue              3v1  0,1,2 red; 3 blue
--slots "cchc" makes a port a human (idle, or --script's frame script on port 1)
or empty; --teams "0101" sets the teams by port.

Metrics (per run, then means):
  captures, carried, thrown; captures per minute; time to the first capture
  pickups of the enemy flag; the share of the match an enemy flag is carried
  returns; the median time a free own flag lies before it goes home (a flag
  still lying at the end counts with the time it lay)
  stuck events and self-destructs, per CPU per 5 minutes
  deadlock: time with both flags away and no flag event for 60 s, as a share
  throws: how each carrier throw ended (scored, a pass, handed to the enemy...)
  defender: with both flags home, the share of time each team's defender is
  within 60 units of its stand (C2's role lines)

--synctest runs one match with MELEE_NET_SYNCTEST=1 (every tick twice from a
snapshot) and fails on any mismatch. --record FILE / --replay FILE pass
MELEE_NET_RECORD / MELEE_NET_REPLAY through and report a divergence.
"""
import argparse
import glob
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DISC = "/home/rmercado/Downloads/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso"

# GO! comes this many CTF frames into the match (READY is 150; ctf_test.py
# waits for f170 before its inputs count).
GO_FRAME = 170  # until a "ctfpos" line gives the real one (t: frames since GO)
FPS = 60.0
# Temple's stands (gmctf.c ctf_layouts), red then blue, before a side swap.
STANDS = [(-105.0, -88.6), (200.0, -5.3)]
DEFEND_RADIUS = 60.0
# The C1-C5 cast (CharacterKind numbers): a spacie, a floaty, a heavy, a
# swordie and two all-rounders. Fox, Peach, DK, Marth, Mario, Link.
CAST = "2,12,1,9,8,6"

MODES = {
    "1v1": ("cpu2", "01"),
    "2v2": ("cpu4", "0101"),
    "2v1": ("cpu3", "010"),
    "3v1": ("cpu4", "0001"),
}

RE_FRAME = re.compile(r"^(?:ctf|ctfev|ctfpos): f(\d+) ")


def parse(text):
    """The run's lines, deduplicated by text (a synctest or a rollback runs a
    tick twice and logs it twice), in order."""
    seen = set()
    lines = []
    for raw in text.splitlines():
        line = raw.strip()
        m = re.search(r"(ctf|ctfev|ctfpos): f(\d+) .*", line)
        if not m:
            continue
        line = m.group(0)
        if line in seen:
            continue
        seen.add(line)
        lines.append((int(m.group(2)), line))
    lines.sort(key=lambda t: t[0])
    return lines


def metrics(lines, teams, cpus, end_frame, go_frame=GO_FRAME):
    """teams: team by port (None for an empty port); cpus: CPU ports."""
    flag_home = [True, True]
    flag_holder = [-1, -1]
    flag_free_since = [None, None]
    last_flag_event = 0
    held_frames = [0, 0]
    held_since = [None, None]
    caps = {"red": 0, "blue": 0}
    carried = thrown = 0
    first_cap = None
    pickups_enemy = 0
    returns = 0
    free_times = []
    stuck = []
    kos = []
    sds = []
    deadlock = 0
    both_away_since = None
    throws = {"scored": 0, "pass": 0, "handed": 0, "self": 0, "home": 0, "open": 0}
    pending_throw = [None, None]  # (thrower team) per flag, until the next outcome
    roles = {}
    defend_in = [0, 0]
    defend_total = [0, 0]
    last_pos = {}
    role_changes = 0
    # Trips (C3): out, from GO or the team's last capture to its next pickup
    # of the enemy flag; back, from a pickup to the carried capture it led to
    # (a drop on the way voids it).
    trip_start = [go_frame, go_frame]
    trip_pick = [None, None]
    trips_out, trips_back = [], []

    def flag_event(f):
        nonlocal last_flag_event, deadlock, both_away_since
        # Deadlock: both flags away and nothing happening for 60 s.
        if both_away_since is not None and f - last_flag_event > 60 * FPS:
            deadlock += f - last_flag_event
        last_flag_event = f

    def update_both(f):
        nonlocal both_away_since
        away = not flag_home[0] and not flag_home[1]
        if away and both_away_since is None:
            both_away_since = f
        elif not away:
            both_away_since = None

    def set_held(t, f, holding):
        if holding and held_since[t] is None:
            held_since[t] = f
        elif not holding and held_since[t] is not None:
            held_frames[t] += f - held_since[t]
            held_since[t] = None

    def throw_outcome(t, what):
        if pending_throw[t] is not None:
            throws[what] += 1
            pending_throw[t] = None

    for f, line in lines:
        if f > end_frame:
            break
        m = re.match(r"ctf: f\d+ (red|blue) flag home \((.*)\)", line)
        if m:
            t = 0 if m.group(1) == "red" else 1
            why = m.group(2)
            if f > 2:
                flag_event(f)
            if flag_free_since[t] is not None:
                free_times.append((f - flag_free_since[t]) / FPS)
                flag_free_since[t] = None
            if why in ("timer", "respawn", "knocked off its stand"):
                throw_outcome(t, "home")
            flag_home[t] = True
            flag_holder[t] = -1
            set_held(t, f, False)
            update_both(f)
            continue
        m = re.match(r"ctf: f\d+ capture (red|blue) \((carried|thrown)\)", line)
        if m:
            caps[m.group(1)] += 1
            if m.group(2) == "carried":
                carried += 1
            else:
                thrown += 1
                throw_outcome(1 if m.group(1) == "red" else 0, "scored")
            if first_cap is None:
                first_cap = f
            ct = 0 if m.group(1) == "red" else 1
            if m.group(2) == "carried" and trip_pick[ct] is not None:
                trips_back.append((f - trip_pick[ct]) / FPS)
            trip_pick[ct] = None
            trip_start[ct] = f
            flag_event(f)
            continue
        m = re.match(r"ctfev: f\d+ pickup p(\d) team(\d) flag(\d)", line)
        if m:
            p, team, t = int(m.group(1)), int(m.group(2)), int(m.group(3))
            if team != t:
                pickups_enemy += 1
                if trip_start[team] is not None:
                    trips_out.append((f - trip_start[team]) / FPS)
                    trip_start[team] = None
                if trip_pick[team] is None:
                    trip_pick[team] = f
                if pending_throw[t] is not None:
                    throw_outcome(t, "pass" if team == pending_throw[t][0] and p != pending_throw[t][1] else "self")
            if flag_free_since[t] is not None and team != t:
                flag_free_since[t] = None  # picked up again by the enemy: not a lie before return
            flag_home[t] = False
            flag_holder[t] = p
            set_held(t, f, True)
            flag_event(f)
            update_both(f)
            continue
        m = re.match(r"ctfev: f\d+ return p(\d) flag(\d)", line)
        if m:
            returns += 1
            t = int(m.group(2))
            throw_outcome(t, "handed")
            continue
        m = re.match(r"ctfev: f\d+ release p(\d) flag(\d) (throw|drop)", line)
        if m:
            p, t = int(m.group(1)), int(m.group(2))
            flag_holder[t] = -1
            set_held(t, f, False)
            trip_pick[t ^ 1] = None
            if flag_free_since[t] is None:
                flag_free_since[t] = f
            if m.group(3) == "throw" and teams[p] is not None and teams[p] != t:
                pending_throw[t] = (teams[p], p)
            flag_event(f)
            continue
        m = re.match(r"ctfev: f\d+ ko p(\d) team\d sd(\d)", line)
        if m:
            kos.append(int(m.group(1)))
            if m.group(2) == "1":
                sds.append(int(m.group(1)))
            continue
        m = re.match(r"ctfev: f\d+ stuck p(\d)", line)
        if m:
            stuck.append(int(m.group(1)))
            continue
        m = re.match(r"ctfev: f\d+ role p(\d) (\w+)", line)
        if m:
            roles[int(m.group(1))] = m.group(2)
            role_changes += 1
            continue
        m = re.match(r"ctfpos: f\d+ (.*)", line)
        if m:
            for pm in re.finditer(r"p(\d) (-?\d+) (-?\d+) (\d)", m.group(1)):
                p = int(pm.group(1))
                last_pos[p] = (float(pm.group(2)), float(pm.group(3)), pm.group(4) == "1")
            if flag_home[0] and flag_home[1] and f >= go_frame:
                for team in (0, 1):
                    defenders = [p for p, r in roles.items() if r == "defend" and teams[p] == team]
                    if not defenders:
                        continue
                    defend_total[team] += 1
                    sx, sy = STANDS[team]
                    for p in defenders:
                        if p in last_pos and not last_pos[p][2]:
                            x, y, _ = last_pos[p]
                            if (x - sx) ** 2 + (y - sy) ** 2 <= DEFEND_RADIUS**2:
                                defend_in[team] += 1
                                break
            continue
    # Close the open intervals at the end.
    for t in (0, 1):
        set_held(t, end_frame, False)
        if flag_free_since[t] is not None:
            free_times.append((end_frame - flag_free_since[t]) / FPS)
        if pending_throw[t] is not None:
            throws["open"] += 1
    if both_away_since is not None and end_frame - last_flag_event > 60 * FPS:
        deadlock += end_frame - last_flag_event
    played = max(1, end_frame - go_frame)
    minutes = played / FPS / 60.0
    n_cpu = max(1, len(cpus))
    per5 = 5.0 / minutes / n_cpu
    total_caps = caps["red"] + caps["blue"]
    return {
        "minutes": round(minutes, 2),
        "captures": total_caps,
        "captures_red": caps["red"],
        "captures_blue": caps["blue"],
        "carried": carried,
        "thrown": thrown,
        "captures_per_min": round(total_caps / minutes, 3),
        "first_capture_s": None if first_cap is None else round((first_cap - go_frame) / FPS, 1),
        "enemy_pickups": pickups_enemy,
        "held_pct": round(100.0 * sum(held_frames) / 2 / played, 1),
        "returns": returns,
        "free_lie_s": [round(x, 1) for x in free_times],
        "free_lie_median_s": round(statistics.median(free_times), 1) if free_times else None,
        "stuck": len(stuck),
        "stuck_per_bot_5min": round(len(stuck) * per5, 2),
        "stuck_by_port": {str(p): stuck.count(p) for p in sorted(set(stuck))},
        "kos": len(kos),
        "sd": len(sds),
        "sd_per_bot_5min": round(len(sds) * per5, 2),
        "sd_by_port": {str(p): sds.count(p) for p in sorted(set(sds))},
        "deadlock_pct": round(100.0 * deadlock / played, 1),
        "throws": throws,
        "role_changes": role_changes,
        "trips_out_s": [round(x, 1) for x in trips_out],
        "trips_back_s": [round(x, 1) for x in trips_back],
        "defend_near_pct": [None if defend_total[t] == 0 else round(100.0 * defend_in[t] / defend_total[t], 1)
                            for t in (0, 1)],
    }


def run_one(args, seed, work):
    vs, teams = MODES[args.mode]
    if args.teams:
        teams = args.teams
    slots = args.slots or ("c" * len(teams))
    out = os.path.join(work, f"{args.tag}-s{seed}.txt")
    env = dict(os.environ)
    for k in [k for k in env if k.startswith("MELEE_DEBUG") or k.startswith("MELEE_NET_")]:
        env.pop(k)
    env.update(DISPLAY=os.environ.get("DISPLAY", ":0"),
               MELEE_BOOT_SCENE="vs", MELEE_DEBUG_VS=vs, MELEE_DEBUG_CTF="1", MELEE_DEBUG_CTF_AI_LOG="1",
               MELEE_DEBUG_CTF_TEAMS=teams, MELEE_DEBUG_VS_SLOTS=slots,
               MELEE_DEBUG_VS_CPU_LEVEL=str(args.level), MELEE_DEBUG_VS_TIME=str(args.minutes),
               MELEE_SEED=str(seed), MELEE_NO_CARD="1", MELEE_UNPACED="1", MELEE_VSYNC="0",
               MELEE_DRAW_EVERY=str(args.draw_every), MELEE_WINDOW_TITLE=f"ctf-ai-{args.tag}-{seed}",
               MELEE_LOG_FILE=os.path.join(work, f"{args.tag}-s{seed}.log"))
    if "XAUTHORITY" not in env:
        xa = glob.glob(f"/run/user/{os.getuid()}/xauth_*")
        if xa:
            env["XAUTHORITY"] = xa[0]
    if args.data:
        env["XDG_DATA_HOME"] = args.data
    chars = args.chars
    if args.cast and not chars:
        # A fixed cast rotated by seed: port p plays cast[(seed + p) % n].
        cast = [int(c, 0) for c in args.cast.split(",")]
        chars = ",".join(str(cast[(seed + p) % len(cast)]) for p in range(4))
    if chars:
        env["MELEE_DEBUG_VS_CHARS"] = chars
    if args.synctest:
        env["MELEE_NET_SYNCTEST"] = "1"
    if args.record:
        env["MELEE_NET_RECORD"] = args.record
    if args.replay:
        env["MELEE_NET_REPLAY"] = args.replay
    for kv in args.env:
        k, _, v = kv.partition("=")
        env[k] = v
    fifo = None
    if args.script:
        fifo = os.path.join(work, f"keys-{seed}")
        if os.path.exists(fifo):
            os.unlink(fifo)
        os.mkfifo(fifo)
        env["MELEE_KEY_FIFO"] = fifo

    t0 = time.time()
    with open(out, "w") as f:
        proc = subprocess.Popen([args.exe, "--no-card", args.disc], env=env, stdout=f, stderr=subprocess.STDOUT)
    status = "timeout"
    try:
        if fifo:
            with open(fifo, "w") as kf:
                kf.write(open(args.script).read())
        last_change, last_frame = time.time(), -1
        match_frames = args.minutes * 3600
        while time.time() - t0 < args.timeout:
            if proc.poll() is not None:
                status = "exited"
                break
            time.sleep(1.0)
            text = open(out, errors="replace").read()
            frames = [int(m) for m in re.findall(r"ctf(?:pos|ev)?: f(\d+) ", text[-20000:])]
            frame = max(frames) if frames else -1
            if frame != last_frame:
                last_frame, last_change = frame, time.time()
            if " wins\n" in text:
                status = "won"
                time.sleep(0.5)
                break
            ts = re.findall(r"ctfpos: f\d+ t(\d+)", text[-20000:])
            if ts and int(ts[-1]) + 30 >= match_frames:
                status = "time"
                break
            if args.replay and ("replay finished" in text or "DIVERGED" in text):
                status = "replay"
                break
            if frame > 0 and time.time() - last_change > 30:
                status = "stalled"
                break
    finally:
        proc.kill()
        proc.wait()
    text = open(out, errors="replace").read()
    lines = parse(text)
    last = max([f for f, _ in lines], default=0)
    go = GO_FRAME
    for f, line in lines:
        mt = re.match(r"ctfpos: f\d+ t(\d+)", line)
        if mt and int(mt.group(1)) > 0:
            go = f - int(mt.group(1))
            break
    end_frame = go + args.minutes * 3600
    m = re.search(r"ctf: f(\d+) (red|blue) wins", text)
    stop = int(m.group(1)) if m else min(last, end_frame)
    team_by_port = [None] * 4
    cpus = []
    for p, (slot, team) in enumerate(zip(slots, teams)):
        if slot in "ch":
            team_by_port[p] = int(team)
        if slot == "c":
            cpus.append(p)
    res = metrics(lines, team_by_port, cpus, stop, go)
    # How far throws fly: each "flag thrown at" to the next "flag landed at"
    # (itctfflag.c; a throw that leaves the stage has no landing).
    flights = []
    thrown_at = None
    for mm in re.finditer(r"ctf: flag (thrown|landed) at (-?[\d.]+) (-?[\d.]+)|ctf: f\d+ \w+ flag (home)", text):
        if mm.group(4):
            thrown_at = None  # scored or lost before it landed
        elif mm.group(1) == "thrown":
            thrown_at = (float(mm.group(2)), float(mm.group(3)))
        elif thrown_at is not None:
            flights.append((round(float(mm.group(2)) - thrown_at[0], 1), round(float(mm.group(3)) - thrown_at[1], 1)))
            thrown_at = None
    res["throw_flights"] = flights
    res.update(seed=seed, status=status, wall_s=round(time.time() - t0, 1), end_frame=stop, go_frame=go, chars=chars,
               winner=m.group(2) if m else None)
    if args.synctest:
        mism = [int(x) for x in re.findall(r"net: synctest frame \d+, (\d+) mismatches", text)]
        res["synctest_frames"] = max([int(x) for x in re.findall(r"net: synctest frame (\d+),", text)], default=0)
        res["synctest_mismatches"] = mism[-1] if mism else None
    if args.replay:
        res["replay_diverged"] = "DIVERGED" in text
        mm = re.search(r"replay finished at frame (\d+)", text)
        res["replay_frames"] = int(mm.group(1)) if mm else None
    return res


def summarize(runs):
    keys = ["captures", "captures_per_min", "carried", "thrown", "enemy_pickups", "held_pct", "returns",
            "stuck_per_bot_5min", "sd_per_bot_5min", "deadlock_pct", "kos"]
    s = {"runs": len(runs)}
    for k in keys:
        vals = [r[k] for r in runs if r.get(k) is not None]
        s[k] = round(statistics.mean(vals), 3) if vals else None
    s["matches_with_pickup"] = sum(1 for r in runs if r["enemy_pickups"] > 0)
    s["matches_with_capture"] = sum(1 for r in runs if r["captures"] > 0)
    firsts = [r["first_capture_s"] for r in runs if r["first_capture_s"] is not None]
    s["first_capture_median_s"] = round(statistics.median(firsts), 1) if firsts else None
    lies = [x for r in runs for x in r["free_lie_s"]]
    s["free_lie_median_s"] = round(statistics.median(lies), 1) if lies else None
    s["free_flags"] = len(lies)
    for k in ("trips_out_s", "trips_back_s"):
        v = [x for r in runs for x in r.get(k, [])]
        s[k[:-2] + "_median_s"] = round(statistics.median(v), 1) if v else None
        s[k[:-2] + "_n"] = len(v)
    s["matches_with_trip"] = sum(1 for r in runs if r.get("trips_back_s"))
    th = {}
    for r in runs:
        for k, v in r["throws"].items():
            th[k] = th.get(k, 0) + v
    s["throws"] = th
    dn = [[r["defend_near_pct"][t] for r in runs if r["defend_near_pct"][t] is not None] for t in (0, 1)]
    s["defend_near_pct"] = [round(statistics.mean(d), 1) if d else None for d in dn]
    wins = {"red": 0, "blue": 0, None: 0}
    for r in runs:
        w = r["winner"]
        if w is None:
            w = "red" if r["captures_red"] > r["captures_blue"] else "blue" if r["captures_blue"] > r[
                "captures_red"] else None
        wins[w] += 1
    s["wins"] = {"red": wins["red"], "blue": wins["blue"], "tie": wins[None]}
    s["statuses"] = sorted(set(r["status"] for r in runs))
    if any("synctest_mismatches" in r for r in runs):
        s["synctest_mismatches"] = [r.get("synctest_mismatches") for r in runs]
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", default=os.path.join(REPO, "build/linux/melee"))
    ap.add_argument("--disc", default=DISC)
    ap.add_argument("--data", help="XDG_DATA_HOME for the runs (keeps the real launcher.cfg untouched)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--tag", default=None)
    ap.add_argument("--mode", default="2v2", choices=sorted(MODES))
    ap.add_argument("--teams", default=None)
    ap.add_argument("--slots", default=None)
    ap.add_argument("--chars", default=None, help="MELEE_DEBUG_VS_CHARS, CharacterKind numbers")
    ap.add_argument("--cast", default=CAST,
                    help="characters rotated by seed (default: Fox, Peach, DK, Marth, Mario, Link); "
                         "'' for the debug match's Link, Mario, Fox, DK")
    ap.add_argument("--level", default="9", help="1-9, or one per port: 9,1,9,1")
    ap.add_argument("--minutes", type=int, default=3)
    ap.add_argument("--seeds", default="10", help="N (seeds 1..N) or a list: 3,7,9 or a range 11-20")
    ap.add_argument("--draw-every", type=int, default=10)
    ap.add_argument("--timeout", type=float, default=400)
    ap.add_argument("--script", help="a frame script for port 1 (MELEE_KEY_FIFO)")
    ap.add_argument("--synctest", action="store_true")
    ap.add_argument("--record")
    ap.add_argument("--replay")
    ap.add_argument("--env", action="append", default=[], help="K=V, more environment")
    args = ap.parse_args()
    if "-" in args.seeds:
        a, b = args.seeds.split("-")
        seeds = list(range(int(a), int(b) + 1))
    elif "," in args.seeds:
        seeds = [int(x) for x in args.seeds.split(",")]
    else:
        seeds = list(range(1, int(args.seeds) + 1))
    args.tag = args.tag or f"{args.mode}-l{args.level.replace(',', '')}"
    work = args.out or tempfile.mkdtemp(prefix="ctf_ai_")
    os.makedirs(work, exist_ok=True)
    runs = []
    for seed in seeds:
        r = run_one(args, seed, work)
        runs.append(r)
        with open(os.path.join(work, f"{args.tag}-s{seed}.json"), "w") as f:
            json.dump(r, f, indent=1)
        extra = ""
        if args.synctest:
            extra = f" synctest {r.get('synctest_mismatches')} mismatches over {r.get('synctest_frames')} frames"
        if args.replay:
            extra = f" replay {'DIVERGED' if r.get('replay_diverged') else 'identical'} ({r.get('replay_frames')})"
        print(f"seed {seed}: {r['status']} {r['wall_s']}s caps {r['captures_red']}-{r['captures_blue']} "
              f"(thrown {r['thrown']}) pickups {r['enemy_pickups']} returns {r['returns']} "
              f"stuck {r['stuck']} sd {r['sd']}/{r['kos']} deadlock {r['deadlock_pct']}% "
              f"first {r['first_capture_s']}{extra}", flush=True)
    s = summarize(runs)
    s.update(mode=args.mode, level=args.level, minutes=args.minutes, chars=args.chars, seeds=seeds, tag=args.tag)
    with open(os.path.join(work, f"{args.tag}.json"), "w") as f:
        json.dump({"summary": s, "runs": runs}, f, indent=1)
    print(json.dumps(s, indent=1))
    bad = [r for r in runs if r["status"] in ("timeout", "stalled", "exited")]
    if args.synctest and any(r.get("synctest_mismatches") != 0 for r in runs):
        return 1
    if args.replay and any(r.get("replay_diverged") for r in runs):
        return 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
