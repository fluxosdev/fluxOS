#!/bin/sh
# Build the GNU programs that run on fluxOS (phase 2 and 3 of the
# self-hosting roadmap): GNU make and binutils, cross-compiled with the
# i686-fluxos toolchain into static programs for the system image.
#
#   tools/ports/build.sh [--toolchain DIR] [--sysroot DIR] [--jobs N] [--cache DIR]
#                        [--work DIR] [--dest DIR] [--only make|binutils]
#
# The toolchain is build/toolchain/cross ("make toolchain"), the sources
# the ones it was built from (binutils) plus GNU make, fetched into the
# same cache and verified. The programs go to DEST (build/ports/root) laid
# out as they are installed on fluxOS: usr/bin/make, usr/bin/as, ...
# "make ports" runs this, and the system image then carries them.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
TARGET=i686-fluxos
BINUTILS=binutils-2.42
BINUTILS_SHA256=f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800
MAKE=make-4.4.1
MAKE_SHA256=dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3

toolchain="$top/build/toolchain/cross"
sysroot="$top/build/sysroot"
cache="$top/build/toolchain/dl"
work="$top/build/ports"
dest="$top/build/ports/root"
jobs=$(nproc 2>/dev/null || echo 2)
only=
while [ $# -gt 0 ]; do
    case "$1" in
    --toolchain) toolchain=$2; shift 2 ;;
    --sysroot) sysroot=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    --cache) cache=$2; shift 2 ;;
    --work) work=$2; shift 2 ;;
    --dest) dest=$2; shift 2 ;;
    --only) only=$2; shift 2 ;;
    *) echo "usage: $0 [--toolchain DIR] [--sysroot DIR] [--jobs N] [--cache DIR] [--work DIR] [--dest DIR] [--only make|binutils]" >&2; exit 2 ;;
    esac
done
# Absolute, as the builds change directory.
mkdir -p "$cache" "$work" "$dest"
toolchain=$(cd "$toolchain" 2>/dev/null && pwd) || { echo "$0: no toolchain directory: run 'make toolchain' first" >&2; exit 1; }
sysroot=$(cd "$sysroot" && pwd)
cache=$(cd "$cache" && pwd)
work=$(cd "$work" && pwd)
dest=$(cd "$dest" && pwd)
[ -x "$toolchain/bin/$TARGET-gcc" ] || { echo "$0: no $toolchain/bin/$TARGET-gcc: run 'make toolchain' first" >&2; exit 1; }
[ -f "$sysroot/usr/lib/libc.a" ] || { echo "$0: $sysroot has no libc.a: run 'make sysroot' first" >&2; exit 1; }
mkdir -p "$cache" "$work" "$dest/usr/bin"
export PATH="$toolchain/bin:$PATH"
export CC="$TARGET-gcc" CFLAGS="-O2" LDFLAGS="-static"

# fetch NAME EXT SHA256 URL: NAME.EXT in the cache, verified.
fetch() {
    f="$cache/$1.$2"
    if [ ! -f "$f" ]; then
        echo "fetching $4"
        curl -fSL -o "$f.part" "$4"
        mv "$f.part" "$f"
    fi
    echo "$3  $f" | sha256sum -c - >/dev/null || { echo "$f: wrong checksum" >&2; exit 1; }
}

# fresh DIR: an empty directory under the work directory.
fresh() {
    rm -rf "${work:?}/${1:?}"
    mkdir -p "$work/$1"
}

# unpack NAME EXT [PATCH]: a fresh tree in the work directory, with fluxOS's config.sub.
unpack() {
    fresh "$1"
    echo "unpacking $1"
    tar -C "$work" -xf "$cache/$1.$2"
    [ -z "$3" ] || (cd "$work/$1" && patch -p1 -s < "$here/../toolchain/patches/$3")
    for sub in $(find "$work/$1" -name config.sub); do
        cp "$here/../toolchain/config.sub" "$sub"
    done
}

build_make() {
    fetch $MAKE tar.gz $MAKE_SHA256 "https://ftp.gnu.org/gnu/make/$MAKE.tar.gz"
    unpack $MAKE tar.gz
    fresh build-make
    cd "$work/build-make"
    "$work/$MAKE/configure" --host=$TARGET --prefix=/usr --disable-nls --without-guile --disable-load \
        >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    make -j"$jobs" >build.log 2>&1 || { tail -40 build.log; exit 1; }
    "$TARGET-strip" -o "$dest/usr/bin/make" make
    echo "make: $dest/usr/bin/make"
}

build_binutils() {
    fetch $BINUTILS tar.xz $BINUTILS_SHA256 "https://ftp.gnu.org/gnu/binutils/$BINUTILS.tar.xz"
    unpack $BINUTILS tar.xz $BINUTILS-fluxos.patch
    fresh build-binutils
    cd "$work/build-binutils"
    "$work/$BINUTILS/configure" --host=$TARGET --target=$TARGET --prefix=/usr --program-prefix= \
        --disable-nls --disable-werror --disable-gdb --disable-gprofng --disable-sim --disable-libdecnumber \
        --disable-readline --disable-shared --enable-static \
        >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    make -j"$jobs" MAKEINFO=true >build.log 2>&1 || { tail -40 build.log; exit 1; }
    for prog in gas/as-new:as ld/ld-new:ld binutils/ar:ar binutils/ranlib:ranlib binutils/nm-new:nm \
                binutils/objcopy:objcopy binutils/objdump:objdump binutils/readelf:readelf \
                binutils/strip-new:strip binutils/size:size binutils/strings:strings; do
        "$TARGET-strip" -o "$dest/usr/bin/${prog#*:}" "${prog%:*}"
    done
    echo "binutils: $dest/usr/bin/as, ld, ar, ranlib, nm, objcopy, objdump, readelf, strip, size, strings"
}

case "$only" in
"") build_make; build_binutils ;;
make) build_make ;;
binutils) build_binutils ;;
*) echo "$0: --only takes make or binutils" >&2; exit 2 ;;
esac
