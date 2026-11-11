#!/usr/bin/env python3
"""Adds Xelee to Steam's library as a non-Steam game, on the Steam Frame (or
any SteamOS): an entry in each Steam user's config/shortcuts.vdf, the binary
file Steam's "Add a Non-Steam Game" writes. An entry for the same executable
is replaced; other entries are kept. The old file is kept beside it as
shortcuts.vdf.bak.

    python3 add-to-steam.py [~/Applications/xelee]   (tools/frame_shortcut.py in the source)

Steam reads the file when it starts and may write its own copy back when it
quits, so stop it first (on SteamOS: systemctl --user stop steam), then start
it again. Prints the shortcut's id for
steam://rungameid/<id> (FRAME_APPID in frame_deploy.sh).
"""
import binascii
import os
import struct
import sys
from pathlib import Path

NAME = "Xelee"


# Binary VDF: 0x00 name (nested map) ... 0x08; 0x01 name string; 0x02 name
# int32; 0x08 ends a map.
def read_map(data, i):
    out = {}
    while True:
        kind = data[i]
        i += 1
        if kind == 0x08:
            return out, i
        end = data.index(b"\0", i)
        key = data[i:end].decode("utf-8", "replace")
        i = end + 1
        if kind == 0x00:
            out[key], i = read_map(data, i)
        elif kind == 0x01:
            end = data.index(b"\0", i)
            out[key] = data[i:end].decode("utf-8", "replace")
            i = end + 1
        elif kind == 0x02:
            out[key] = struct.unpack_from("<i", data, i)[0]
            i += 4
        else:
            raise ValueError("unknown VDF field type %#x at %d" % (kind, i - 1))


def write_map(m):
    out = bytearray()
    for key, value in m.items():
        k = key.encode() + b"\0"
        if isinstance(value, dict):
            out += b"\x00" + k + write_map(value)
        elif isinstance(value, int):
            out += b"\x02" + k + struct.pack("<i", value)
        else:
            out += b"\x01" + k + str(value).encode() + b"\0"
    return bytes(out + b"\x08")


def shortcut_ids(exe, name):
    """Steam's id for a shortcut (what Steam ROM Manager computes): the
    entry's appid (signed 32 bits) and the 64-bit one steam://rungameid/
    takes."""
    crc = binascii.crc32((exe + name).encode()) | 0x80000000
    return struct.unpack("<i", struct.pack("<I", crc))[0], (crc << 32) | 0x02000000


def main():
    home = Path(sys.argv[1] if len(sys.argv) > 1 else "~/Applications/xelee").expanduser().resolve()
    script = home / "xelee.sh"
    if not script.exists():
        sys.exit("no %s: deploy Xelee first" % script)
    exe = '"%s"' % script
    appid, game_id = shortcut_ids(exe, NAME)
    users = sorted(Path("~/.local/share/Steam/userdata").expanduser().glob("*/config"))
    if not users:
        sys.exit("no Steam user found under ~/.local/share/Steam/userdata")
    for config in users:
        path = config / "shortcuts.vdf"
        shortcuts = {}
        if path.exists():
            data = path.read_bytes()
            if data[:1] != b"\x00":
                sys.exit("%s: not a binary VDF file" % path)
            root, _ = read_map(data, 1 + len(b"shortcuts\0"))
            shortcuts = {k: v for k, v in root.items()
                         if not (isinstance(v, dict) and v.get("Exe") == exe)}
            path.with_name("shortcuts.vdf.bak").write_bytes(data)
        entries = list(shortcuts.values())
        entries.append({
            "appid": appid,
            "AppName": NAME,
            "Exe": exe,
            "StartDir": '"%s/"' % home,
            "icon": str(home / "xelee.png"),
            "ShortcutPath": "",
            "LaunchOptions": "",
            "IsHidden": 0,
            "AllowDesktopConfig": 1,
            "AllowOverlay": 1,
            # A VR app: the headset shows it, not a flat window.
            "OpenVR": 1,
            "Devkit": 0,
            "DevkitGameID": "",
            "DevkitOverrideAppID": 0,
            "LastPlayTime": 0,
            "FlatpakAppID": "",
            "sortas": "",
            "tags": {},
        })
        tree = {str(i): e for i, e in enumerate(entries)}
        tmp = path.with_suffix(".vdf.tmp")
        tmp.write_bytes(b"\x00shortcuts\0" + write_map(tree) + b"\x08")
        os.replace(tmp, path)
        print("%s: %d shortcut(s), %s added" % (path, len(entries), NAME))
    print("steam://rungameid/%d" % game_id)


if __name__ == "__main__":
    main()
