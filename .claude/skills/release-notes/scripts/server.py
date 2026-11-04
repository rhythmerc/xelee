#!/usr/bin/env python3
"""The release-notes editor: a small web page on the tailnet where the user
edits a draft changelog from their phone.

    python3 -I server.py --dir DIR [--port 8793] [--host ADDR]

DIR holds:
    notes.md          the draft (the page edits it; Save writes it)
    claude-notes.md   optional: questions and caveats shown above the editor
    history/          every earlier text, saved before each change

Endpoints: GET / (the page), GET /notes.md, GET /claude-notes.md,
POST /save (body = the new markdown), GET /history (JSON list),
GET /history/<file>.

It binds the machine's Tailscale IPv4 by default, so only the tailnet can
reach it. There is no other access control: never put secrets in DIR.
"""
import argparse
import json
import os
import subprocess
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "editor.html")
MAX = 256 * 1024
DIR = ""


def path(*parts):
    return os.path.join(DIR, *parts)


class H(BaseHTTPRequestHandler):
    def send(self, code, body, ctype="text/plain; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def file(self, p, ctype):
        if not os.path.isfile(p):
            return self.send(404, "not found")
        with open(p, "rb") as f:
            return self.send(200, f.read(), ctype)

    def do_GET(self):
        p = self.path.split("?")[0]
        md = "text/markdown; charset=utf-8"
        if p in ("/", "/index.html"):
            return self.file(PAGE, "text/html; charset=utf-8")
        if p == "/notes.md":
            return self.file(path("notes.md"), md)
        if p == "/claude-notes.md":
            return self.file(path("claude-notes.md"), md)
        if p == "/history":
            files = sorted((f for f in os.listdir(path("history")) if f.endswith(".md")), reverse=True)
            return self.send(200, json.dumps(files), "application/json")
        if p.startswith("/history/"):
            name = os.path.basename(p)
            if name.endswith(".md"):
                return self.file(path("history", name), md)
        return self.send(404, "not found")

    def do_POST(self):
        if self.path != "/save":
            return self.send(404, "not found")
        n = int(self.headers.get("Content-Length", "0"))
        if n <= 0 or n > MAX:
            return self.send(400, "bad length")
        text = self.rfile.read(n).decode("utf-8")
        old = ""
        if os.path.isfile(path("notes.md")):
            with open(path("notes.md"), encoding="utf-8") as f:
                old = f.read()
        if old != text:
            if old:
                stamp = time.strftime("%Y%m%d-%H%M%S")
                with open(path("history", f"notes-{stamp}.md"), "w", encoding="utf-8") as f:
                    f.write(old)
            tmp = path("notes.md.tmp")
            with open(tmp, "w", encoding="utf-8") as f:
                f.write(text)
            os.replace(tmp, path("notes.md"))
        return self.send(200, json.dumps({"saved": time.strftime("%H:%M:%S")}), "application/json")

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % args))


def tailscale_ip():
    for cmd in (["tailscale", "ip", "-4"], ["distrobox-host-exec", "tailscale", "ip", "-4"]):
        try:
            out = subprocess.run(cmd, capture_output=True, text=True, timeout=10).stdout.split()
            if out:
                return out[0]
        except (OSError, subprocess.TimeoutExpired):
            pass
    sys.exit("no Tailscale address; pass --host")


def main():
    global DIR
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--port", type=int, default=8793)
    ap.add_argument("--host")
    a = ap.parse_args()
    DIR = os.path.abspath(a.dir)
    os.makedirs(path("history"), exist_ok=True)
    if not os.path.isfile(path("notes.md")):
        sys.exit(f"{path('notes.md')} is missing: write the draft first")
    host = a.host or tailscale_ip()
    print(f"release notes editor on http://{host}:{a.port}/ for {DIR}", flush=True)
    ThreadingHTTPServer((host, a.port), H).serve_forever()


if __name__ == "__main__":
    main()
