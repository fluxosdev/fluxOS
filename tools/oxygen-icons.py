#!/usr/bin/env python3
"""
Copy the Oxygen icons the desktop uses (16, 32 and 48 pixels) from an
installed Oxygen theme to rootfs/usr/share/icons/oxygen. The output is
checked in, so building fluxOS does not need this script. Oxygen is
LGPL-3.0-or-later: https://invent.kde.org/frameworks/oxygen-icons5

usage: tools/oxygen-icons.py [THEME_DIR]   (run from the top of the source tree)
  THEME_DIR defaults to /usr/share/icons/oxygen/base; on Debian/Ubuntu,
  "apt-get download oxygen-icon-theme" and "dpkg-deb -x" it anywhere.
"""
import os
import shutil
import sys

# icon name in fluxOS (the freedesktop.org name) -> name in Oxygen, if different
ICONS = {
    "computer": None, "utilities-terminal": None, "text-x-generic": None,
    "application-x-executable": None, "system-shutdown": None,
    "dialog-information": None, "dialog-warning": None, "folder": None,
    "folder-open": None, "folder-new": None, "user-home": None,
    "drive-harddisk": None, "media-cdrom": "media-optical",
    "image-x-generic": None, "go-up": None, "edit-delete": None,
    "system-installer": "package-x-generic", "accessories-text-editor": None,
    "internet-web-browser": "applications-internet", "accessories-calculator": None,
    "text-html": None, "go-previous": None, "go-next": None, "go-home": None,
    "view-refresh": None, "process-stop": None, "video-display": None,
    "applications-system": None, "audio-volume-high": None,
    "audio-volume-muted": None, "audio-x-generic": None,
    "applications-graphics": None, "utilities-file-archiver": None,
    "utilities-system-monitor": None, "edit-bomb": None, "clock": None,
    "preferences-system-time": None,
    "battery-caution": None, "battery-low": None, "battery-040": None,
    "battery-060": None, "battery-080": None, "battery-100": None,
    "battery-charging-caution": None, "battery-charging-low": None,
    "battery-charging-040": None, "battery-charging-060": None,
    "battery-charging-080": None, "battery-charging": None, "battery-missing": None,
    "network-wireless": None, "input-mouse": None,
    "emblem-favorite": "rating", "video-x-generic": None,
}
SIZES = (16, 32, 48)
OUT = "rootfs/usr/share/icons/oxygen"


def find(theme, size, name):
    d = "%s/%dx%d" % (theme, size, size)
    for root, _, files in os.walk(d):
        if name + ".png" in files:
            return os.path.join(root, name + ".png")
    sys.exit("no %s at %dx%d in %s" % (name, size, size, theme))


def main():
    theme = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/icons/oxygen/base"
    for size in SIZES:
        os.makedirs("%s/%dx%d" % (OUT, size, size), exist_ok=True)
        for name, source in ICONS.items():
            shutil.copyfile(find(theme, size, source or name),
                            "%s/%dx%d/%s.png" % (OUT, size, size, name))
    print("%d icons at %d sizes" % (len(ICONS), len(SIZES)))


if __name__ == "__main__":
    main()
