#!/usr/bin/env python3
"""
Generate fluxOS's bitmap fonts as C source. The output is checked in, so
building fluxOS does not need this script, Python or PIL.

  lib/font8x16.c           8x16 VGA terminal font, characters 32-255 of code
                           page 437 (ASCII, then the PC's accented letters,
                           box drawing and symbols, for DOS programs), from
                           the Linux console fonts Lat15-VGA16 (ASCII) and
                           Uni2-VGA16 (the rest), both in the kbd package
  user/desktop/font_sans.c the classic theme's proportional UI fonts (regular
                           and bold, and a larger bold one for the Start menu
                           banner), ASCII 32-126, rasterised anti-aliased (4
                           bits of coverage per pixel) from Liberation Sans
                           (SIL Open Font License 1.1)
  user/desktop/font_archivo.c
                           the Kinetik theme's UI fonts, the same way, from
                           Archivo (https://fonts.google.com/specimen/Archivo,
                           SIL Open Font License 1.1): Medium and Bold at 12
                           pixels for text (as wide as Liberation Sans, so
                           dialogs still fit), Medium 14 for captions, Regular
                           24 for the Start menu's heading and Bold 18
                           for the Start button, set on the weight axis of the
                           variable font Google Fonts ships (Archivo[wdth,wght].ttf),
                           which is looked for in $ARCHIVO_DIR (default
                           /usr/share/fonts/truetype/archivo)
  rootfs/usr/share/fonts/web.fnt
                           the web browser's fonts: Liberation Sans, Serif
                           and Mono in regular, bold, italic and bold italic
                           (Mono: regular and bold) at 12 pixel sizes, with
                           Latin-1 and common typographic characters,
                           anti-aliased (4 bits of coverage per pixel)

usage: tools/mkfont.py   (run from the top of the source tree)

web.fnt layout (little-endian):
  "FXWA", u16 font count, u16 character count, u32 code points (ascending)
  per font: u8 family (0 sans, 1 serif, 2 mono), u8 style (1 bold, 2 italic),
            u8 pixel size, u8 line height, u8 ascent, 3 bytes padding,
            u32 offset of its glyph table
  glyph table, one entry per code point: u16 advance in 1/16 pixels, i8 x
            offset of the ink, u8 ink width, u8 first ink row, u8 ink rows,
            2 bytes padding, u32 offset of the bitmap (rows of ceil(width / 2)
            bytes: one 4-bit coverage value per pixel, 0-15, high nibble first)
"""
import gzip
import os
import struct

from PIL import Image, ImageDraw, ImageFont

PSF = "/usr/share/consolefonts/Lat15-VGA16.psf.gz"
PSF_CP437 = "/usr/share/consolefonts/Uni2-VGA16.psf.gz"
SANS = "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
SANS_BOLD = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"
SANS_PX = 12
BANNER_PX = 16
ARCHIVO_DIR = os.environ.get("ARCHIVO_DIR", "/usr/share/fonts/truetype/archivo")
ARCHIVO = "Archivo[wdth,wght].ttf"
ARCHIVO_FONTS = (("font_archivo", ARCHIVO, 12, 500, "Medium"), ("font_archivo_bold", ARCHIVO, 12, 700, "Bold"),
                 ("font_archivo_title", ARCHIVO, 14, 500, "Medium"),
                 ("font_archivo_large", ARCHIVO, 24, 400, "Regular"),
                 ("font_archivo_start", ARCHIVO, 18, 700, "Bold"))
CANVAS_W, ORIGIN = 64, 8                # glyphs are drawn this far in, so italics can overhang
FIRST, LAST = 32, 126
LAST_CP437 = 255


def psf1_load(path):
    """A PSF1 font's glyphs and its unicode table (code point -> glyph)."""
    data = gzip.open(path).read()
    magic, mode, height = struct.unpack("<HBB", data[:4])
    assert magic == 0x0436, "not a PSF1 font"
    count = 512 if mode & 1 else 256
    glyphs = [data[4 + i * height:4 + (i + 1) * height] for i in range(count)]
    index = {}
    if mode & 2:                        # unicode table: u16 lists, 0xFFFF ends
        pos, g = 4 + count * height, 0
        while g < count and pos + 1 < len(data):
            (u,) = struct.unpack("<H", data[pos:pos + 2])
            pos += 2
            if u == 0xFFFF:
                g += 1
            elif u != 0xFFFE:
                index.setdefault(u, g)
    assert height == 16
    return glyphs, index


# Code page 437 characters that Uni2-VGA16 lacks, drawn as in the IBM VGA ROM.
CP437_EXTRA = {
    0x2302: "00 00 00 00 10 38 6c c6 c6 c6 fe 00 00 00 00 00",     # house
    0x2593: "dd 77 dd 77 dd 77 dd 77 dd 77 dd 77 dd 77 dd 77",     # dark shade
    0x2584: "00 00 00 00 00 00 00 00 ff ff ff ff ff ff ff ff",     # lower half
    0x258C: "f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0 f0",     # left half
    0x2590: "0f 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f 0f",     # right half
    0x2580: "ff ff ff ff ff ff ff ff 00 00 00 00 00 00 00 00",     # upper half
}


def cp437_unicode(c):
    return 0x2302 if c == 0x7F else ord(bytes([c]).decode("cp437"))


def font8x16_glyphs():
    ascii_glyphs, ascii_index = psf1_load(PSF)
    pc_glyphs, pc_index = psf1_load(PSF_CP437)
    out = []
    for c in range(FIRST, LAST_CP437 + 1):
        if c <= LAST:
            out.append(ascii_glyphs[ascii_index.get(c, c)])
            continue
        u = cp437_unicode(c)
        if u in pc_index:
            out.append(pc_glyphs[pc_index[u]])
        else:
            out.append(bytes.fromhex(CP437_EXTRA[u]))
    return out


def write_font8x16(path):
    rows = font8x16_glyphs()
    with open(path, "w") as f:
        f.write("/*\n * 8x16 VGA font, code page 437 characters 32-255 (generated by\n"
                " * tools/mkfont.py from the Linux console fonts Lat15-VGA16 and\n"
                " * Uni2-VGA16). One byte per row, MSB is leftmost.\n */\n")
        f.write("#include <flux/font.h>\n\n")
        f.write("const uint8_t font8x16[FONT_GLYPHS][16] = {\n")
        for c, g in zip(range(FIRST, LAST_CP437 + 1), rows):
            name = repr(chr(c)) if c <= LAST else "0x%02x U+%04X" % (c, cp437_unicode(c))
            f.write("    { %s },  /* %s */\n" % (", ".join("0x%02x" % b for b in g), name))
        f.write("};\n")


def rasterise(ttf, px, weight=None):
    font = ImageFont.truetype(ttf, px)
    if weight:                          # a variable font: its weight, at normal width
        font.set_variation_by_axes([weight, 100])
    ascent, descent = font.getmetrics()
    height = ascent + descent
    glyphs = []
    for c in range(FIRST, LAST + 1):
        ch = chr(c)
        advance = int(round(font.getlength(ch)))
        img = Image.new("L", (CANVAS_W, height), 0)
        d = ImageDraw.Draw(img)
        d.text((ORIGIN, ascent), ch, font=font, fill=255, anchor="ls")
        pix = img.load()
        cols = [x for x in range(CANVAS_W) for y in range(height) if pix[x, y]]
        left = min(cols) if cols else ORIGIN
        right = max(cols) + 1 if cols else ORIGIN
        # Keep the ink, starting at the origin or where the ink starts if it
        # overhangs to the left.
        x0 = min(left, ORIGIN)
        width = max(right - x0, 0)
        assert 0 < left and right < CANVAS_W or not cols, "glyph %r does not fit" % ch
        cov = [(pix[x0 + x, y] + 8) // 17 for y in range(height) for x in range(width)]
        glyphs.append((ch, advance, x0 - ORIGIN, width, cov))
    return ascent, height, glyphs


def write_pfonts(f, fonts):
    """Each (C name, TTF path, pixel size[, weight]) as a struct pfont."""
    f.write('#include "font.h"\n')
    for name, ttf, px, *weight in fonts:
        ascent, height, glyphs = rasterise(ttf, px, *weight)
        f.write("\nstatic const uint8_t %s_cov[] = {\n" % name)
        offsets, pos = [], 0
        for ch, _, _, _, cov in glyphs:
            offsets.append(pos)
            pos += len(cov)
            if cov:
                f.write("    %s,  /* %r */\n" % (", ".join(str(v) for v in cov), ch))
            else:
                f.write("    /* %r: no ink */\n" % ch)
        f.write("    0\n};\n\n")
        f.write("const struct pfont %s = {\n    .height = %d,\n    .ascent = %d,\n"
                "    .glyphs = {\n" % (name, height, ascent))
        for (ch, adv, xoff, width, _), off in zip(glyphs, offsets):
            f.write("        { %d, %d, %d, %s_cov + %d },  /* %r */\n" % (adv, xoff, width, name, off, ch))
        f.write("    },\n};\n")


def write_sans(path):
    with open(path, "w") as f:
        f.write("/*\n * The classic theme's proportional UI fonts, ASCII 32-126 (generated by\n"
                " * tools/mkfont.py). Rasterised anti-aliased at %dpx (the banner font at %dpx)\n"
                " * from Liberation Sans, which is licensed under the SIL Open Font License\n"
                " * 1.1. Each pixel is 4 bits of coverage (0-15), one per byte, row by row.\n"
                " */\n" % (SANS_PX, BANNER_PX))
        write_pfonts(f, (("font_classic", SANS, SANS_PX), ("font_classic_bold", SANS_BOLD, SANS_PX),
                         ("font_banner", SANS_BOLD, BANNER_PX)))


def write_archivo(path):
    with open(path, "w") as f:
        f.write("/*\n * The Kinetik theme's UI fonts, ASCII 32-126 (generated by tools/mkfont.py),\n"
                " * rasterised anti-aliased from Archivo, Copyright 2020 The Archivo Project\n"
                " * Authors (https://github.com/Omnibus-Type/Archivo), licensed under the SIL\n"
                " * Open Font License 1.1. Each pixel is 4 bits of coverage (0-15), one per\n"
                " * byte, row by row.\n *\n")
        for n, _, px, _, style in ARCHIVO_FONTS:
            f.write(" *   %-20s Archivo %s, %d pixels\n" % (n, style, px))
        f.write(" */\n")
        write_pfonts(f, tuple((n, os.path.join(ARCHIVO_DIR, t), px, w) for n, t, px, w, _ in ARCHIVO_FONTS))


LIB = "/usr/share/fonts/truetype/liberation/Liberation%s-%s.ttf"
WEB_FACES = [(0, "Sans"), (1, "Serif"), (2, "Mono")]
WEB_STYLES = [(0, "Regular"), (1, "Bold"), (2, "Italic"), (3, "BoldItalic")]
WEB_SIZES = (10, 11, 12, 13, 14, 16, 18, 20, 24, 28, 32, 40)
# Latin Extended A/B, Greek and Cyrillic cover common European scripts.
WEB_CHARS = sorted(set(range(32, 127)) | set(range(160, 0x250)) |
                   set(range(0x370, 0x530)) | set(range(0x1E00, 0x1F00)) | {
    0x0152, 0x0153, 0x0160, 0x0161, 0x0178, 0x017D, 0x017E, 0x0192, 0x02C6, 0x02DC,
    0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, 0x2020, 0x2021,
    0x2022, 0x2026, 0x2030, 0x2032, 0x2039, 0x203A, 0x20AC, 0x2122, 0x2190, 0x2191,
    0x2192, 0x2193, 0x2212, 0x2264, 0x2265, 0x25A0, 0x25A1, 0x25B2, 0x25BC, 0x25CB,
    0x25CF, 0x2665})


def web_glyph(font, notdef, fallback, cp, ascent, height):
    """Rasterise one character, anti-aliased:
    (advance in 1/16 px, xoff, width, top, rows, bitmap bytes)."""
    ch = chr(cp)
    if cp != 32 and cp != 160 and font.getmask(ch).size == notdef.size and \
            bytes(font.getmask(ch)) == bytes(notdef):
        font = fallback                 # not in this face: borrow Liberation Sans's
    advance = int(round(font.getlength(ch) * 16))
    pad = height
    w = advance // 16 + 2 * pad + 1
    img = Image.new("L", (w, height), 0)
    d = ImageDraw.Draw(img)
    d.text((pad, ascent), ch, font=font, fill=255, anchor="ls")
    px = img.load()
    cols = [x for x in range(w) for y in range(height) if px[x, y]]
    rows = [y for y in range(height) for x in range(w) if px[x, y]]
    if not cols:
        return advance, 0, 0, 0, 0, b""
    left, right = min(cols), max(cols) + 1
    top, bottom = min(rows), max(rows) + 1
    width = right - left
    stride = (width + 1) // 2
    out = bytearray()
    for y in range(top, bottom):
        row = bytearray(stride)
        for x in range(width):
            v = (px[left + x, y] + 8) // 17         # 0-255 -> 0-15
            row[x // 2] |= v << 4 if x % 2 == 0 else v
        out += row
    return advance, left - pad, width, top, bottom - top, bytes(out)


def write_webfonts(path):
    fonts = []
    for fam, fname in WEB_FACES:
        for style, sname in WEB_STYLES:
            if fam == 2 and style & 2:
                continue                # no italic monospace: the browser uses upright
            for px in WEB_SIZES:
                fonts.append((fam, style, px, LIB % (fname, sname), LIB % ("Sans", sname)))
    header = struct.pack("<4sHH", b"FXWA", len(fonts), len(WEB_CHARS))
    header += b"".join(struct.pack("<I", c) for c in WEB_CHARS)
    records_at = len(header)
    blob = bytearray()
    records = []
    base = records_at + 12 * len(fonts)
    for fam, style, px, ttf, fallback_ttf in fonts:
        font = ImageFont.truetype(ttf, px)
        fallback = ImageFont.truetype(fallback_ttf, px)
        notdef = font.getmask("\U000F0000")
        ascent, descent = font.getmetrics()
        height = ascent + descent
        table = bytearray()
        bitmaps = bytearray()
        glyphs = [web_glyph(font, notdef, fallback, cp, ascent, height) for cp in WEB_CHARS]
        table_at = base + len(blob)
        bits_at = table_at + 12 * len(glyphs)
        for adv, xoff, width, top, nrows, bits in glyphs:
            assert adv < 65536 and width < 256 and -128 <= xoff < 128
            table += struct.pack("<HbBBB2xI", adv, xoff, width, top, nrows, bits_at + len(bitmaps))
            bitmaps += bits
        blob += table + bitmaps
        records.append(struct.pack("<BBBBB3xI", fam, style, px, height, ascent, table_at))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(header + b"".join(records) + blob)


if __name__ == "__main__":
    import sys
    if sys.argv[1:] == ["--web-only"]:
        write_webfonts("rootfs/usr/share/fonts/web.fnt")
        raise SystemExit(0)
    write_font8x16("lib/font8x16.c")
    write_sans("user/desktop/font_sans.c")
    write_archivo("user/desktop/font_archivo.c")
    write_webfonts("rootfs/usr/share/fonts/web.fnt")
