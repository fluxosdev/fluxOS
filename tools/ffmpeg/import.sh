#!/bin/sh
# Bring FFmpeg into third_party/ffmpeg: the part of it fluxOS builds, with
# what FFmpeg's configure made for fluxOS, so that the system's own Makefile
# compiles it as it compiles FreeType and Mbed TLS (no configure run on the
# machine that builds fluxOS, which may be fluxOS itself).
#
#   tools/ffmpeg/import.sh [--toolchain DIR] [--sysroot DIR] [--jobs N] [--cache DIR] [--work DIR]
#
# The release is fetched into the toolchain's download cache and verified;
# configure runs with the x86_64-fluxos cross compiler ("make toolchain")
# against the sysroot ("make sysroot"), choosing the components below, and
# the libraries, ffmpeg and ffprobe are built once in the work directory.
# What that build compiled and included (its .d files) is copied into
# third_party/ffmpeg, the files configure generated into
# third_party/ffmpeg/gen, and the lists of sources into
# third_party/ffmpeg/flux.mk for the Makefile. Change the components here
# and run it again to change what fluxOS's FFmpeg can do.
set -e
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)
TARGET=x86_64-fluxos
FFMPEG=ffmpeg-9.0.2
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e

# What the Media Player plays: the video and audio of the files people have,
# old (AVI, MPEG, Flash, Windows Media, QuickTime) and new (MP4, Matroska, WebM).
DECODERS="h264 hevc vp8 vp9 mpeg1video mpeg2video mpeg4 msmpeg4v1 msmpeg4v2 msmpeg4v3 h263 h263p flv
  theora vp6 vp6a vp6f wmv1 wmv2 wmv3 vc1 svq3 mjpeg png gif rawvideo prores dvvideo cinepak
  aac aac_latm mp1float mp2float mp3float vorbis opus flac alac ac3 eac3 dca wmav1 wmav2 wmapro
  amrnb amrwb nellymoser adpcm_ima_wav adpcm_ima_qt adpcm_ms
  pcm_s8 pcm_u8 pcm_s16le pcm_s16be pcm_s24le pcm_s24be pcm_s32le pcm_s32be pcm_f32le pcm_f32be pcm_f64le
  pcm_alaw pcm_mulaw pcm_dvd pcm_bluray"
DEMUXERS="mov matroska avi mpegps mpegts mpegvideo ogg flac wav mp3 aac ac3 eac3 dts flv asf h264 hevc
  m4v ivf mjpeg w64 aiff au caf amr dv gif"
PARSERS="h264 hevc vp8 vp9 mpegvideo mpeg4video mpegaudio aac aac_latm ac3 flac vorbis opus mjpeg vc1
  h263 dca png gif"
# What the ffmpeg program can make: the encoders FFmpeg has of its own (no x264 or LAME), and the
# containers and filters a conversion needs.
ENCODERS="pcm_s16le pcm_s24le pcm_f32le pcm_u8 flac alac aac mp2 ac3 mpeg4 mpeg1video mpeg2video
  mjpeg png gif rawvideo ffv1"
MUXERS="pcm_s16le pcm_f32le wav mp4 mov ipod matroska webm avi mpegts mpeg1system ogg oga flac mp3 mp2 adts ac3 null image2
  gif md5 framemd5 framecrc rawvideo h264 hevc m4v"
FILTERS="scale aresample format aformat null anull fps setpts asetpts volume pad crop split asplit
  concat transpose hflip vflip trim atrim showinfo ashowinfo copy acopy"
BSFS="aac_adtstoasc h264_mp4toannexb hevc_mp4toannexb vp9_superframe vp9_superframe_split
  extract_extradata null"
PROTOCOLS="file pipe"

toolchain="$top/build/toolchain/cross"
sysroot="$top/build/sysroot"
cache="$top/build/toolchain/dl"
work="$top/build/ffmpeg"
dest="$top/third_party/ffmpeg"
jobs=$(nproc 2>/dev/null || echo 2)
while [ $# -gt 0 ]; do
    case "$1" in
    --toolchain) toolchain=$2; shift 2 ;;
    --sysroot) sysroot=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    --cache) cache=$2; shift 2 ;;
    --work) work=$2; shift 2 ;;
    *) echo "usage: $0 [--toolchain DIR] [--sysroot DIR] [--jobs N] [--cache DIR] [--work DIR]" >&2; exit 2 ;;
    esac
done
mkdir -p "$cache" "$work"
toolchain=$(cd "$toolchain" 2>/dev/null && pwd) || { echo "$0: no toolchain directory: run 'make toolchain' first" >&2; exit 1; }
cache=$(cd "$cache" && pwd)
work=$(cd "$work" && pwd)
[ -x "$toolchain/bin/$TARGET-gcc" ] || { echo "$0: no $toolchain/bin/$TARGET-gcc: run 'make toolchain' first" >&2; exit 1; }
[ -f "$sysroot/usr/lib/libc.a" ] || { echo "$0: $sysroot has no libc.a: run 'make sysroot' first" >&2; exit 1; }
export PATH="$toolchain/bin:$PATH"

f="$cache/$FFMPEG.tar.xz"
if [ ! -f "$f" ]; then
    echo "fetching $FFMPEG"
    curl -fSL -o "$f.part" "https://ffmpeg.org/releases/$FFMPEG.tar.xz"
    mv "$f.part" "$f"
fi
echo "$FFMPEG_SHA256  $f" | sha256sum -c - >/dev/null || { echo "$f: wrong checksum" >&2; exit 1; }

echo "unpacking $FFMPEG"
rm -rf "${work:?}/$FFMPEG" "$work/cfg"
tar -C "$work" -xf "$f"
mkdir -p "$work/cfg"
cd "$work/cfg"

# configure's lists are comma-separated
list() { echo $1 | tr ' ' ','; }
echo "configuring"
"../$FFMPEG/configure" --enable-cross-compile --cross-prefix=$TARGET- --arch=x86_64 --cpu=x86-64 \
    --target-os=none --prefix=/usr --disable-everything --disable-autodetect --disable-doc --disable-debug \
    --disable-avdevice --disable-network --disable-x86asm --disable-ffplay --enable-ffmpeg --enable-ffprobe \
    --enable-pthreads --enable-static --disable-shared --enable-swscale --enable-swresample --enable-avfilter \
    --enable-decoder="$(list "$DECODERS")" --enable-demuxer="$(list "$DEMUXERS")" \
    --enable-parser="$(list "$PARSERS")" --enable-encoder="$(list "$ENCODERS")" \
    --enable-muxer="$(list "$MUXERS")" --enable-filter="$(list "$FILTERS")" \
    --enable-bsf="$(list "$BSFS")" --enable-protocol="$(list "$PROTOCOLS")" \
    >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
echo "building"
make -j"$jobs" >build.log 2>&1 || { grep -E "error|undefined" build.log | head -40; exit 1; }

# Which objects each program adds to the libraries (fftools/Makefile's lists).
printf 'print-%%:\n\t@echo $($*)\n' >print.mk
ffmpeg_objs=$(make -s -f Makefile -f print.mk print-OBJS-ffmpeg)
ffprobe_objs=$(make -s -f Makefile -f print.mk print-OBJS-ffprobe)

# One configuration for both kinds of program fluxOS builds: what follows
# the processor's word size in config.h is made to follow the compiler's
# (a 32-bit program's FFmpeg, should one be built again, takes the same
# sources and the other half of these).
sed -i -e 's/^#define ARCH_X86_32 [01]$/#ifdef __x86_64__\n#define ARCH_X86_32 0\n#else\n#define ARCH_X86_32 1\n#endif/' \
    -e 's/^#define ARCH_X86_64 [01]$/#ifdef __x86_64__\n#define ARCH_X86_64 1\n#else\n#define ARCH_X86_64 0\n#endif/' \
    -e 's/^#define HAVE_FAST_64BIT [01]$/#ifdef __x86_64__\n#define HAVE_FAST_64BIT 1\n#else\n#define HAVE_FAST_64BIT 0\n#endif/' config.h

echo "copying into $dest"
rm -rf "$dest.new"
mkdir -p "$dest.new/gen"
# Every object's sources and headers: src/... is FFmpeg's tree (the build's link to it, or its own
# path for what configure's flags name, like compat/stdbit), a relative path is configure's
# (config.h, the component lists), another absolute one the sysroot's or GCC's.
for d in $(find . -name '*.d' ! -path './ffbuild/*'); do
    sed -e 's/^[^:]*://' -e 's/\\$//' -e "s|$work/$FFMPEG/|src/|g" "$d" | tr ' ' '\n'
done | sort -u | while read -r p; do
    case "$p" in
    "") ;;
    /*) ;;
    src/*) mkdir -p "$dest.new/$(dirname "${p#src/}")"; cp "$p" "$dest.new/${p#src/}" ;;
    *) mkdir -p "$dest.new/gen/$(dirname "$p")"; cp "$p" "$dest.new/gen/$p" ;;
    esac
done
cp "../$FFMPEG/COPYING.LGPLv2.1" "../$FFMPEG/LICENSE.md" "../$FFMPEG/VERSION" "$dest.new/"
# Objects compiled without a .d are bin2c's: data files (ffmpeg's graph page) made C in the build.
for o in $(find . -name '*.o' ! -path './ffbuild/*'); do
    [ -f "${o%.o}.d" ] || { mkdir -p "$dest.new/gen/$(dirname "$o")"; cp "${o%.o}.c" "$dest.new/gen/${o%.o}.c"; }
done

# An object's source, as the Makefile names it.
source_of() {
    [ -f "${1%.o}.d" ] || { echo "third_party/ffmpeg/gen/${1%.o}.c"; return; }
    s=$(tr '\\\n' '  ' <"${1%.o}.d" | sed 's/^[^:]*: *//;s/ .*//')
    case "$s" in
    src/*) echo "third_party/ffmpeg/${s#src/}" ;;
    *) echo "third_party/ffmpeg/gen/$s" ;;
    esac
}
sources() {
    for o in "$@"; do source_of "$o"; done | sort | sed 's/^/\t/;s/$/ \\/'
    echo
}
{
    echo "# Generated by tools/ffmpeg/import.sh from $FFMPEG: what fluxOS builds of it. Do not edit."
    echo "FFMPEG_VERSION := ${FFMPEG#ffmpeg-}"
    echo "FFMPEG_LIB_SRCS := \\"
    sources $(find libavutil libavcodec libavformat libavfilter libswscale libswresample -name '*.o' | sort)
    echo "FFMPEG_FFMPEG_SRCS := \\"
    sources $ffmpeg_objs
    echo "FFMPEG_FFPROBE_SRCS := \\"
    sources $ffprobe_objs
} >"$dest.new/flux.mk"
[ ! -f "$dest/README.flux" ] || cp "$dest/README.flux" "$dest.new/"
rm -rf "$dest"
mv "$dest.new" "$dest"
echo "$dest: $(grep -c '\.c \\$' "$dest/flux.mk") sources, $(du -sh "$dest" | cut -f1)"
