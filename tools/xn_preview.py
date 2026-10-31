#!/usr/bin/env python3
"""Screenshots of XeleeNet's pages with sample data, for working on their
look (src/pc/xeleenet_menu.cpp, resources/xeleenet*.rcss).

    tools/xn_preview.py [PAGE ...] [--share] [--out build/xn-preview]

PAGE is host, more, stages, fighters, lobbies, code, joining, progress, failed, banner,
css-ready, css-countdown, css-waiting, css-leaving, css-left, css-N (a full
lobby of N seats, for the doors and the names in them), the stage
select's sss-N, sss-draw-N or sss-landed-N for N players, or LAN's lan-host,
lan-lobbies, lan-empty, lan-blocked, lan-address and lan-banner (all of them
by default). Each boots the game straight into the page
(MELEE_BOOT_SCENE=xn-host or css, MELEE_XN_PREVIEW=PAGE: no network, no
service), waits for it to fade in and captures the window. --share puts the
pictures on the tailnet share as xn-preview-PAGE.png.

For a stylesheet or the text alone there's no need to restart: edit the
.rcss or resources/lang/en.yaml (the copy beside the executable) with the
game on the page, and press F5 in it.
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PAGES = ["host", "more", "stages", "fighters", "lobbies", "code", "joining", "progress", "failed", "banner",
         "css-ready", "css-countdown", "css-waiting", "css-leaving", "css-left", "css-2", "css-3", "css-4",
         "sss-2", "sss-3", "sss-4", "sss-draw-2", "sss-draw-4", "sss-landed-2", "sss-landed-4",
         "lan-host", "lan-lobbies", "lan-empty", "lan-blocked", "lan-address", "lan-banner"]
DISC = os.path.expanduser("~/Downloads/Super Smash Bros. Melee (USA) (En,Ja) (Rev 2).iso")
SHARE = os.path.expanduser("~/.claude/skills/tailnet-share/scripts/share.py")


def shoot(page, args):
    out = os.path.join(args.out, f"{page}.png")
    log_path = os.path.join(args.out, f"{page}.log")
    ident = os.path.join(args.out, "id")
    os.makedirs(ident, exist_ok=True)
    title = f"xn-preview-{page}"
    sss = page.startswith("sss-")
    banner = page in ("banner", "lan-banner") or page.startswith("css-")
    env = dict(os.environ,
               MELEE_BOOT_SCENE="sss" if sss else "css" if banner else "xn-host",
               MELEE_XN_PREVIEW=page, MELEE_XN_NAME="RHYTHM", MELEE_XN_IDENTITY=ident,
               MELEE_WINDOW_TITLE=title, MELEE_VSYNC="0", MELEE_NO_CARD="1",
               SDL_VIDEO_DRIVER=os.environ.get("SDL_VIDEO_DRIVER", "x11"))
    with open(log_path, "wb") as log:
        proc = subprocess.Popen([args.exe, "--no-card", args.disc], env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            css = banner or sss
            want = b"MnSlMap" if sss else b"MnSlChr" if css else b"xeleenet pages: preview"
            deadline = time.time() + 90
            seen = False
            while time.time() < deadline and proc.poll() is None:
                time.sleep(0.5)
                with open(log_path, "rb") as f:
                    text = f.read()
                if want in text:
                    seen = True
                    break
            if not seen:
                print(f"xn_preview: {page}: never showed (see {log_path})")
                return None
            time.sleep(args.settle + (5 if css else 0))  # the CSS's doors, then the banner
            r = subprocess.run([sys.executable, os.path.join(HERE, "devctl.py"), "shot", out],
                               env=dict(env, MELEE_WINDOW_TITLE=title), capture_output=True, text=True)
            if r.returncode != 0:
                print(f"xn_preview: {page}: capture failed: {r.stderr.strip()}")
                return None
            print(f"xn_preview: {page} -> {out}")
            return out
        finally:
            proc.kill()
            proc.wait()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("pages", nargs="*", default=PAGES)
    ap.add_argument("--share", action="store_true")
    ap.add_argument("--exe", default=os.path.join(ROOT, "build", "linux", "melee"))
    ap.add_argument("--disc", default=DISC)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "xn-preview"))
    ap.add_argument("--settle", type=float, default=3.0, help="seconds after the page appears")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    bad = [p for p in args.pages if p not in PAGES]
    if bad:
        sys.exit(f"unknown page(s) {bad}; pages are {PAGES}")
    shots = [s for s in (shoot(p, args) for p in args.pages) if s]
    if args.share and shots:
        for s in shots:
            subprocess.run([sys.executable, "-I", SHARE, "add", s, "--as",
                            "xn-preview-" + os.path.basename(s)], check=False, capture_output=True)
        print("xn_preview: shared as xn-preview-*.png")
    sys.exit(0 if len(shots) == len(args.pages) else 1)


if __name__ == "__main__":
    main()
