#!/usr/bin/env python3
"""
Render the Breeze icons the desktop uses (16, 32 and 48 pixels) from an
installed Breeze theme into rootfs/usr/share/icons/breeze, as PNG. The
output is checked in, so building fluxOS does not need this script.
Breeze is LGPL-3.0-or-later: https://invent.kde.org/frameworks/breeze-icons

Breeze is drawn as SVG at a few design sizes (16, 22, 32, 48, 64...),
which differ by context, and many of its small designs are monochrome
glyphs (one dark ink, for toolbars) where the larger ones are in colour.
Coloured designs are favoured: each icon is rendered from a coloured
design if it has any, else from its monochrome ones. Among those, it is
the design made for the size wanted if there is one, else the next
larger one (the small ones are simplified), else the largest there is.
A design counts as monochrome if, rendered, everything opaque in it is a
dark gray (Breeze's glyphs in red or blue, like edit-delete, are colour).

usage: tools/breeze-icons.py [THEME_DIR]   (run from the top of the source tree)
  THEME_DIR defaults to /usr/share/icons/breeze; on Debian/Ubuntu,
  "apt-get download breeze-icon-theme" and "dpkg-deb -x" it anywhere.
  Needs cairosvg ("pip install cairosvg", in a venv if need be).
"""
import io
import os
import re
import shutil
import sys

import cairosvg
from PIL import Image

# icon name in fluxOS (the freedesktop.org name) -> name in Breeze, if different
ICONS = {
    "computer": None, "utilities-terminal": None, "text-x-generic": None,
    "application-x-executable": None, "system-shutdown": None,
    "dialog-information": None, "dialog-warning": None, "folder": None,
    "folder-open": None, "folder-new": None, "user-home": None,
    "drive-harddisk": None, "media-cdrom": "media-optical",
    "image-x-generic": None, "go-up": None, "edit-delete": None,
    "system-installer": "system-software-install", "accessories-text-editor": None,
    "internet-web-browser": None, "accessories-calculator": None,
    "text-html": None, "go-previous": None, "go-next": None, "go-home": None,
    "view-refresh": None, "process-stop": None, "video-display": None,
    "applications-system": None, "audio-volume-high": None,
    "audio-volume-muted": None, "audio-x-generic": None,
    "applications-graphics": None, "utilities-file-archiver": None,
    "utilities-system-monitor": None, "edit-bomb": None, "clock": "preferences-system-time",
    "preferences-system-time": None,
    # Breeze's battery-low is its 40% one, the same as battery-040: 20% keeps the six steps apart.
    "battery-caution": None, "battery-low": "battery-020", "battery-040": None,
    "battery-060": None, "battery-080": None, "battery-100": None,
    "battery-charging-caution": "battery-caution-charging",
    "battery-charging-low": "battery-020-charging",
    "battery-charging-040": "battery-040-charging", "battery-charging-060": "battery-060-charging",
    "battery-charging-080": "battery-080-charging", "battery-charging": "battery-100-charging",
    "battery-missing": None,
    "network-wireless": None, "input-mouse": None,
    # Breeze's emblem-favorite and clock are monochrome only: Files' Favourites gets the coloured
    # folder-favorites, the Date and Time window the coloured preferences-system-time.
    "emblem-favorite": "folder-favorites", "video-x-generic": None,
}
SIZES = (16, 32, 48)
OUT = "rootfs/usr/share/icons/breeze"
# fluxOS's own (the logo and Wave's), not Breeze's: the same files as in the Oxygen set
OWN = ("fluxos", "wave")
OWN_SIZES = (16, 24, 32, 48)
OWN_FROM = "rootfs/usr/share/icons/oxygen"


def designs(theme, name):
    """{design size: path} for name, in any context (not the symbolic or @2x ones)."""
    found = {}
    for context in sorted(os.listdir(theme)):
        cdir = os.path.join(theme, context)
        if not os.path.isdir(cdir):
            continue
        for sub in os.listdir(cdir):
            if not re.fullmatch(r"\d+", sub):
                continue
            path = os.path.join(cdir, sub, name + ".svg")
            if os.path.exists(path):
                found.setdefault(int(sub), os.path.realpath(path))
    return found


def monochrome(path):
    """Is the design a glyph in one dark ink (anything opaque is a dark, unsaturated gray)?"""
    png = cairosvg.svg2png(url=path, output_width=32, output_height=32)
    opaque = [p for p in Image.open(io.BytesIO(png)).convert("RGBA").getdata() if p[3] >= 96]
    return all(max(p[:3]) - min(p[:3]) <= 40 and sum(p[:3]) <= 3 * 170 for p in opaque)


def pick(found, size):
    """The design to render at size: a coloured one if there are any."""
    coloured = {s: path for s, path in found.items() if not monochrome(path)}
    found = coloured or found
    if size in found:
        return found[size]
    larger = [s for s in found if s > size]
    return found[min(larger)] if larger else found[max(found)]


def main():
    theme = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/icons/breeze"
    for size in sorted(set(SIZES) | set(OWN_SIZES)):
        os.makedirs("%s/%dx%d" % (OUT, size, size), exist_ok=True)
    for name, source in ICONS.items():
        found = designs(theme, source or name)
        if not found:
            sys.exit("no %s in %s" % (source or name, theme))
        for size in SIZES:
            cairosvg.svg2png(url=pick(found, size), write_to="%s/%dx%d/%s.png" % (OUT, size, size, name),
                             output_width=size, output_height=size)
    for name in OWN:
        for size in OWN_SIZES:
            shutil.copyfile("%s/%dx%d/%s.png" % (OWN_FROM, size, size, name),
                            "%s/%dx%d/%s.png" % (OUT, size, size, name))
    print("%d icons at %d sizes" % (len(ICONS), len(SIZES)))


if __name__ == "__main__":
    main()
