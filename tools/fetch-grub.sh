#!/bin/sh
# Fetch Debian 12's GRUB 2.06 (BIOS) into DIR; with --firmware, also
# refresh the GRUB images and modules checked into firmware/grub from it.
#
# GRUB 2.12, as Ubuntu 24.04 and Linux Mint 22 ship it, reboots some older
# PCs (an nForce MCP61 board, for one) right after "Welcome to GRUB!", while
# 2.06 starts them fine. The packages are checked against these hashes and
# unpacked with dpkg-deb; nothing is installed on the host.
#
# The build does not run this: the ISO is made from firmware/grub by mkiso.
# It is how those files were made, and how to make them again (for another
# GRUB, or another module on the menus); "make MKRESCUE=..." still builds
# the ISO with grub-mkrescue from DIR.
#
# usage: tools/fetch-grub.sh [--firmware] DIR
set -e
firmware=
if [ "$1" = --firmware ]; then
    firmware=1
    shift
fi
dir=$1
[ -n "$dir" ] || { echo "usage: $0 [--firmware] DIR" >&2; exit 2; }
ver=2.06-13+deb12u2
url=https://deb.debian.org/debian/pool/main/g/grub2
if [ ! -x "$dir/usr/bin/grub-mkimage" ]; then
    tmp=$dir.tmp
    rm -rf "$tmp"
    mkdir -p "$tmp/deb"
    while read -r sum pkg; do
        deb=${pkg}_${ver}_amd64.deb
        echo "fetching $deb"
        curl -fsSL -o "$tmp/deb/$deb" "$url/$deb"
        echo "$sum  $tmp/deb/$deb" | sha256sum -c --quiet
        dpkg-deb -x "$tmp/deb/$deb" "$tmp"
    done <<LIST
5067186a3bf88a99555069b9b29203ae2facec3372746ac93db1b02e92ef933e grub2-common
fa2a91e48bb85effb1d9e22e16d83774e6de0ae6167268e8150e76ae963ec671 grub-common
db6aced5a89f02e10e150140b4df9621196f311c26203baf8558b4c6808e1298 grub-pc-bin
LIST
    rm -rf "$tmp/deb" "$dir"
    mv "$tmp" "$dir"
fi
"$dir/usr/bin/grub-mkimage" --version
[ -n "$firmware" ] || exit 0

# ---- firmware/grub ----
pc=$dir/usr/lib/grub/i386-pc
out=$(dirname "$0")/../firmware/grub
rm -rf "$out"
mkdir -p "$out/i386-pc"
# The hard disk's: boot.img in the MBR, core.img in the gap before the
# FAT32 partition, reading its menu and modules from /boot/grub there
# (fluxinstall writes both).
cp "$pc/boot.img" "$out/boot.img"
"$dir/usr/bin/grub-mkimage" -O i386-pc -d "$pc" -o "$out/core.img" \
    -p '(,msdos1)/boot/grub' biosdisk part_msdos fat
# The CD's: El Torito's no-emulation image, reading /boot/grub from the
# drive it started from (a CD, or a USB stick the ISO was written to, which
# boot_hybrid.img in the ISO's first sector starts).
"$dir/usr/bin/grub-mkimage" -O i386-pc-eltorito -d "$pc" -o "$out/i386-pc/eltorito.img" \
    -p /boot/grub biosdisk iso9660
cp "$pc/boot_hybrid.img" "$out/boot_hybrid.img"
# The modules the two menus (grub/install.cfg, grub/grub.cfg) load, their
# commands' modules (command.lst), a few for GRUB's own command line, and
# everything those depend on (moddep.lst).
mods="normal biosdisk part_msdos fat iso9660 all_video vbe vga video_bochs video_cirrus gfxterm
      terminal probe loadenv multiboot chain test echo sleep configfile ls cat help reboot halt"
for cmd in $(sed -e 's/#.*//' grub/grub.cfg grub/install.cfg | awk 'NF { print $1 }' | sort -u); do
    m=$(awk -F': ' -v c="$cmd" '{ sub(/^\*/, "", $1) } $1 == c { print $2 }' "$pc/command.lst")
    mods="$mods $m"
done
todo=$mods
all=
while [ -n "$(echo $todo)" ]; do
    next=
    for m in $todo; do
        case " $all " in *" $m "*) continue ;; esac
        all="$all $m"
        next="$next $(awk -F: -v m="$m" '$1 == m { print $2 }' "$pc/moddep.lst")"
    done
    todo=$next
done
for m in $all; do
    cp "$pc/$m.mod" "$out/i386-pc/"
done
cp "$pc"/*.lst "$out/i386-pc/"
(cd "$out" && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | LC_ALL=C sort | xargs sha256sum > SHA256SUMS)
echo "$out: $(echo $all | wc -w) modules from GRUB $ver"
