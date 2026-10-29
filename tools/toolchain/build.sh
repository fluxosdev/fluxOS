#!/bin/sh
# Build the fluxOS GCC toolchain: binutils and GCC with an i686-fluxos
# target, as a cross compiler that runs on the build host, and then as a
# GCC that runs on fluxOS itself.
#
#   tools/toolchain/build.sh [--prefix DIR] [--sysroot DIR] [--jobs N]
#                            [--cache DIR] [--work DIR] [--only binutils|gcc]
#   tools/toolchain/build.sh --native [--dest DIR] ...
#
# The sysroot must hold fluxOS's headers and libraries first ("make
# sysroot" puts them in build/sysroot). The result is
# PREFIX/bin/i686-fluxos-gcc, -g++, -ld, -as, -ar..., with libgcc and
# libstdc++ for the target; "make toolchain" runs this with the defaults
# below.
#
# --native is the Canadian cross (phase 4 of self-hosting-roadmap.md): with
# the cross toolchain on the path, GCC is configured --host=i686-fluxos
# --target=i686-fluxos --prefix=/usr and installed under DEST
# (build/toolchain/native/root) laid out as it goes onto the system
# image: usr/bin/gcc, usr/libexec/gcc/i686-fluxos/13.2.0/cc1, ... Its
# assembler and linker are the binutils "make ports" builds. "make
# toolchain-native" runs this.
#
# The same patches (tools/toolchain/patches) serve both.
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
dest="$top/build/toolchain/native/root"
jobs=$(nproc 2>/dev/null || echo 2)
only=
native=
resume=

while [ $# -gt 0 ]; do
    case "$1" in
    --prefix) prefix=$2; shift 2 ;;
    --sysroot) sysroot=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    --cache) cache=$2; shift 2 ;;
    --work) work=$2; shift 2 ;;
    --dest) dest=$2; shift 2 ;;
    --only) only=$2; shift 2 ;;
    --native) native=1; shift ;;
    --continue) resume=1; shift ;;
    *) echo "usage: $0 [--native] [--prefix DIR] [--sysroot DIR] [--dest DIR] [--jobs N] [--cache DIR] [--work DIR] [--only binutils|gcc]" >&2; exit 2 ;;
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

# unpack NAME PATCH [DIR]: a fresh tree DIR/NAME (DIR: the work directory), patched.
unpack() {
    d=${3:-$work}
    mkdir -p "$d"
    rm -rf "$d/$1"
    echo "unpacking $1"
    tar -C "$d" -xf "$cache/$1.tar.xz"
    (cd "$d/$1" && patch -p1 -s < "$here/patches/$2")
}

# prerequisites DIR: GMP, MPFR, MPC and ISL inside the GCC tree at DIR.
prerequisites() {
    (cd "$1" && ./contrib/download_prerequisites --directory="$cache" >prerequisites.log 2>&1) || \
        { tail -20 "$1/prerequisites.log"; exit 1; }
    for lib in gmp mpfr mpc isl; do
        [ -e "$1/$lib" ] || { echo "$1/$lib missing after download_prerequisites" >&2; exit 1; }
    done
}

# What both GCCs are configured with: C and C++, static, one thread model
# (libstdc++'s gthr-single: the ports do not need threads), constructors in
# .init_array (the C library's startup code runs them; there is no crti.o),
# and no __thread variables (the C library has no static TLS: GCC emulates
# them through libgcc instead).
GCC_OPTIONS="--enable-languages=c,c++ --disable-nls --disable-shared --disable-threads --disable-tls
    --enable-initfini-array --disable-multilib --disable-libssp --disable-libquadmath --disable-libgomp
    --disable-libatomic --disable-libstdcxx-pch --disable-fixincludes --disable-lto --disable-plugin
    --with-arch=pentium4 --with-tune=generic"

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
    prerequisites "$work/$GCC"
    rm -rf "$work/build-gcc"
    mkdir -p "$work/build-gcc"
    cd "$work/build-gcc"
    PATH="$prefix/bin:$PATH" "$work/$GCC/configure" --target=$TARGET --prefix="$prefix" --with-sysroot="$sysroot" \
        $GCC_OPTIONS >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    PATH="$prefix/bin:$PATH" make -j"$jobs" all-gcc >build-gcc.log 2>&1 || { tail -40 build-gcc.log; exit 1; }
    PATH="$prefix/bin:$PATH" make -j"$jobs" all-target-libgcc >build-libgcc.log 2>&1 || { tail -40 build-libgcc.log; exit 1; }
    PATH="$prefix/bin:$PATH" make install-gcc install-target-libgcc >install.log 2>&1 || { tail -20 install.log; exit 1; }
    PATH="$prefix/bin:$PATH" make -j"$jobs" all-target-libstdc++-v3 >build-libstdc++.log 2>&1 || { tail -40 build-libstdc++.log; exit 1; }
    PATH="$prefix/bin:$PATH" make install-target-libstdc++-v3 >>install.log 2>&1 || { tail -20 install.log; exit 1; }
    echo "gcc: $prefix/bin/$TARGET-gcc, $TARGET-g++"
}

# The GCC that runs on fluxOS. Built in its own tree, since GMP and the
# others are configured for the fluxOS host this time (their config.sub
# files learn the host from tools/toolchain/config.sub, as the ports do).
build_native() {
    [ -x "$prefix/bin/$TARGET-g++" ] || { echo "$0: no $prefix/bin/$TARGET-g++: run 'make toolchain' first" >&2; exit 1; }
    src="$work/native/$GCC"
    if [ -n "$resume" ] && [ -f "$work/build-native-gcc/Makefile" ]; then
        cd "$work/build-native-gcc"
        native_make
        return
    fi
    fetch $GCC $GCC_SHA256 "https://ftp.gnu.org/gnu/gcc/$GCC/$GCC.tar.xz"
    unpack $GCC $GCC-fluxos.patch "$work/native"
    prerequisites "$src"
    # download_prerequisites links the libraries in from the cache; this tree
    # gets its own copies, since their config.sub files are replaced below.
    for lib in gmp mpfr mpc isl; do
        if [ -L "$src/$lib" ]; then
            real=$(readlink "$src/$lib")
            rm "$src/$lib"
            cp -rp "$real" "$src/$lib"      # with their times: autotools outputs must stay newer than their inputs
        fi
    done
    for sub in $(find "$src" -name config.sub); do
        cp "$here/config.sub" "$sub"
    done
    build=$(sh "$src/config.guess")
    rm -rf "$work/build-native-gcc"
    mkdir -p "$work/build-native-gcc"
    cd "$work/build-native-gcc"
    # What configure cannot find out by running a program on the host: the
    # compiler's page allocator takes its pages from malloc (the kernel's
    # mapping table is small; the heap can grow to the mappings).
    PATH="$prefix/bin:$PATH" \
    gcc_cv_func_mmap_file=no gcc_cv_func_mmap_dev_zero=no gcc_cv_func_mmap_anon=no \
    "$src/configure" --build="$build" --host=$TARGET --target=$TARGET --prefix=/usr \
        --with-sysroot=/ --with-build-sysroot="$sysroot" --with-native-system-header-dir=/usr/include \
        $GCC_OPTIONS >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    native_make
}

# The native build's make and install steps (from the build directory).
native_make() {
    PATH="$prefix/bin:$PATH" make -j"$jobs" MAKEINFO=true all-gcc >build-gcc.log 2>&1 || { tail -40 build-gcc.log; exit 1; }
    PATH="$prefix/bin:$PATH" make -j"$jobs" MAKEINFO=true all-target-libgcc >build-libgcc.log 2>&1 || { tail -40 build-libgcc.log; exit 1; }
    rm -rf "$dest"
    mkdir -p "$dest"
    dest=$(cd "$dest" && pwd)
    PATH="$prefix/bin:$PATH" make DESTDIR="$dest" MAKEINFO=true install-gcc install-target-libgcc >install.log 2>&1 || \
        { tail -20 install.log; exit 1; }
    # Only what runs on fluxOS: no manuals, no .la files, no build-machine
    # tools, and the programs stripped (cc1 is 40 MB with its symbols).
    rm -rf "$dest/usr/share" "$dest/usr/lib/gcc/$TARGET/13.2.0/plugin" "$dest/usr/lib/gcc/$TARGET/13.2.0/install-tools" \
        "$dest/usr/libexec/gcc/$TARGET/13.2.0/install-tools" "$dest/usr/libexec/gcc/$TARGET/13.2.0/plugin" \
        "$dest/usr/libexec/gcc/$TARGET/13.2.0/lto-wrapper"
    find "$dest" -name '*.la' -delete
    rm -f "$dest/usr/bin/$TARGET-"* "$dest/usr/bin/gcc-ar" "$dest/usr/bin/gcc-nm" "$dest/usr/bin/gcc-ranlib" \
        "$dest/usr/bin/gcov" "$dest/usr/bin/gcov-dump" "$dest/usr/bin/gcov-tool" "$dest/usr/bin/lto-dump"
    rm -f "$dest/usr/lib/gcc/$TARGET/13.2.0/libgcov.a"
    for f in "$dest"/usr/bin/* "$dest"/usr/libexec/gcc/$TARGET/13.2.0/*; do
        [ -f "$f" ] && "$prefix/bin/$TARGET-strip" "$f" 2>/dev/null || true
    done
    cp "$dest/usr/bin/gcc" "$dest/usr/bin/cc"      # a copy: the installed FAT system has no links
    # The C++ runtime for programs compiled on fluxOS: the cross toolchain's
    # libstdc++ (built for fluxOS against the same sysroot), where the native g++
    # looks: headers in /usr/include/c++/13.2.0, the libraries in /usr/lib. Without
    # debug information they are 5 MB instead of 23.
    mkdir -p "$dest/usr/include/c++" "$dest/usr/lib"
    cp -r "$prefix/$TARGET/include/c++/13.2.0" "$dest/usr/include/c++/"
    for lib in libstdc++.a libsupc++.a; do
        "$prefix/bin/$TARGET-strip" --strip-debug -o "$dest/usr/lib/$lib" "$prefix/$TARGET/lib/$lib"
    done
    echo "native gcc: $dest/usr/bin/gcc, cc, cpp, c++, g++, with libstdc++ ($(du -sh "$dest" | cut -f1))"
}

if [ -n "$native" ]; then
    build_native
    exit 0
fi
case "$only" in
"") build_binutils; build_gcc ;;
binutils) build_binutils ;;
gcc) build_gcc ;;
*) echo "$0: --only takes binutils or gcc" >&2; exit 2 ;;
esac
echo "toolchain in $prefix (sysroot $sysroot)"
