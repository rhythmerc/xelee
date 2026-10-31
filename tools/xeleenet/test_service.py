#!/usr/bin/env python3
"""End-to-end test of the XeleeNet lobby service.

Builds and starts the service locally with short timeouts, plays hosts and
joiners through tools/xn_cli.py's XnClient, and checks every answer; then
the rate limits, STUN, and a restart that keeps the lobbies. Needs Go and the
`cryptography` package.

    python3 tools/xeleenet/test_service.py            # build, start, run, stop
    python3 tools/xeleenet/test_service.py --url URL  # the API part against a running service
"""
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from xn_cli import CLI_COMPAT, DEFAULT_SETTINGS, XnClient, XnError  # noqa: E402

EXPIRE = 3
JOIN_WAIT = 2
GRACE = 1
BLOCKED = "darnit"

failures = []


def check(cond, what):
    print(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        failures.append(what)


def refused(fn, reason, what):
    try:
        r = fn()
    except XnError as e:
        check(e.reason == reason, f"{what}: refused with {e.reason} (want {reason})")
        return e
    check(False, f"{what}: answered {r} (want refusal {reason})")
    return None


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def go_tool():
    """The Go toolchain: $GO, `go` on PATH, or ~/.local/opt/go (README.md)."""
    for g in (os.environ.get("GO"), shutil.which("go"), os.path.expanduser("~/.local/opt/go/bin/go")):
        if g and os.access(g, os.X_OK):
            return g
    sys.exit("no Go toolchain: install Go 1.27+ or set $GO")


def build(out_dir):
    exe = os.path.join(out_dir, "xeleenet")
    subprocess.run([go_tool(), "build", "-o", exe, "."], cwd=HERE, check=True,
                   env=dict(os.environ, CGO_ENABLED="0"))
    return exe


def start_service(short=True, limits=False, stun=False, host="127.0.0.1", port=None):
    """The service on a free port. `short`: this test's short timeouts and
    word list; otherwise the service's own (tools/net_test_n.py --xn).
    `limits` turns the rate limits on (off by default: one machine plays
    every player). Returns (proc, url, state dir[, stun port])."""
    port = port or free_port()
    state = tempfile.mkdtemp(prefix="xn-test-")
    cmd = [build(state), "-listen", f"{host}:{port}"]
    if short:
        cmd += ["-expire", f"{EXPIRE}s", "-join-wait", f"{JOIN_WAIT}s", "-roster-grace", f"{GRACE}s"]
    if not limits:
        cmd += ["-limit-ip", "0", "-limit-host", "0", "-limit-join", "0"]
    stun_port = None
    if stun:
        stun_port = free_port()
        cmd += ["-stun", f"127.0.0.1:{stun_port}"]
    env = dict(os.environ, XELEENET_TOKEN_KEY=os.environ.get("XELEENET_TOKEN_KEY", "test-token-key"))
    if short:
        env["XELEENET_NAME_BLOCKLIST"] = BLOCKED
    log = open(os.path.join(state, "service.log"), "w")
    proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    url = f"http://127.0.0.1:{port}"
    for _ in range(100):
        if proc.poll() is not None:
            break
        try:
            urllib.request.urlopen(url + "/healthz", timeout=2)
            return (proc, url, state, stun_port) if stun else (proc, url, state)
        except OSError:
            time.sleep(0.1)
    log.close()
    print(open(os.path.join(state, "service.log")).read())
    sys.exit("the lobby service didn't come up")


def stop_service(proc, state):
    try:
        os.killpg(proc.pid, 15)
        proc.wait(timeout=10)
    except Exception:
        os.killpg(proc.pid, 9)
    if failures:
        print(f"service log: {os.path.join(state, 'service.log')}")
    else:
        shutil.rmtree(state, ignore_errors=True)


def run(url):
    def player(name, compat=None):
        return XnClient(url, name=name, compat=compat)

    print("-- sign in")
    a = player("RHYTHM")
    r = a.sign_in()
    check(len(r["tag"]) == 8 and r["token"], "sign in gives a token and an 8-char tag")
    bad = player("MALLORY")
    nonce = bad.call("POST", "/v1/session", {"pubkey": bad.pubkey}, auth=False)["nonce"]
    refused(lambda: bad.call("POST", "/v1/session/verify", {"pubkey": bad.pubkey, "nonce": nonce,
            "sig": "A" * 86}, auth=False), "unauthorized", "a wrong signature")
    other = player("OTHER")
    refused(lambda: other.call("POST", "/v1/session/verify", {"pubkey": other.pubkey, "nonce": nonce,
            "sig": "A" * 86}, auth=False), "unauthorized", "someone else's nonce")
    bad.token = a.token[:-2] + ("AA" if not a.token.endswith("AA") else "BB")
    refused(lambda: bad.list(), "unauthorized", "a forged token")

    print("-- host and list")
    four = dict(DEFAULT_SETTINGS, preset="doubles", seats=4, mode="teams")
    r = a.host("Friday netplay", ["198.51.100.1:41000", "192.168.1.10:41000"], settings=four, xr=True)
    lobby, code = r["id"], r["code"]
    check(len(code) == 6 and "0" not in code and "O" not in code and "1" not in code and "I" not in code,
          f"a 6-char code without look-alikes ({code})")
    b = player("PIXEL")
    rows = b.list()["lobbies"]
    mine = [x for x in rows if x["code"] == code]
    check(len(mine) == 1 and mine[0]["taken"] == 1 and mine[0]["xr"] and mine[0]["host"]["tag"] == a.tag,
          "the browser lists it: 1 taken, XR, host tag")
    check("candidates" not in str(mine), "the browser never shows addresses")
    stranger = player("OLDBUILD", compat=dict(CLI_COMPAT, rev="older"))
    r = stranger.list()
    check(all(x["code"] != code for x in r["lobbies"]) and r["other_versions"] >= 1,
          "another build doesn't see it, only counts it")
    refused(lambda: stranger.join(code, ["203.0.113.9:41000"]), "version", "another build joining")

    print("-- joining")
    r = b.join(code.lower()[:3] + "-" + code.lower()[3:], ["203.0.113.2:41000"])
    check(r["status"] == "joined" and r["port"] == 1, "a code in lower case with a dash seats P2")
    check([m["port"] for m in r["members"]] == [0] and r["members"][0]["candidates"][0] == "198.51.100.1:41000",
          "the joiner gets the host's addresses")
    r2 = b.join(code, ["203.0.113.2:41001"])
    check(r2["status"] == "joined" and r2["port"] == 1, "joining again keeps the seat")
    hb = a.heartbeat(lobby, roster=[])
    check([m["name"] for m in hb["members"]] == ["PIXEL"] and hb["members"][0]["candidates"] == ["203.0.113.2:41001"],
          "the host's heartbeat shows the newcomer with fresh addresses")
    c = player("KOOPA")
    d = player("WAVE")
    check(c.join(code, ["203.0.113.3:41000"])["port"] == 2, "third player seats P3")
    r = d.join(code, ["203.0.113.4:41000"])
    check(r["port"] == 3 and sorted(m["port"] for m in r["members"]) == [0, 1, 2], "fourth seats P4 and sees all three")
    e = player("LATE")
    refused(lambda: e.join(code, ["203.0.113.5:41000"]), "full", "a fifth player")
    check(all(x["code"] != code for x in e.list()["lobbies"]), "a full lobby leaves the browser")
    check(any(x["code"] == code for x in e.list(full=True)["lobbies"]), "Show full brings it back")
    refused(lambda: a.join(code, ["198.51.100.1:41000"]), "own_lobby", "the host joining their own lobby")

    print("-- the fighter list")
    refused(lambda: a.settings(lobby, "Friday netplay", dict(four, fighter_list="top")),
            "bad_settings", "a fighter list that doesn't exist")
    a.settings(lobby, "Friday netplay", dict(four, fighter_list="low"))
    row = [x for x in e.list(full=True)["lobbies"] if x["code"] == code]
    check(row and row[0]["settings"]["fighter_list"] == "low", "the listing carries the fighter list")
    a.settings(lobby, "Friday netplay", four)
    row = [x for x in e.list(full=True)["lobbies"] if x["code"] == code]
    check(row and row[0]["settings"]["fighter_list"] == "all", "a game that sends none lists All")

    print("-- leaving, removing")
    b.leave(lobby)
    check(e.join(code, ["203.0.113.5:41000"])["port"] == 1, "a leaver's seat (P2) goes to the next joiner")
    a.kick(lobby, e.pubkey)
    refused(lambda: e.join(code, ["203.0.113.5:41000"]), "banned", "a removed player joining again")
    refused(lambda: c.kick(lobby, d.pubkey), "not_host", "a guest removing someone")
    refused(lambda: c.heartbeat(lobby), "not_host", "a guest heartbeating")

    print("-- settings")
    refused(lambda: a.settings(lobby, "Friday netplay", dict(four, seats=2, mode="ffa", preset="1v1")),
            "seats", "fewer seats than players")
    refused(lambda: a.settings(lobby, "Friday netplay", dict(four, mode="ffa", pick="loser")),
            "bad_settings", "loser picks in a 4-player free-for-all")
    refused(lambda: a.settings(lobby, f"the {BLOCKED.upper()} room", four), "bad_name", "a blocked word")
    refused(lambda: a.settings(lobby, "x" * 21, four), "bad_name", "a 21-char lobby name")
    refused(lambda: c.settings(lobby, "mine now", four), "not_host", "a guest changing settings")
    a.settings(lobby, "Friday netplay 2", four, visibility="private")
    check(all(x["code"] != code for x in d.list(full=True)["lobbies"]), "going private leaves the browser")

    print("-- the roster")
    time.sleep(GRACE + 0.5)
    hb = a.heartbeat(lobby, roster=[c.pubkey])
    check(sorted(m["name"] for m in hb["members"]) == ["KOOPA"], "players the host no longer reaches are dropped")

    print("-- seats a machine's other pads hold")
    a.heartbeat(lobby, roster=[c.pubkey], extra=[1])
    i = player("ITEM")
    check(i.join(code, ["203.0.113.8:41000"])["port"] == 3, "a joiner skips a port another pad holds (P2)")
    a.heartbeat(lobby, roster=[c.pubkey, i.pubkey], extra=[1])
    j = player("JUNK")
    refused(lambda: j.join(code, ["203.0.113.9:41000"]), "full", "a joiner when other pads fill the last seat")
    a.heartbeat(lobby, roster=[c.pubkey, i.pubkey], extra=[])
    check(j.join(code, ["203.0.113.9:41000"])["port"] == 1, "a seat another pad gave up goes to the next joiner")

    print("-- ask before joining")
    f = player("HOST2")
    r = f.host("Ask first", ["198.51.100.6:41000"], visibility="private", ask=True,
               settings=dict(DEFAULT_SETTINGS, preset="ffa", seats=4, pick="everyone"))
    ask_lobby, ask_code = r["id"], r["code"]
    check(all(x["code"] != ask_code for x in d.list(full=True)["lobbies"]), "a private lobby isn't listed")
    g, h = player("GUEST"), player("NOPE")
    result = {}

    def g_join():
        result["g"] = g.join(ask_code, ["203.0.113.7:41000"])

    t = threading.Thread(target=g_join)
    t.start()
    time.sleep(0.5)
    hb = f.heartbeat(ask_lobby)
    check([p["name"] for p in hb["pending"]] == ["GUEST"] and not hb["members"], "the host sees GUEST asking")
    f.admit(ask_lobby, g.pubkey, True)
    t.join(10)
    check(result.get("g", {}).get("status") == "joined" and result["g"]["port"] == 1,
          "admitting answers the waiting joiner at once")
    r = h.join(ask_code, ["203.0.113.8:41000"])
    check(r["status"] == "pending", f"no answer within {JOIN_WAIT} s gives 'pending'")
    f.admit(ask_lobby, h.pubkey, False)
    refused(lambda: h.join_status(ask_lobby), "declined", "a declined joiner asking again")
    refused(lambda: f.admit(ask_lobby, h.pubkey, True), "not_pending", "answering twice")

    print("-- one lobby per host, closing, expiry")
    r = a.host("Second", ["198.51.100.1:41000"])
    refused(lambda: d.join(code, ["203.0.113.4:41000"]), "not_found", "hosting again closes the host's old lobby")
    refused(lambda: c.leave(lobby), "closed", "a member of the closed lobby")
    second = r["code"]
    f.close(ask_lobby)
    refused(lambda: g.join(ask_code, ["203.0.113.7:41000"]), "not_found", "a closed lobby's code")
    r = h.host("Third", ["198.51.100.2:41000"])
    h.call("POST", f"/v1/lobbies/{r['id']}/close", {})
    refused(lambda: g.join(r["code"], ["203.0.113.7:41000"]), "not_found", "closing with POST (the game's way)")
    time.sleep(EXPIRE + 2.5)
    refused(lambda: d.join(second, ["203.0.113.4:41000"]), "not_found",
            f"a lobby without heartbeats for {EXPIRE} s expires")
    check(all(x["code"] != second for x in d.list(full=True)["lobbies"]), "and leaves the browser")
    # A host that heartbeats past the first alarm and then dies (a crash, a
    # kill): the alarm has re-armed itself on heartbeats, and must still
    # close the lobby once they stop (2026-10-29: dead hosts' lobbies stayed
    # listed for hours).
    r = g.host("Crashed", ["198.51.100.3:41000"])
    for _ in range(4):
        g.heartbeat(r["id"], roster=[])
        time.sleep(1)
    time.sleep(2 * EXPIRE + 3)
    check(all(x["code"] != r["code"] for x in d.list(full=True)["lobbies"]),
          f"a host's heartbeats stopping for {EXPIRE} s closes its lobby too")


def run_limits():
    print("-- rate limits")
    proc, url, state = start_service(limits=True)
    try:
        a = XnClient(url, name="FLOOD")
        a.sign_in()
        for i in range(20):
            refused(lambda: a.join("ZZZZZZ", ["203.0.113.9:41000"]), "not_found", "a guess") if i == 0 else \
                _quiet(lambda: a.join("ZZZZZZ", ["203.0.113.9:41000"]))
        refused(lambda: a.join("ZZZZZZ", ["203.0.113.9:41000"]), "rate_limited", "the 21st join in a minute")
        for i in range(6):
            a.host(f"Lobby {i}", ["198.51.100.1:41000"])
        refused(lambda: a.host("Lobby 7", ["198.51.100.1:41000"]), "rate_limited", "the 7th lobby in a minute")
    finally:
        stop_service(proc, state)


def _quiet(fn):
    try:
        fn()
    except XnError:
        pass


def run_stun():
    print("-- STUN")
    proc, url, state, stun_port = start_service(stun=True)
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.settimeout(2)
            sock.bind(("127.0.0.1", 0))
            txid = os.urandom(12)
            sock.sendto(struct.pack(">HHI", 1, 0, 0x2112A442) + txid, ("127.0.0.1", stun_port))
            reply = sock.recv(1500)
            mine = sock.getsockname()
        kind, length, cookie = struct.unpack(">HHI", reply[:8])
        check(kind == 0x0101 and cookie == 0x2112A442 and reply[8:20] == txid and length == len(reply) - 20,
              "a Binding request gets a success with our transaction id")
        atype, alen, _, family, xport = struct.unpack(">HHBBH", reply[20:28])
        port = xport ^ 0x2112
        ip = ".".join(str(b ^ k) for b, k in zip(reply[28:32], struct.pack(">I", 0x2112A442)))
        check(atype == 0x0020 and family == 1 and (ip, port) == mine,
              f"XOR-MAPPED-ADDRESS is our address ({ip}:{port})")
    finally:
        stop_service(proc, state)


def run_restart():
    print("-- a restart keeps the lobbies")
    port = free_port()
    url = f"http://127.0.0.1:{port}"
    a, b = XnClient(url, name="STAYS"), XnClient(url, name="LATER")
    state = tempfile.mkdtemp(prefix="xn-test-")
    saved = os.path.join(state, "state.json")
    exe = build(state)
    env = dict(os.environ, XELEENET_TOKEN_KEY="test-token-key")
    proc = None
    try:
        def up():
            p = subprocess.Popen([exe, "-listen", f"127.0.0.1:{port}", "-state", saved, "-expire", f"{EXPIRE}s"],
                                 env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            for _ in range(100):
                try:
                    urllib.request.urlopen(url + "/healthz", timeout=2)
                    return p
                except OSError:
                    time.sleep(0.1)
            sys.exit("the lobby service didn't come back")

        proc = up()
        r = a.host("Survivor", ["198.51.100.1:41000"])
        os.killpg(proc.pid, 15)
        proc.wait(timeout=10)
        check(os.path.exists(saved), "stopping writes the state file")
        proc = up()
        check(any(x["code"] == r["code"] for x in b.list()["lobbies"]), "the lobby is listed after the restart")
        check(a.heartbeat(r["id"])["state"] == "open", "and its host's token and heartbeat still work")
        check(b.join(r["code"], ["203.0.113.2:41000"])["port"] == 1, "and it can be joined")
    finally:
        stop_service(proc, state)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--url", help="test a running service (the API part only)")
    args = ap.parse_args()
    if args.url:
        run(args.url)
    else:
        proc, url, state = start_service()
        try:
            run(url)
        finally:
            stop_service(proc, state)
        run_limits()
        run_stun()
        run_restart()
    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
