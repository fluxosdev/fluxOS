#!/usr/bin/env python3
"""
Pack the color emoji the browser draws into rootfs/usr/share/fonts/noto/emoji.fxe,
from Noto Color Emoji (SIL Open Font License 1.1). The output is checked in, so
building fluxOS does not need this script, PIL or the font.

  tools/mkemoji.py UCD_DIR [NotoColorEmoji.ttf]

UCD_DIR holds emoji-data.txt (https://www.unicode.org/Public/16.0.0/ucd/emoji/).
Every single code point that is an emoji (U+00A9 and up) is drawn by PIL from the
font's bitmaps, cut to its ink, centered in a square and scaled to SIZE pixels.
Sequences (skin tones, ZWJ families, flags) are not in it: their parts show one by one.

File layout (little-endian):
  "FXEM", u32 count, u16 SIZE, u16 0
  u32 code points, ascending; bit 31 set if it shows as an emoji by default
      (Emoji_Presentation), else only when U+FE0F follows it
  u32 offsets of each glyph's data, and one past the last (from the start of the file)
  per glyph: SIZE x SIZE pixels of premultiplied R, G, B, A, deflated with a zlib header
"""
import os
import struct
import sys
import zlib

from PIL import Image, ImageDraw, ImageFont

SIZE = 40
FONT = "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf"
OUT = "rootfs/usr/share/fonts/noto/emoji.fxe"


def emoji_sets(path):
    emoji, pres = set(), set()
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cps, prop = (x.strip() for x in line.split(";"))
        lo, hi = (int(x, 16) for x in cps.split("..")) if ".." in cps else (int(cps, 16),) * 2
        for c in range(lo, hi + 1):
            if prop == "Emoji":
                emoji.add(c)
            elif prop == "Emoji_Presentation":
                pres.add(c)
    return emoji, pres


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    emoji, pres = emoji_sets(os.path.join(sys.argv[1], "emoji-data.txt"))
    font = ImageFont.truetype(sys.argv[2] if len(sys.argv) == 3 else FONT, 109)   # its one bitmap size
    cps, blobs = [], []
    for c in sorted(x for x in emoji if x >= 0xA9):
        im = Image.new("RGBA", (160, 160), (0, 0, 0, 0))
        ImageDraw.Draw(im).text((10, 10), chr(c), font=font, embedded_color=True)
        box = im.getbbox()
        if not box:
            continue
        im = im.crop(box)
        w, h = im.size
        side = max(w, h)
        square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
        square.paste(im, ((side - w) // 2, (side - h) // 2))
        square = square.convert("RGBa").resize((SIZE, SIZE), Image.LANCZOS)
        cps.append(c | (1 << 31 if c in pres else 0))
        blobs.append(zlib.compress(square.tobytes(), 9))
    n = len(cps)
    header = struct.pack("<4sIHH", b"FXEM", n, SIZE, 0)
    at = len(header) + 4 * n + 4 * (n + 1)
    offsets = []
    for b in blobs:
        offsets.append(at)
        at += len(b)
    offsets.append(at)
    with open(OUT, "wb") as f:
        f.write(header)
        f.write(struct.pack("<%dI" % n, *cps))
        f.write(struct.pack("<%dI" % (n + 1), *offsets))
        for b in blobs:
            f.write(b)
    print("%s: %d emoji, %d bytes" % (OUT, n, at))


if __name__ == "__main__":
    main()
