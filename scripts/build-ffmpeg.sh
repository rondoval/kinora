#!/bin/sh
# Build FFmpeg for m68k-amigaos as static libraries. CMake calls it inside the container.
#
#   build-ffmpeg.sh source <ffmpeg source> <patches dir> <build dir>
#       copy the submodule into <build dir>/source and patch the copy; the submodule
#       itself stays as upstream tagged it
#   build-ffmpeg.sh <demuxer|audio> <shim header> <build dir> <target flags>
#       configure and build one configuration from that copy
#
# FFmpeg's configure insists on a host compiler. With these options the build never
# runs one, so the cross compiler is named for it.
set -eu

CONFIGURATION=$1

if [ "$CONFIGURATION" = source ]; then
	SOURCE=$2
	PATCHES=$3
	BUILD=$4
	COPY="$BUILD/source"
	rm -rf "$COPY"
	mkdir -p "$COPY"
	(cd "$SOURCE" && tar cf - --exclude=.git .) | (cd "$COPY" && tar xf -)
	for patch in "$PATCHES"/*.patch; do
		(cd "$COPY" && GIT_CEILING_DIRECTORIES="$BUILD" git apply "$patch")
	done
	touch "$COPY/.kinora-stamp"
	exit 0
fi

SHIM=$2
BUILD=$3
TARGET_FLAGS=$4
COPY="$BUILD/source"

case "$CONFIGURATION" in
demuxer)
	PARTS="--disable-swresample
		--enable-demuxer=matroska,mov,mpegts
		--enable-parser=h264,hevc,aac,aac_latm,ac3,mpegaudio,dca,opus,vorbis,flac
		--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc"
	;;
audio)
	PARTS="--disable-avformat --enable-swresample
		--enable-decoder=aac,aac_latm,ac3,eac3,dca,mp3,mp2,opus,vorbis,flac,truehd
		--enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_u8,pcm_f32le,pcm_f32be"
	;;
*)
	echo "unknown configuration: $CONFIGURATION" >&2
	exit 2
	;;
esac

OBJECTS="$BUILD/$CONFIGURATION"
mkdir -p "$OBJECTS"
cd "$OBJECTS"

# shellcheck disable=SC2086
"$COPY/configure" \
	--enable-cross-compile --cross-prefix=m68k-amigaos- --arch=m68k --target-os=none \
	--host-cc=m68k-amigaos-gcc --host-ld=m68k-amigaos-gcc \
	--host-cflags="$TARGET_FLAGS" --host-ldflags="$TARGET_FLAGS" \
	--disable-everything --disable-programs --disable-doc --disable-network \
	--disable-pthreads --disable-w32threads --disable-os2threads --disable-autodetect \
	--disable-asm --disable-inline-asm --disable-debug --disable-avdevice --disable-avfilter --disable-swscale \
	--disable-iconv --enable-static --disable-shared --enable-small --pkg-config=false \
	$PARTS \
	--extra-cflags="$TARGET_FLAGS -Wno-error=incompatible-pointer-types -include $SHIM" \
	--extra-ldflags="$TARGET_FLAGS" \
	> configure.log 2>&1 || { tail -40 ffbuild/config.log >&2; exit 1; }

grep -q '^License: LGPL version 2.1 or later$' configure.log || {
	echo "FFmpeg is not configured as LGPL 2.1 or later:" >&2
	grep '^License' configure.log >&2
	exit 1
}

make -j"$(nproc)" > make.log 2>&1 || { grep -E ' error|Error ' make.log | head -40 >&2; exit 1; }
