#!/usr/bin/env python3
"""A real-network XeleeNet trial: three players on three networks, joining by
code (docs/xeleenet-plan.md, XN2), with the lobby service on the host's
desktop instead of Cloudflare.

    tools/xn_trial.py serve                       the lobby service (and STUN) for every network
    tools/xn_trial.py menu --url URL              a desktop game, driven by hand in the menus
    tools/xn_trial.py quest-menu --url URL        the Quest, the same (adb), then Play
    tools/xn_trial.py host [--seats 3]            the host's game; prints the code
    tools/xn_trial.py join CODE --url URL         a desktop guest
    tools/xn_trial.py quest CODE --url URL        a Quest guest (adb), then Play
    tools/xn_trial.py quest-clean                 take the trial off the Quest
    tools/xn_trial.py bundle                      zip the desktop build for another machine

`serve` listens on every interface and prints its LAN, tailnet and (when
`tailscale funnel --bg 8790` is on) public URLs. A guest at home uses the LAN
one; one elsewhere (a phone hotspot) the tailnet or Funnel one. `menu` and
`quest-menu` play through Online Mode > XeleeNet (Host, Join) by hand: every
device only needs the URL and the same build tag. Only the lobby calls go that way: the games punch their own links over
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
REV = "xn-trial"
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


def funnel_url():
    """The Funnel's public URL when one serves PORT, else None."""
    try:
        out = subprocess.run(["tailscale", "funnel", "status"], capture_output=True, text=True,
                             timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return None
    m = re.search(r"(https://\S+?)/? \(Funnel on\)[\s\S]*?proxy http://127\.0\.0\.1:%d" % PORT, out)
    return m.group(1) if m else None


def serve(args):
    state = tempfile.mkdtemp(prefix="xn-trial-")
    sys.path.insert(0, os.path.join(HERE, "xeleenet"))
    import test_service  # noqa: E402
    cmd = [test_service.build(state), "-listen", f"0.0.0.0:{PORT}", "-stun", f":{PORT}"]
    env = dict(os.environ, XELEENET_TOKEN_KEY="xn-trial")
    print(f"XeleeNet service on port {PORT}:")
    print(f"  this machine   http://127.0.0.1:{PORT}")
    print(f"  at home        http://{lan_ip()}:{PORT}")
    ts = tailnet_ip()
    print(f"  elsewhere      http://{ts}:{PORT}" if ts else "  elsewhere      (no tailnet address found)")
    fu = funnel_url()
    print(f"  public         {fu}" if fu else f"  public         (no Funnel: tailscale funnel --bg {PORT})")
    print("Ctrl-C stops it.\n", flush=True)
    try:
        subprocess.run(cmd, env=env)
    except KeyboardInterrupt:
        pass
    finally:
        shutil.rmtree(state, ignore_errors=True)


def trial_env(xn, url, seats, name, wait_min=10):
    return {
        "MELEE_XN": xn,
        "MELEE_XN_URL": url,
        "MELEE_XN_SEATS": str(seats),
        "MELEE_XN_NAME": name,
        "MELEE_XN_WAIT_S": str(int(wait_min * 60)),
        "MELEE_APP_REV": REV,
        # Every device boots without a memory card: a card that prompts (a
        # fresh machine's first boot) holds that one in the boot scene, and
        # the others give up on it.
        "MELEE_NO_CARD": "1",
        "MELEE_DEBUG_VS": f"human{seats}",
    }


def menu_env(url):
    # No MELEE_XN: the menus host and join (Online Mode > XeleeNet).
    return {"MELEE_XN_URL": url, "MELEE_APP_REV": REV, "MELEE_NO_CARD": "1"}


def play(args, xn, role, env_extra=None):
    # In the bundle (`bundle`), the game sits beside this script, started by
    # its run.sh (not tools/run.sh, the gdb wrapper).
    in_bundle = os.path.exists(os.path.join(HERE, "melee"))
    exe = args.exe or (os.path.join(HERE, "run.sh") if in_bundle else os.path.join(ROOT, "build", "linux", "melee"))
    if not os.path.exists(args.disc):
        sys.exit(f"no disc at {args.disc}; pass --disc")
    env = dict(os.environ, **(env_extra if env_extra is not None else
                              trial_env(xn, args.url, args.seats, args.name or role.upper(), args.wait)))
    log_path = os.path.expanduser(f"~/xn-trial-{role}.log")
    print(f"log: {log_path}", flush=True)
    with open(log_path, "wb") as log:
        proc = subprocess.Popen([exe, "--no-card", args.disc], env=env, stdout=subprocess.PIPE,
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


def quest(args, env=None):
    kept = quest_env_lines()
    if env is None:
        env = trial_env(f"join:{args.code}", args.url, args.seats, args.name or "QUEST", args.wait)
        print(f"The Quest joins {args.code} at its next launch. Start Xelee and press Play.")
    else:
        print("The Quest reaches the service at its next launch. Start Xelee and press Play.")
    quest_write(kept + [BEGIN] + [f"{k}={v}" for k, v in env.items()] + [END])
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

Play (the host gives you the service URL), then Online Mode > XeleeNet:

    ./xn_trial.py menu --url URL --disc /path/to/melee.iso

The log goes to ~/xn-trial-menu.log; send it back after the run.
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
        p.add_argument("--wait", type=float, default=10, help="minutes to wait for the others")
        if name != "quest":
            p.add_argument("--exe", default="")
            p.add_argument("--disc", default=DEFAULT_DISC)
    for name in ("menu", "quest-menu"):
        p = sub.add_parser(name)
        p.add_argument("--url", required=True, help="the service, as `serve` printed it")
        if name == "menu":
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
    elif args.cmd == "menu":
        play(args, None, "menu", menu_env(args.url))
    elif args.cmd == "quest-menu":
        quest(args, menu_env(args.url))
    elif args.cmd == "bundle":
        bundle(args)
    else:
        quest_clean(args)


if __name__ == "__main__":
    main()
