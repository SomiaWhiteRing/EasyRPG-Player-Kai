#!/usr/bin/env bash
# Frozen decode-only FFmpeg subset, compiled with the active NDK.
set -euo pipefail
output="$1"; archive="$2"; compiler="$3"; target="$4"; sysroot="$5"; arch="$6"; ar="$7"; ranlib="$8"
echo "de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f  $archive" | sha256sum --check
mkdir -p "$output"
source_dir="$output/ffmpeg-7.1.5"
if [ ! -f "$source_dir/configure" ]; then tar -xf "$archive" -C "$output"; fi
signature="$( { sha256sum "$0" "$archive"; printf '%s\n' "$compiler" "$target" "$sysroot" "$arch"; "$compiler" --version; } | sha256sum)"
cd "$source_dir"
if [ ! -f .player-config ] || [ "$(cat .player-config)" != "$signature" ]; then
  [ ! -f ffbuild/config.mak ] || make distclean
  ./configure --prefix="$output/install" --cc="$compiler --target=$target --sysroot=$sysroot" \
    --ar="$ar" --ranlib="$ranlib" --target-os=android --arch="$arch" --enable-cross-compile \
    --disable-everything --disable-autodetect --disable-programs --disable-doc --disable-debug \
    --disable-network --disable-pthreads --disable-w32threads --disable-os2threads \
    --disable-asm --disable-x86asm --disable-avdevice --disable-avfilter --disable-postproc \
    --enable-small --enable-swscale --enable-swresample \
    --enable-demuxer=avi,mpegps,mpegts,mov \
    --enable-parser=mpegvideo,mpeg4video,h263,h264,mpegaudio,aac,ac3 \
    --enable-decoder=mpeg1video,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,msvideo1,cinepak,indeo3,mjpeg,h264,rawvideo \
    --enable-decoder=mp1float,mp2float,mp3float,aac,ac3,pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,adpcm_ms,adpcm_ima_wav \
    --extra-cflags='-Os -fPIC'
  printf '%s' "$signature" > .player-config
fi
make -j "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
make install
