#!/usr/bin/env python3
"""
Gather the Noto fonts the browser falls back on for scripts that web.fnt
(Liberation: Latin, Greek, Cyrillic) does not cover, into
rootfs/usr/share/fonts/noto/. The output is checked in, so building fluxOS
does not need this script, the fonts or a C compiler.

  *.ttf   Noto Sans for each script, copied as they are (SIL Open Font
          License 1.1). webfont.c loads one the first time a character in
          its range turns up.
  cjk.fxo the Han, kana and Hangul glyphs of Noto Sans CJK as outlines,
          packed by tools/mkcjk.c: the code points of the GB 2312, Big5,
          JIS X 0208 and KS X 1001 character sets plus punctuation and
          the full-width forms. The 19 MB font would not fit the initrd.

usage: tools/mknoto.py   (run from the top of the source tree)
"""
import os
import shutil
import subprocess
import sys

NOTO = "/usr/share/fonts/truetype/noto/"
CJK = "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"
OUT = "rootfs/usr/share/fonts/noto/"
FONTS = ["NotoSans", "NotoSansSymbols", "NotoSansSymbols2", "NotoSansMath", "NotoSansArabic", "NotoSansHebrew", "NotoSansThai",
         "NotoSansLao", "NotoSansDevanagari", "NotoSansBengali", "NotoSansGurmukhi", "NotoSansGujarati",
         "NotoSansOriya", "NotoSansTamil", "NotoSansTelugu", "NotoSansKannada", "NotoSansMalayalam",
         "NotoSansSinhala", "NotoSansKhmer", "NotoSansMyanmar", "NotoSansArmenian", "NotoSansGeorgian",
         "NotoSansEthiopic"]


def codepoints():
    cps = set()
    for codec, lead in (("gb2312", range(0xA1, 0xF8)), ("big5", range(0xA1, 0xFA)),
                        ("shift_jis", range(0x81, 0xF0)), ("euc_kr", range(0xB0, 0xCA))):
        for a in lead:
            for b in range(0xA1, 0xFF) if codec != "shift_jis" else range(0x40, 0xFD):
                try:
                    ch = bytes([a, b]).decode(codec)
                except UnicodeDecodeError:
                    continue
                if len(ch) == 1:
                    cps.add(ord(ch))
    cps |= set(range(0x2E80, 0x2FE0)) - set(range(0x2FD6, 0x2FE0))   # radicals
    cps |= set(range(0x3000, 0x3100)) | set(range(0x3130, 0x3190)) | set(range(0x31F0, 0x3200))
    cps |= set(range(0x3200, 0x3300)) | set(range(0xFE30, 0xFE50)) | set(range(0xFF00, 0xFFF0))
    cps |= set(range(0x2460, 0x2500)) & set()                      # enclosed numbers: Symbols has them
    return sorted(c for c in cps if c >= 0x2E80)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in FONTS:
        shutil.copy(NOTO + name + "-Regular.ttf", OUT + name + "-Regular.ttf")
    cps = codepoints()
    tmp = os.environ.get("TMPDIR", "/tmp")
    with open(tmp + "/cjk-codepoints.txt", "w") as f:
        f.write("\n".join("%x" % c for c in cps))
    ft = "third_party/freetype"
    srcs = [ft + "/src/" + s for s in (
        "base/ftbase.c base/ftinit.c base/ftsystem.c base/ftdebug.c base/ftbitmap.c base/ftglyph.c "
        "base/ftmm.c base/ftfstype.c sfnt/sfnt.c truetype/truetype.c cff/cff.c psnames/psnames.c "
        "psaux/psaux.c pshinter/pshinter.c raster/raster.c smooth/smooth.c gzip/ftgzip.c").split()]
    srcs += [os.path.join(d, f) for d in ("third_party/brotli/c/common", "third_party/brotli/c/dec")
             for f in os.listdir(d) if f.endswith(".c")]
    tool = tmp + "/mkcjk"
    subprocess.check_call(["gcc", "-std=gnu11", "-O1", "-I" + ft + "/include", "-I" + ft,
                           "-Ithird_party/brotli/c/include", "-DFT2_BUILD_LIBRARY",
                           '-DFT_CONFIG_MODULES_H="flux_ftmodule.h"', '-DFT_CONFIG_OPTIONS_H="flux_ftoption.h"',
                           '-DFT_CONFIG_STANDARD_LIBRARY_H="flux_ftstdlib.h"', "tools/mkcjk.c"] + srcs +
                          ["-o", tool])
    subprocess.check_call([tool, CJK, tmp + "/cjk-codepoints.txt", OUT + "cjk.fxo"])


if __name__ == "__main__":
    sys.exit(main())
