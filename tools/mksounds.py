#!/usr/bin/env python3
"""
Synthesize the sounds in rootfs/usr/share/sounds: a "ding" as a WAV file
and a chime as a Creative Voice (.VOC) file. The output is checked in, so
building fluxOS does not need this. startup.wav, the desktop's startup
sound, is not made here: it is a recording that is checked in as it is.

usage: tools/mksounds.py   (run from the top of the source tree)
"""
import math
import os
import struct

OUT = "rootfs/usr/share/sounds"
RATE = 22050


def note(n):
    """Frequency of MIDI note n (69 is A4, 440 Hz)."""
    return 440.0 * 2 ** ((n - 69) / 12.0)


def bell(t, f, decay):
    """A soft bell: a fundamental and two inharmonic partials, struck at t=0."""
    if t < 0:
        return 0.0
    attack = min(1.0, t / 0.01)
    env = attack * math.exp(-t / decay)
    return env * (math.sin(2 * math.pi * f * t) +
                  0.35 * math.sin(2 * math.pi * f * 2.76 * t) * math.exp(-t / (decay * 0.4)) +
                  0.15 * math.sin(2 * math.pi * f * 5.4 * t) * math.exp(-t / (decay * 0.2)))


def render(length, voices, gain):
    """voices: (start time, frequency, decay). Returns floats in -1..1."""
    out = []
    for i in range(int(length * RATE)):
        t = i / RATE
        s = sum(bell(t - start, f, d) for start, f, d in voices)
        out.append(max(-1.0, min(1.0, s * gain)))
    # fade the last 50 ms so the sound ends at rest
    fade = int(0.05 * RATE)
    for i in range(fade):
        out[-1 - i] *= i / fade
    return out


def write_wav(path, samples):
    data = b"".join(struct.pack("<h", int(s * 32000)) for s in samples)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def write_voc(path, samples, rate):
    """8-bit unsigned mono in one sound data block (type 1)."""
    data = bytes(max(0, min(255, int(128 + s * 120))) for s in samples)
    tc = 256 - 1000000 // rate
    with open(path, "wb") as f:
        f.write(b"Creative Voice File\x1a" + struct.pack("<HHH", 26, 0x010A, (0x1234 + ~0x010A) & 0xFFFF))
        block = struct.pack("<BB", tc, 0) + data
        f.write(struct.pack("<B", 1) + struct.pack("<I", len(block))[:3] + block)
        f.write(b"\x00")


def main():
    os.makedirs(OUT, exist_ok=True)
    # Ding: one struck note.
    write_wav(OUT + "/ding.wav", render(0.7, [(0.0, note(84), 0.25)], 0.55))
    # Chime: three falling notes, as an 11025 Hz VOC file.
    global RATE
    RATE = 11025
    chime = render(1.4, [(0.0, note(88), 0.4), (0.25, note(84), 0.4), (0.5, note(79), 0.6)], 0.4)
    write_voc(OUT + "/chime.voc", chime, 11025)
    with open(OUT + "/README", "w") as f:
        f.write("Sounds synthesized by tools/mksounds.py. Play them with play(1).\n")


if __name__ == "__main__":
    main()
