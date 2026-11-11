#!/bin/sh
# Make a small CD that starts a kernel through Limine with one file as its
# initrd: what "make run-kernel" and the QEMU tests boot (QEMU itself can
# only load a kernel the Multiboot way, which a 64-bit kernel is not).
#
# usage: tools/bootiso.sh OUT.iso KERNEL INITRD "CMDLINE" [MKISO [LIMINE-DIR]]
#
# INITRD may be "" for none (the kernel then needs root= on its command
# line). MKISO is the build's (build/host/mkiso) and LIMINE-DIR the tree's
# firmware/limine unless given. Limine starts the one entry at once and
# reports on the serial port; the screen is in 1024x768 graphics unless the
# command line has "textmode", which is taken off it and asks Limine for a
# BIOS's text mode instead.
set -e
[ $# -ge 4 ] || { echo "usage: $0 OUT.iso KERNEL INITRD CMDLINE [MKISO [LIMINE-DIR]]" >&2; exit 2; }
out=$1 kernel=$2 initrd=$3 cmdline=$4
top=$(dirname "$0")/..
mkiso=${5:-$top/build/host/mkiso}
limine=${6:-$top/firmware/limine}

tree=$out.tree
rm -rf "$tree"
mkdir -p "$tree/boot/limine"
cp "$kernel" "$tree/boot/fluxos.kernel"
cp "$limine/limine-bios.sys" "$limine/limine-bios-cd.bin" "$limine/limine-uefi-cd.bin" "$tree/boot/limine/"

mode="resolution: 1024x768x32"
case " $cmdline " in
*" textmode "*)
    mode="textmode: yes"
    cmdline=$(printf '%s' " $cmdline " | sed 's/ textmode / /; s/^ *//; s/ *$//')
    ;;
esac
{
    echo "timeout: 0"
    echo "serial: yes"
    echo
    echo "/fluxOS"
    echo "    protocol: limine"
    echo "    path: boot():/boot/fluxos.kernel"
    echo "    cmdline: $cmdline"
    if [ -n "$initrd" ]; then
        cp "$initrd" "$tree/boot/initrd.tar"
        echo "    module_path: boot():/boot/initrd.tar"
    fi
    echo "    $mode"
} > "$tree/boot/limine/limine.conf"

"$mkiso" -o "$out" -b boot/limine/limine-bios-cd.bin -e boot/limine/limine-uefi-cd.bin "$tree"
rm -rf "$tree"
