FFmpeg 9.0.2, https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz (signature checked against the
FFmpeg release key). Used under the LGPL version 2.1 or later (COPYING.LGPLv2.1); no GPL or
nonfree component is enabled (no x264, no libfdk, no x86 assembly).
tools/ffmpeg/import.sh made this directory: it runs FFmpeg's configure with the i686-fluxos
cross compiler, builds once, and copies the sources and headers that build used, unmodified.
gen/ holds what configure generated for fluxOS (config.h, the lists of codecs, demuxers and
so on); flux.mk lists the sources the Makefile compiles. The components are chosen in
import.sh. The Media Player (user/desktop/apps/mediaplayer.c) links libffmpeg.a; ffmpeg and
ffprobe are in /usr/bin. "make test-ffmpeg-qemu" runs a selection of FFmpeg's own tests.
