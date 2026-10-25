#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Makes the XR room light calibration's sounds (src/pc/room_chime.c) into
resources/xr/, mono 16-bit at AX's 32 kHz:

  calibrate-chime-1.wav .. -4.wav   a light's chime: a small bell on two
                                    notes a fifth and 70 ms apart, E6, F#6,
                                    G#6 then B6 (a pentatonic step up a
                                    light; the fifth light on keeps the
                                    fourth's), played at the light
  calibrate-done.wav                the Done card's: the same bell lower,
                                    B5 up to E6 over a soft G#5, centred

The user's pick (chime B of the first proposal) as it was made at 48 kHz;
here at 32 kHz, without the partials past 15 kHz (the top one of the
highest notes), at the levels they had in the approved demo track. No
reverb. Stdlib only. Run from anywhere:
python3 tools/xr_cards/make_calibration_sounds.py
"""
import math
import os
import random
import struct
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, "..", "..", "resources", "xr"))
SR = 32000
NYQUIST_KEEP = 15000.0


def bell(f0, dur, amp=1.0, decay=0.22):
    parts = [(1.0, 1.0, 1.0), (2.0, 0.42, 0.55), (3.01, 0.22, 0.38), (4.17, 0.12, 0.28), (5.43, 0.06, 0.2)]
    parts = [p for p in parts if f0 * p[0] < NYQUIST_KEEP]
    n = int(dur * SR)
    out = [0.0] * n
    for i in range(n):
        t = i / SR
        a = min(1.0, t / 0.003)
        s = 0.0
        for ratio, g, d in parts:
            s += g * math.exp(-t / (decay * d)) * math.sin(2 * math.pi * f0 * ratio * t)
        s += 0.25 * math.exp(-t / decay) * math.sin(2 * math.pi * f0 * 1.003 * t)
        out[i] = amp * a * s
    return out


def sparkle(rng, dur=0.03, amp=0.12):
    n = int(dur * SR)
    out, prev, y = [], 0.0, 0.0
    for i in range(n):
        x = rng.uniform(-1, 1)
        y = 0.7 * (y + x - prev)
        prev = x
        out.append(amp * y * math.exp(-i / (0.006 * SR)))
    return out


def mix(dst, src, at):
    o = int(at * SR)
    need = o + len(src) - len(dst)
    if need > 0:
        dst.extend([0.0] * need)
    for i, v in enumerate(src):
        dst[o + i] += v


def chime(rng, f):
    s = []
    mix(s, bell(f, 0.45, 0.7, decay=0.12), 0)
    mix(s, sparkle(rng), 0)
    mix(s, bell(f * 1.4983, 0.8, 1.0), 0.07)
    mix(s, sparkle(rng, amp=0.08), 0.07)
    return s


def done(rng):
    s = []
    mix(s, bell(987.77, 0.6, 0.55, decay=0.16), 0)
    mix(s, sparkle(rng, amp=0.06), 0)
    mix(s, bell(1318.51, 1.1, 0.75, decay=0.30), 0.11)
    mix(s, bell(830.61, 1.1, 0.30, decay=0.30), 0.11)
    mix(s, sparkle(rng, amp=0.05), 0.11)
    return s


def write(name, samples, gain):
    path = os.path.join(OUT, name)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1, min(1, v * gain)) * 32767)) for v in samples))
    print(f"{path}: {len(samples) / SR:.2f} s")


if __name__ == "__main__":
    rng = random.Random(7)
    E6 = 1318.51
    steps = [1.0, 2 ** (2 / 12), 2 ** (4 / 12), 2 ** (7 / 12), 2 ** (7 / 12)]  # E F# G# B, then B held
    chimes = [chime(rng, E6 * r) for r in steps]
    end = done(rng)
    # The demo track's level: its peak at 0.7, with the chimes 0.55 s apart.
    track = []
    for k, c in enumerate(chimes):
        mix(track, c, 0.10 + 0.55 * k)
    mix(track, end, 3.45)
    gain = 0.7 / max(abs(v) for v in track)
    os.makedirs(OUT, exist_ok=True)
    for k in range(4):
        write(f"calibrate-chime-{k + 1}.wav", chimes[k], gain)
    write("calibrate-done.wav", end, gain)
