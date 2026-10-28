#!/usr/bin/env python3
"""A real-network XeleeNet trial: three players on three networks, joining by
code (docs/xeleenet-plan.md, XN2), with the lobby service on the host's
desktop instead of Cloudflare.

    tools/xn_trial.py serve                       wrangler dev for every network
    tools/xn_trial.py host [--seats 3]            the host's game; prints the code
    tools/xn_trial.py join CODE --url URL         a desktop guest
    tools/xn_trial.py quest CODE --url URL        a Quest guest (adb), then Play
    tools/xn_trial.py quest-clean                 take the trial off the Quest
    tools/xn_trial.py bundle                      zip the desktop build for another machine

`serve` listens on every interface and prints its LAN and tailnet URLs. A
guest at home uses the LAN one; one elsewhere (a phone hotspot) the tailnet
one. Only the lobby calls go that way: the games punch their own links over
the internet (net_punch.c).

Every player runs the same MELEE_APP_REV (the service only seats players on the
same build) and MELEE_DEBUG_VS=human<seats>: once everyone is in, the host's
Start at the title goes straight into a match for all. Each desktop's log goes
to ~/xn-trial-<role>.log; the Quest's is logcat. The Quest reads its settings
from melee-env.txt at boot, so `quest` adds them there (between markers, the
rest of the file kept) and `quest-clean` takes them out again -- until then,
every launch of the game on the Quest tries to join.
"""
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PORT = 8790  # not 8787: the tailnet share listens there
REV = "xn2-trial"
QUEST_ENV = "/sdcard/Android/data/dev.melee.game/files/melee-env.txt"
BEGIN, END = "# xn-trial begin", "# xn-trial end"
DEFAULT_DISC = os.path.expanduser("~/Downloads/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso")


def lan_ip():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect(("8.8.8.8", 53))  # nothing is sent: it only picks the route
        return s.getsockname()[0]


def tailnet_ip():
    for cmd in (["tailscale", "ip", "-4"], ["distrobox-host-exec", "tailscale", "ip", "-4"]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=10).stdout.split()
            if out:
                return out[0]
        except (OSError, subprocess.TimeoutExpired):
            pass
    return None


def serve(args):
    state = tempfile.mkdtemp(prefix="xn-trial-")
    cmd = ["npx", "wrangler", "dev", "--ip", "0.0.0.0", "--port", str(PORT), "--persist-to", state,
           "--var", "TOKEN_KEY:xn-trial"]
    env = dict(os.environ, WRANGLER_SEND_METRICS="false")
    print(f"XeleeNet service on port {PORT}:")
    print(f"  this machine   http://127.0.0.1:{PORT}")
    print(f"  at home        http://{lan_ip()}:{PORT}")
    ts = tailnet_ip()
    print(f"  elsewhere      http://{ts}:{PORT}" if ts else "  elsewhere      (no tailnet address found)")
    print("Ctrl-C stops it.\n", flush=True)
    try:
        subprocess.run(cmd, cwd=os.path.join(HERE, "xeleenet"), env=env)
    except KeyboardInterrupt:
        pass
    finally:
        shutil.rmtree(state, ignore_errors=True)


def trial_env(xn, url, seats, name):
    return {
        "MELEE_XN": xn,
        "MELEE_XN_URL": url,
        "MELEE_XN_SEATS": str(seats),
        "MELEE_XN_NAME": name,
        "MELEE_XN_WAIT_S": "600",
        "MELEE_APP_REV": REV,
        "MELEE_DEBUG_VS": f"human{seats}",
    }


def play(args, xn, role):
    # In the bundle (`bundle`), the game sits beside this script.
    bundled = os.path.join(HERE, "run.sh")
    exe = args.exe or (bundled if os.path.exists(bundled) else os.path.join(ROOT, "build", "linux", "melee"))
    if not os.path.exists(args.disc):
        sys.exit(f"no disc at {args.disc}; pass --disc")
    env = dict(os.environ, **trial_env(xn, args.url, args.seats, args.name or role.upper()))
    log_path = os.path.expanduser(f"~/xn-trial-{role}.log")
    print(f"log: {log_path}", flush=True)
    with open(log_path, "wb") as log:
        proc = subprocess.Popen([exe, args.disc], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        # Echo the connect phase and the session's verdicts; everything goes to the log.
        keep = re.compile(rb"xeleenet:|net: (rollback with|link to|P\d|DESYNC|disconnected)|"
                          rb"peer silent|cannot hear")
        try:
            for line in proc.stdout:
                log.write(line)
                log.flush()
                if keep.search(line):
                    sys.stdout.write(line.decode("utf-8", "replace"))
                    sys.stdout.flush()
                m = re.search(rb"xeleenet: lobby code ([A-Z0-9]{6})", line)
                if m:
                    print(f"\n*** lobby code {m.group(1).decode()} -- give it to the others ***\n",
                          flush=True)
        except KeyboardInterrupt:
            proc.terminate()
        proc.wait()


def adb(*a, check=True, **kw):
    return subprocess.run(["adb", *a], check=check, capture_output=True, text=True, **kw)


def quest_env_lines():
    r = adb("shell", f"cat {QUEST_ENV} 2>/dev/null", check=False)
    text = r.stdout if r.returncode == 0 else ""
    out, skip = [], False
    for line in text.splitlines():
        if line.strip() == BEGIN:
            skip = True
        elif line.strip() == END:
            skip = False
        elif not skip:
            out.append(line)
    return out


def quest_write(lines):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write("\n".join(lines) + ("\n" if lines else ""))
        path = f.name
    try:
        adb("push", path, QUEST_ENV)
    finally:
        os.unlink(path)


def quest(args):
    kept = quest_env_lines()
    env = trial_env(f"join:{args.code}", args.url, args.seats, args.name or "QUEST")
    quest_write(kept + [BEGIN] + [f"{k}={v}" for k, v in env.items()] + [END])
    print(f"The Quest joins {args.code} at its next launch. Start Xelee and press Play.")
    print("Its connect phase, from logcat:\n", flush=True)
    adb("logcat", "-c", check=False)
    try:
        p = subprocess.Popen(["adb", "logcat", "-v", "brief", "Melee:I", "*:S"], stdout=subprocess.PIPE,
                             text=True)
        for line in p.stdout:
            if "xeleenet:" in line or "net: link to" in line or "net: rollback with" in line:
                print(line.rstrip(), flush=True)
    except KeyboardInterrupt:
        pass
    print("\nRun `tools/xn_trial.py quest-clean` when you're done, or every launch tries to join.")


def quest_clean(args):
    quest_write(quest_env_lines())
    print("The trial is off the Quest; the rest of its melee-env.txt is as it was.")


README = """Xelee XeleeNet trial build (feat/xelee-net {rev})

Needs Linux x86_64 with glibc 2.39 or newer, libcurl and OpenSSL 3 (any
current Fedora, Ubuntu 24.04+, Arch), and your own Melee disc image.

Join a lobby (the host gives you the code and the service URL):

    ./xn_trial.py join CODE --url http://HOST:{port} --disc /path/to/melee.iso

The log goes to ~/xn-trial-join.log; send it back after the run.
"""


def bundle(args):
    """The desktop build, its libraries and this script in one zip."""
    build = os.path.join(ROOT, "build", "linux")
    rev = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True,
                         text=True).stdout.strip()
    stage_root = os.path.join(ROOT, "build", "xn-trial-bundle")
    name = "xelee-xn-trial"
    stage = os.path.join(stage_root, name)
    shutil.rmtree(stage_root, ignore_errors=True)
    os.makedirs(os.path.join(stage, "lib"))
    shutil.copy2(os.path.join(build, "melee"), stage)
    subprocess.run(["strip", "--strip-debug", os.path.join(stage, "melee")], check=True)
    shutil.copytree(os.path.join(ROOT, "resources"), os.path.join(stage, "resources"))
    subprocess.run(f"gzip -dc '{ROOT}/tools/initial_pipeline_cache.db.gz' > '{stage}/initial_pipeline_cache.db'",
                   shell=True, check=True)
    shutil.copy2(os.path.join(build, "libphonon.so"), os.path.join(stage, "lib"))
    libusb = subprocess.run("ldconfig -p | grep -m1 -oE '/[^ ]*libusb-1\\.0\\.so\\.0$'", shell=True,
                            capture_output=True, text=True).stdout.strip()
    if libusb:
        shutil.copy(libusb, os.path.join(stage, "lib", "libusb-1.0.so.0"))
    with open(os.path.join(stage, "run.sh"), "w") as f:
        f.write('#!/bin/bash\nHERE="$(dirname "$(readlink -f "$0")")"\n'
                'export LD_LIBRARY_PATH="$HERE/lib:${LD_LIBRARY_PATH:-}"\n'
                'exec "$HERE/melee" "$@"\n')
    os.chmod(os.path.join(stage, "run.sh"), 0o755)
    shutil.copy2(os.path.abspath(__file__), os.path.join(stage, "xn_trial.py"))
    with open(os.path.join(stage, "README.txt"), "w") as f:
        f.write(README.format(rev=rev, port=PORT))
    out = os.path.join(ROOT, "dist", f"{name}-linux-x86_64.zip")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    if os.path.exists(out):
        os.unlink(out)
    subprocess.run(["zip", "-qr", out, name], cwd=stage_root, check=True)  # zip keeps the x bits
    print(f"{out} ({os.path.getsize(out) / 1e6:.0f} MB, {rev})")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("serve")
    for name in ("host", "join", "quest"):
        p = sub.add_parser(name)
        if name != "host":
            p.add_argument("code")
            p.add_argument("--url", required=True, help="the service, as `serve` printed it")
        else:
            p.add_argument("--url", default=f"http://127.0.0.1:{PORT}")
        p.add_argument("--seats", type=int, default=3, choices=(2, 3, 4))
        p.add_argument("--name", default="", help="up to 12 letters, digits, spaces")
        if name != "quest":
            p.add_argument("--exe", default="")
            p.add_argument("--disc", default=DEFAULT_DISC)
    sub.add_parser("quest-clean")
    sub.add_parser("bundle")
    args = ap.parse_args()
    if args.cmd == "serve":
        serve(args)
    elif args.cmd == "host":
        play(args, "host", "host")
    elif args.cmd == "join":
        play(args, f"join:{args.code}", "join")
    elif args.cmd == "quest":
        quest(args)
    elif args.cmd == "bundle":
        bundle(args)
    else:
        quest_clean(args)


if __name__ == "__main__":
    main()
