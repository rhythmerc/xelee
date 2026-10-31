#!/usr/bin/env python3
"""Drive the XeleeNet lobby service from a terminal (tools/xeleenet).

A headless stand-in for the game's side of the API: host a lobby and watch
who joins, list lobbies, join by code. tools/xeleenet/test_service.py uses
the XnClient class below for its end-to-end run.

    xn_cli.py keygen                         make this CLI's key; print its tag
    xn_cli.py host [--private] [--ask] ...   host, then heartbeat until Ctrl-C
    xn_cli.py list [--full]
    xn_cli.py join K7M-Q2D

The service URL comes from --url, $XELEENET_URL, ~/.config/xelee/xeleenet-url,
or http://127.0.0.1:8787 (a local service). The key is a 32-byte Ed25519 seed,
the same form as the game's identity file; --key picks the file.

Needs the `cryptography` package for Ed25519.
"""
import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat

CONFIG = os.path.expanduser("~/.config/xelee")
DEFAULT_KEY = os.path.join(CONFIG, "xn-cli.key")
# Stands in for a build: lobbies only match the exact same one.
CLI_COMPAT = {"proto": 12, "rev": "xn-cli", "disc": "GALE01"}
DEFAULT_SETTINGS = {
    "preset": "1v1", "seats": 2, "mode": "ffa", "win": "stock", "stocks": 4, "time_min": 8,
    "items": "off", "stage_list": "competitive", "pick": "loser", "format": "first_to",
    "first_to": 2, "place_secs": 15,
}


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()


class XnError(Exception):
    def __init__(self, status, body):
        self.status = status
        self.body = body if isinstance(body, dict) else {"error": str(body)}
        self.reason = self.body.get("reason", "")
        super().__init__(f"{status} {self.reason}: {self.body.get('error', '')}")


class XnClient:
    """One player: a key, a session token, and the API calls."""

    def __init__(self, url, seed=None, name="CLI", compat=None):
        self.url = url.rstrip("/")
        self.key = Ed25519PrivateKey.from_private_bytes(seed) if seed else Ed25519PrivateKey.generate()
        raw = self.key.public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)
        self.pubkey = b64url(raw)
        self.name = name
        self.compat = dict(compat or CLI_COMPAT)
        self.token = None
        self.tag = None

    def call(self, method, path, body=None, auth=True, timeout=40):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.url + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        if auth:
            if not self.token:
                self.sign_in()
            req.add_header("Authorization", "Bearer " + self.token)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                payload = json.loads(raw)
            except ValueError:
                payload = raw.decode(errors="replace")
            raise XnError(e.code, payload) from None

    def sign_in(self):
        nonce = self.call("POST", "/v1/session", {"pubkey": self.pubkey}, auth=False)["nonce"]
        sig = self.key.sign(f"xeleenet-session-v1\n{nonce}\n{self.pubkey}".encode())
        r = self.call("POST", "/v1/session/verify",
                      {"pubkey": self.pubkey, "nonce": nonce, "sig": b64url(sig)}, auth=False)
        self.token, self.tag = r["token"], r["tag"]
        return r

    def list(self, full=False):
        c = self.compat
        q = f"?proto={c['proto']}&rev={c['rev']}&disc={c['disc']}" + ("&full=1" if full else "")
        return self.call("GET", "/v1/lobbies" + q)

    def host(self, name, candidates, visibility="public", ask=False, settings=None, xr=False):
        return self.call("POST", "/v1/lobbies", {
            "name": name, "player": self.name, "visibility": visibility, "ask": ask, "xr": xr,
            "compat": self.compat, "settings": settings or DEFAULT_SETTINGS, "candidates": candidates,
        })

    def heartbeat(self, lobby, roster=(), state="open", score=None, xr=False, extra=None, owners=None):
        body = {"roster": list(roster), "state": state, "xr": xr}
        if extra is not None:
            body["extra"] = list(extra)  # ports machines' other pads hold
        if owners is not None:
            body["owners"] = list(owners)  # each one's machine, by its port
        if score is not None:
            body["score"] = score
        return self.call("POST", f"/v1/lobbies/{lobby}/heartbeat", body)

    def settings(self, lobby, name, settings, visibility="public", ask=False):
        return self.call("PUT", f"/v1/lobbies/{lobby}/settings",
                         {"name": name, "visibility": visibility, "ask": ask, "settings": settings})

    def admit(self, lobby, key, accept=True):
        return self.call("POST", f"/v1/lobbies/{lobby}/admit", {"key": key, "accept": accept})

    def kick(self, lobby, key):
        return self.call("POST", f"/v1/lobbies/{lobby}/kick", {"key": key})

    def close(self, lobby):
        return self.call("DELETE", f"/v1/lobbies/{lobby}")

    def join(self, code, candidates, xr=False):
        return self.call("POST", "/v1/join", {
            "code": code, "player": self.name, "xr": xr, "compat": self.compat, "candidates": candidates,
        })

    def join_status(self, lobby):
        return self.call("GET", f"/v1/lobbies/{lobby}/join")

    def leave(self, lobby):
        return self.call("POST", f"/v1/lobbies/{lobby}/leave", {})


def default_url():
    if os.environ.get("XELEENET_URL"):
        return os.environ["XELEENET_URL"]
    path = os.path.join(CONFIG, "xeleenet-url")
    if os.path.exists(path):
        with open(path) as f:
            return f.read().strip()
    return "http://127.0.0.1:8787"


def load_seed(path, create):
    if os.path.exists(path):
        with open(path, "rb") as f:
            seed = f.read()
        if len(seed) != 32:
            sys.exit(f"{path}: not a 32-byte key")
        return seed
    if not create:
        sys.exit(f"no key at {path}; run `xn_cli.py keygen` first")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    seed = os.urandom(32)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(seed)
    return seed


def fmt_code(code):
    return f"{code[:3]}-{code[3:]}"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--url", default=default_url())
    ap.add_argument("--key", default=DEFAULT_KEY)
    ap.add_argument("--name", default="CLI", help="player name (1-12 chars)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keygen")
    h = sub.add_parser("host")
    h.add_argument("--lobby", default=None, help="lobby name (default: <name>'s lobby)")
    h.add_argument("--private", action="store_true")
    h.add_argument("--ask", action="store_true", help="Ask before joining; answer on the terminal")
    h.add_argument("--seats", type=int, default=2)
    h.add_argument("--teams", action="store_true")
    h.add_argument("--addr", action="append", default=None, help="an address to publish (repeatable)")
    l_ = sub.add_parser("list")
    l_.add_argument("--full", action="store_true")
    j = sub.add_parser("join")
    j.add_argument("code")
    j.add_argument("--addr", action="append", default=None)
    args = ap.parse_args()

    seed = load_seed(args.key, create=args.cmd == "keygen")
    c = XnClient(args.url, seed, name=args.name)
    if args.cmd == "keygen":
        print(f"{args.name}#{c.sign_in()['tag']}  ({args.key})")
        return
    try:
        if args.cmd == "list":
            r = c.list(full=args.full)
            for lob in r["lobbies"]:
                s = lob["settings"]
                print(f"{fmt_code(lob['code'])}  {lob['taken']}/{s['seats']}  {lob['name']:<20}  "
                      f"{lob['host']['name']}#{lob['host']['tag']}  {s['mode']}  {lob['state']}  {lob['country']}")
            if r["other_versions"]:
                print(f"({r['other_versions']} on other versions)")
        elif args.cmd == "join":
            r = c.join(args.code, args.addr or ["192.0.2.2:41000"])
            while r.get("status") == "pending":
                print("waiting for the host to let you in…")
                r = c.join_status(r["id"])
            print(json.dumps(r, indent=2))
        elif args.cmd == "host":
            s = dict(DEFAULT_SETTINGS, seats=args.seats, mode="teams" if args.teams else "ffa",
                     preset="custom", pick="everyone" if args.seats > 2 and not args.teams else "loser")
            r = c.host(args.lobby or f"{args.name}'s lobby", args.addr or ["192.0.2.1:41000"],
                       visibility="private" if args.private else "public", ask=args.ask, settings=s)
            lobby = r["id"]
            print(f"hosting {fmt_code(r['code'])} ({lobby}); Ctrl-C closes it")
            # The CLI has no peer links, so it reports every member as reached.
            members, known, asked = [], set(), set()
            try:
                while True:
                    hb = c.heartbeat(lobby, roster=[m["key"] for m in members])
                    for m in hb["members"]:
                        if m["key"] not in known:
                            known.add(m["key"])
                            print(f"joined: P{m['port'] + 1} {m['name']}#{m['tag']} at {', '.join(m['candidates'])}")
                    for p in hb["pending"]:
                        if p["key"] not in asked:
                            asked.add(p["key"])
                            yes = input(f"{p['name']}#{p['tag']} wants to join; let in? [y/N] ").lower() == "y"
                            print(c.admit(lobby, p["key"], yes)["result"])
                    members = hb["members"]
                    time.sleep(2)
            except KeyboardInterrupt:
                c.close(lobby)
                print("\nclosed")
    except XnError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
