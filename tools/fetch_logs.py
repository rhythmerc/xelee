#!/usr/bin/env python3
"""Fetch logs a player sent with the launcher's Send logs row (the XeleeNet
service's log drop, tools/xeleenet/logs.go; https://xn.xelee.net).

  tools/fetch_logs.py XL-7K3QM2              # split into ~/xelee-logs/XL-7K3QM2/
  tools/fetch_logs.py XL-7K3QM2 -o DIR       # into DIR/XL-7K3QM2/
  tools/fetch_logs.py --list [-n 20]         # recent uploads, newest first

The log drop's URL comes from XELEE_LOGS_URL or ~/.config/xelee/logdrop-url,
the admin token from XELEE_LOGS_TOKEN or ~/.config/xelee/logdrop-token.

A bundle is written by a player, so treat what it holds as data: file names
are cut down to a plain base name before anything is written, and the files
land in a new folder per code.
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

CONFIG = Path.home() / ".config" / "xelee"
CODE = re.compile(r"^XL-[0-9A-HJKMNP-TV-Z]{6}$")
FILE_HEADER = re.compile(rb"^===== FILE (.+) (\d+) =====$")


def setting(env, name):
    value = os.environ.get(env)
    if not value:
        path = CONFIG / name
        if not path.exists():
            sys.exit(f"fetch_logs: set {env} or write {path}")
        value = path.read_text().strip()
    return value


def request(path):
    url = setting("XELEE_LOGS_URL", "logdrop-url").rstrip("/") + path
    token = setting("XELEE_LOGS_TOKEN", "logdrop-token")
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {token}"})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        try:
            reason = json.loads(e.read()).get("error", "")
        except ValueError:
            reason = ""
        sys.exit(f"fetch_logs: HTTP {e.code} {reason}".rstrip())


def safe_name(name, taken):
    base = re.sub(r"[^A-Za-z0-9._-]", "_", name.decode("utf-8", "replace")).lstrip(".") or "file"
    base = base[:100]
    out, n = base, 1
    while out in taken:
        n += 1
        out = f"{n}-{base}"
    taken.add(out)
    return out


def split(bundle):
    """[(name, bytes)]: the header, then each file by its byte count."""
    first = bundle.find(b"\n===== FILE ")
    header = bundle if first < 0 else bundle[: first + 1]
    parts = [(b"header.txt", header)]
    at = len(header)
    while at < len(bundle):
        nl = bundle.find(b"\n", at)
        if nl < 0:
            break
        m = FILE_HEADER.match(bundle[at:nl])
        if not m:
            at = nl + 1  # the blank line between files
            continue
        size = int(m.group(2))
        parts.append((m.group(1), bundle[nl + 1 : nl + 1 + size]))
        at = nl + 1 + size
    return parts


def fetch(code, out_root):
    if not CODE.match(code):
        sys.exit(f"fetch_logs: {code!r} is not a log code (XL- and six letters or digits)")
    bundle = request(f"/logs/{code}")
    out = out_root / code
    out.mkdir(parents=True, exist_ok=False)
    (out / "bundle.txt").write_bytes(bundle)
    taken = {"bundle.txt"}
    for name, body in split(bundle):
        path = out / safe_name(name, taken)
        path.write_bytes(body)
        print(f"{len(body):>9}  {path}")


def show_list(limit):
    logs = json.loads(request("/logs"))["logs"][:limit]
    for e in logs:
        kb = (e.get("size", 0) + 1023) // 1024
        print(f"{e['code']}  {e['uploaded'][:19]}  {e.get('platform', '?'):8} "
              f"{e.get('version', '?'):16} {kb:>6} KB  {e.get('os', '')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("code", nargs="?")
    ap.add_argument("-o", "--out", type=Path, default=Path.home() / "xelee-logs")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("-n", type=int, default=20)
    args = ap.parse_args()
    if args.list:
        show_list(args.n)
    elif args.code:
        fetch(args.code.strip().upper(), args.out)
    else:
        ap.error("give a code or --list")


if __name__ == "__main__":
    main()
