#!/usr/bin/env python3
"""
Fetch the Tango icons the desktop uses from Wikimedia Commons and write
them, scaled to 16, 32 and 48 pixels, to rootfs/usr/share/icons/tango.
The output is checked in, so building fluxOS does not need this script,
network access, Python or PIL.

The Tango Desktop Project released these icons into the public domain:
https://commons.wikimedia.org/wiki/Tango_icons

usage: tools/tango-icons.py   (run from the top of the source tree)
"""
import io
import os
import urllib.request

from PIL import Image

# Commons file name -> icon name in fluxOS (the freedesktop.org name)
ICONS = {
    "Start-here.svg": "start-here",
    "Computer.svg": "computer",
    "Utilities-terminal.svg": "utilities-terminal",
    "Text-x-generic.svg": "text-x-generic",
    "Application-x-executable.svg": "application-x-executable",
    "System-shutdown.svg": "system-shutdown",
    "Dialog-information.svg": "dialog-information",
    "Dialog-warning.svg": "dialog-warning",
}
SIZES = (16, 32, 48)
SOURCE_PX = 250         # a standard Commons thumbnail width, scaled down from here
OUT = "rootfs/usr/share/icons/tango"
URL = "https://commons.wikimedia.org/wiki/Special:FilePath/%s?width=%d"


def fetch(name):
    req = urllib.request.Request(URL % (name, SOURCE_PX),
                                 headers={"User-Agent": "fluxOS-build/1.0 (tools/tango-icons.py)"})
    with urllib.request.urlopen(req) as r:
        return Image.open(io.BytesIO(r.read())).convert("RGBA")


def main():
    for size in SIZES:
        os.makedirs("%s/%dx%d" % (OUT, size, size), exist_ok=True)
    for commons, name in ICONS.items():
        src = fetch(commons)
        for size in SIZES:
            # Box filtering from a large rendering keeps thin lines (the text
            # in text-x-generic) that smoother filters blur into grey.
            img = src.resize((size, size), Image.BOX)
            img.save("%s/%dx%d/%s.png" % (OUT, size, size, name), optimize=True)
        print(name)
    with open(OUT + "/README", "w") as f:
        f.write("Icons from the Tango Desktop Project, released into the public domain.\n"
                "Source: https://commons.wikimedia.org/wiki/Tango_icons\n"
                "Regenerate with tools/tango-icons.py.\n")


if __name__ == "__main__":
    main()
