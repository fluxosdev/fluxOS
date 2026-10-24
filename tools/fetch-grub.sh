#!/bin/sh
# Fetch Debian 12's GRUB 2.06 (BIOS) into DIR, for building the ISO.
#
# GRUB 2.12, as Ubuntu 24.04 and Linux Mint 22 ship it, reboots some older
# PCs (an nForce MCP61 board, for one) right after "Welcome to GRUB!", while
# 2.06 starts them fine. The packages are checked against these hashes and
# unpacked with dpkg-deb; nothing is installed on the host.
#
# usage: tools/fetch-grub.sh DIR
set -e
dir=$1
[ -n "$dir" ] || { echo "usage: $0 DIR" >&2; exit 2; }
ver=2.06-13+deb12u2
url=https://deb.debian.org/debian/pool/main/g/grub2
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
"$dir/usr/bin/grub-mkimage" --version
