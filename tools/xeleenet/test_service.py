#!/usr/bin/env python3
"""End-to-end test of the XeleeNet lobby service against `wrangler dev`.

Starts the Worker locally (Durable Objects included) with short timeouts,
plays hosts and joiners through tools/xn_cli.py's XnClient, and checks every
answer. Needs `npm ci` in tools/xeleenet and the `cryptography` package.

    python3 tools/xeleenet/test_service.py            # start wrangler dev, run, stop
    python3 tools/xeleenet/test_service.py --url URL  # against a running service

The rate limits aren't covered: wrangler dev doesn't enforce them.
"""
import argparse
import os
import shutil
import socket
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


def start_wrangler(short=True):
    """wrangler dev on a free port. `short`: this test's short timeouts and
    word list; otherwise the service's own (tools/net_test_n.py --xn)."""
    port = free_port()
    state = tempfile.mkdtemp(prefix="xn-test-")
    cmd = ["npx", "wrangler", "dev", "--ip", "127.0.0.1", "--port", str(port), "--persist-to", state,
           "--var", "TOKEN_KEY:test-token-key"]
    if short:
        cmd += ["--var", f"EXPIRE_SECS:{EXPIRE}", "--var", f"JOIN_WAIT_SECS:{JOIN_WAIT}",
                "--var", f"ROSTER_GRACE_SECS:{GRACE}", "--var", f"NAME_BLOCKLIST:{BLOCKED}"]
    env = dict(os.environ, WRANGLER_SEND_METRICS="false", CI="1")
    log = open(os.path.join(state, "wrangler.log"), "w")
    proc = subprocess.Popen(cmd, cwd=HERE, stdout=log, stderr=subprocess.STDOUT, env=env, start_new_session=True)
    url = f"http://127.0.0.1:{port}"
    for _ in range(120):
        if proc.poll() is not None:
            break
        try:
            urllib.request.urlopen(urllib.request.Request(url + "/v1/nothing"), timeout=2)
        except urllib.error.HTTPError:
            return proc, url, state  # it answers (404): ready
        except OSError:
            time.sleep(0.5)
    log.close()
    print(open(os.path.join(state, "wrangler.log")).read())
    sys.exit("wrangler dev didn't come up")


def stop_wrangler(proc, state):
    try:
        os.killpg(proc.pid, 15)
        proc.wait(timeout=10)
    except Exception:
        os.killpg(proc.pid, 9)
    if failures:
        print(f"wrangler log: {os.path.join(state, 'wrangler.log')}")
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--url", help="test a running service instead of starting wrangler dev")
    args = ap.parse_args()
    proc = state = None
    url = args.url
    if not url:
        proc, url, state = start_wrangler()
    try:
        run(url)
    finally:
        if proc:
            stop_wrangler(proc, state)
    print(f"\n{len(failures)} failed" if failures else "\nall passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
