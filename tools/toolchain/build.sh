#!/bin/sh
# Build the fluxOS GCC toolchain: binutils and GCC with an i686-fluxos
# target, as a cross compiler that runs on the build host.
#
#   tools/toolchain/build.sh [--prefix DIR] [--sysroot DIR] [--jobs N]
#                            [--cache DIR] [--work DIR] [--only binutils|gcc]
#
# The sysroot must hold fluxOS's headers and libraries first ("make
# sysroot" puts them in build/sysroot). The result is
# PREFIX/bin/i686-fluxos-gcc, -ld, -as, -ar..., with libgcc for the
# target; "make toolchain" runs this with the defaults below.
#
# The same patches (tools/toolchain/patches) are what a GCC that runs on
# fluxOS itself is built from later: see self-hosting-roadmap.md.
set -e

BINUTILS=binutils-2.42
BINUTILS_SHA256=f6e4d41fd5fc778b06b7891457b3620da5ecea1006c6a4a41ae998109f85a800
GCC=gcc-13.2.0
GCC_SHA256=e275e76442a6067341a27f04c5c6b83d8613144004c0413528863dc6b5c743da
TARGET=i686-fluxos

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
prefix="$top/build/toolchain/cross"
sysroot="$top/build/sysroot"
cache="$top/build/toolchain/dl"
work="$top/build/toolchain/src"
jobs=$(nproc 2>/dev/null || echo 2)
only=

while [ $# -gt 0 ]; do
    case "$1" in
    --prefix) prefix=$2; shift 2 ;;
    --sysroot) sysroot=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    --cache) cache=$2; shift 2 ;;
    --work) work=$2; shift 2 ;;
    --only) only=$2; shift 2 ;;
    *) echo "usage: $0 [--prefix DIR] [--sysroot DIR] [--jobs N] [--cache DIR] [--work DIR] [--only binutils|gcc]" >&2; exit 2 ;;
    esac
done

prefix=$(mkdir -p "$prefix" && cd "$prefix" && pwd)
sysroot=$(mkdir -p "$sysroot" && cd "$sysroot" && pwd)
mkdir -p "$cache" "$work"
cache=$(cd "$cache" && pwd)
work=$(cd "$work" && pwd)

if [ ! -f "$sysroot/usr/include/flux/abi.h" ] || [ ! -f "$sysroot/usr/lib/libc.a" ]; then
    echo "$0: $sysroot has no fluxOS headers and libc.a: run 'make sysroot' first" >&2
    exit 1
fi

# fetch NAME SHA256 URL: NAME.tar.xz in the cache, verified.
fetch() {
    f="$cache/$1.tar.xz"
    if [ ! -f "$f" ]; then
        echo "fetching $3"
        curl -fSL -o "$f.part" "$3"
        mv "$f.part" "$f"
    fi
    echo "$2  $f" | sha256sum -c - >/dev/null || { echo "$f: wrong checksum" >&2; exit 1; }
}

# unpack NAME PATCH: a fresh tree in the work directory, patched.
unpack() {
    rm -rf "$work/$1"
    echo "unpacking $1"
    tar -C "$work" -xf "$cache/$1.tar.xz"
    (cd "$work/$1" && patch -p1 -s < "$here/patches/$2")
}

build_binutils() {
    fetch $BINUTILS $BINUTILS_SHA256 "https://ftp.gnu.org/gnu/binutils/$BINUTILS.tar.xz"
    unpack $BINUTILS $BINUTILS-fluxos.patch
    rm -rf "$work/build-binutils"
    mkdir -p "$work/build-binutils"
    cd "$work/build-binutils"
    "$work/$BINUTILS/configure" --target=$TARGET --prefix="$prefix" --with-sysroot="$sysroot" \
        --disable-nls --disable-werror --disable-gdb --disable-gprofng --disable-sim --disable-libdecnumber \
        --disable-readline >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    make -j"$jobs" MAKEINFO=true >build.log 2>&1 || { tail -40 build.log; exit 1; }
    make install MAKEINFO=true >install.log 2>&1 || { tail -20 install.log; exit 1; }
    echo "binutils: $prefix/bin/$TARGET-ld"
}

build_gcc() {
    fetch $GCC $GCC_SHA256 "https://ftp.gnu.org/gnu/gcc/$GCC/$GCC.tar.xz"
    unpack $GCC $GCC-fluxos.patch
    # GMP, MPFR, MPC and ISL, built inside the GCC tree
    (cd "$work/$GCC" && ./contrib/download_prerequisites --directory="$cache" >prerequisites.log 2>&1) || \
        { tail -20 "$work/$GCC/prerequisites.log"; exit 1; }
    for lib in gmp mpfr mpc isl; do
        [ -e "$work/$GCC/$lib" ] || { echo "$GCC/$lib missing after download_prerequisites" >&2; exit 1; }
    done
    rm -rf "$work/build-gcc"
    mkdir -p "$work/build-gcc"
    cd "$work/build-gcc"
    PATH="$prefix/bin:$PATH" "$work/$GCC/configure" --target=$TARGET --prefix="$prefix" --with-sysroot="$sysroot" \
        --enable-languages=c --disable-nls --disable-shared --disable-threads --disable-multilib \
        --disable-libssp --disable-libquadmath --disable-libgomp --disable-libatomic --disable-libstdcxx \
        --disable-fixincludes --disable-lto --with-arch=pentium4 --with-tune=generic \
        >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    PATH="$prefix/bin:$PATH" make -j"$jobs" all-gcc >build-gcc.log 2>&1 || { tail -40 build-gcc.log; exit 1; }
    PATH="$prefix/bin:$PATH" make -j"$jobs" all-target-libgcc >build-libgcc.log 2>&1 || { tail -40 build-libgcc.log; exit 1; }
    PATH="$prefix/bin:$PATH" make install-gcc install-target-libgcc >install.log 2>&1 || { tail -20 install.log; exit 1; }
    echo "gcc: $prefix/bin/$TARGET-gcc"
}

case "$only" in
"") build_binutils; build_gcc ;;
binutils) build_binutils ;;
gcc) build_gcc ;;
*) echo "$0: --only takes binutils or gcc" >&2; exit 2 ;;
esac
echo "toolchain in $prefix (sysroot $sysroot)"
