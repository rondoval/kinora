#!/bin/sh
# Build FFmpeg for m68k-amigaos as static libraries. CMake calls it inside the container.
#
#   build-ffmpeg.sh source <ffmpeg source> <patches dir> <build dir>
#       copy the submodule into <build dir>/source and patch the copy; the submodule
#       itself stays as upstream tagged it
#   build-ffmpeg.sh <demuxer|audio|video> <glue dir> <build dir> <target flags> <pgo> <profiles dir>
#       configure and build one configuration from that copy, into <build dir>/<configuration>
#
# <glue dir> holds the headers forced in ahead of every FFmpeg file (amiga_shim.h,
# branchless_clip.h). <pgo> is the profile-guided build's mode (docs/architecture.md, section 3):
#   OFF       no profile
#   GENERATE  instrumented (-fprofile-arcs), for a training round; its stamp goes to
#             <configuration>/kinora-profile-stamp, for the profiles made from it
#   USE       laid out by <profiles dir>/<configuration>/ (.gcda files and STAMP); built as
#             OFF when there are none, refused when STAMP is not this build's
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
	# what the profiles were made from (USE checks it): the release, the patches
	{
		echo "ffmpeg $(cat "$COPY/RELEASE")"
		echo "patches $(cat "$PATCHES"/*.patch | sha256sum | cut -d' ' -f1)"
	} > "$COPY/.kinora-source"
	touch "$COPY/.kinora-stamp"
	exit 0
fi

GLUE=$2
BUILD=$3
TARGET_FLAGS=$4
PGO=$5
PROFILES="$6/$CONFIGURATION"
COPY="$BUILD/source"

case "$CONFIGURATION" in
demuxer)
	PARTS="--disable-swresample
		--enable-demuxer=matroska,mov,mpegts,avi,mpegps,mpegvideo,m4v
		--enable-parser=h264,hevc,aac,aac_latm,ac3,mpegaudio,dca,opus,vorbis,flac,mpegvideo,mpeg4video
		--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,mpeg4_unpack_bframes"
	;;
audio)
	PARTS="--disable-avformat --enable-swresample
		--enable-decoder=aac,aac_latm,ac3_fixed,eac3,dca,mp3,mp2,opus,vorbis,flac,truehd
		--enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_u8,pcm_f32le,pcm_f32be
		--enable-decoder=pcm_dvd,pcm_alaw,pcm_mulaw,adpcm_ima_wav,adpcm_ms"
	;;
video)
	PARTS="--disable-avformat --disable-swresample
		--enable-decoder=mpeg1video,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,h263,h263i,flv,theora
		--enable-parser=mpegvideo,mpeg4video"
	;;
*)
	echo "unknown configuration: $CONFIGURATION" >&2
	exit 2
	;;
esac

# For Emu68 (docs/architecture.md, section 3): -O3, misaligned reads, the branchless clamps.
# gcc's m68k backend assumes strict alignment unless told; configure sets neither
# HAVE_FAST_UNALIGNED nor HAVE_FAST_CLZ for m68k, so config.h gets them below.
CFLAGS="$TARGET_FLAGS -mno-strict-align -Wno-error=incompatible-pointer-types -include $GLUE/amiga_shim.h -include $GLUE/branchless_clip.h"
SOURCE_STAMP=$(cat "$COPY/.kinora-source")
# shellcheck disable=SC2086
STAMP=$(printf '%s\nflags %s\ncompiler %s\n' "$SOURCE_STAMP" \
	"$(echo $CFLAGS $PARTS | sha256sum | cut -d' ' -f1)" "$(m68k-amigaos-gcc --version | head -1)")

PGO_CFLAGS=
PGO_LDFLAGS=
case "$PGO" in
OFF) ;;
GENERATE)
	PGO_CFLAGS="-fprofile-arcs -fprofile-update=single"
	PGO_LDFLAGS="-fprofile-arcs"
	;;
USE)
	if [ ! -f "$PROFILES/STAMP" ]; then
		echo "FFmpeg ($CONFIGURATION): no profiles in $PROFILES, built without" >&2
		PGO=OFF
	elif [ "$(cat "$PROFILES/STAMP")" != "$STAMP" ]; then
		echo "FFmpeg ($CONFIGURATION): the profiles in $PROFILES were made from another FFmpeg, patch set," >&2
		echo "flags or compiler. Train again (scripts/pgo-train.py) or build with -DKINORA_PGO=OFF." >&2
		printf 'theirs:\n%s\nthis build:\n%s\n' "$(cat "$PROFILES/STAMP")" "$STAMP" >&2
		exit 1
	else
		# gcc notes code the training did not reach; that is no fault
		PGO_CFLAGS="-fbranch-probabilities -Wno-missing-profile"
	fi
	;;
*)
	echo "unknown profile mode: $PGO" >&2
	exit 2
	;;
esac

OBJECTS="$BUILD/$CONFIGURATION"
# A change of flags, mode or profiles builds everything again: make would not see it.
BUILT=$(printf '%s\npgo %s\n' "$STAMP" "$PGO"; [ "$PGO" != USE ] || find "$PROFILES" -type f | sort | xargs sha256sum)
if [ -f "$OBJECTS/kinora-build" ] && [ "$(cat "$OBJECTS/kinora-build")" != "$BUILT" ]; then
	rm -rf "$OBJECTS"
fi
mkdir -p "$OBJECTS"
cd "$OBJECTS"
if [ "$PGO" = USE ]; then
	# a .gcda file is named after its object: the same tree as the objects'
	(cd "$PROFILES" && find . -name '*.gcda') | while read -r gcda; do
		mkdir -p "$(dirname "$gcda")"
		cp -p "$PROFILES/$gcda" "$gcda"
	done
fi

# shellcheck disable=SC2086
"$COPY/configure" \
	--enable-cross-compile --cross-prefix=m68k-amigaos- --arch=m68k --target-os=none \
	--host-cc=m68k-amigaos-gcc --host-ld=m68k-amigaos-gcc \
	--host-cflags="$TARGET_FLAGS" --host-ldflags="$TARGET_FLAGS" \
	--disable-everything --disable-programs --disable-doc --disable-network \
	--disable-pthreads --disable-w32threads --disable-os2threads --disable-autodetect \
	--disable-asm --disable-inline-asm --disable-debug --disable-avdevice --disable-avfilter --disable-swscale \
	--disable-iconv --enable-static --disable-shared --pkg-config=false \
	$PARTS \
	--extra-cflags="$CFLAGS $PGO_CFLAGS" \
	--extra-ldflags="$TARGET_FLAGS $PGO_LDFLAGS" \
	> configure.log 2>&1 || { tail -40 ffbuild/config.log >&2; exit 1; }

grep -q '^License: LGPL version 2.1 or later$' configure.log || {
	echo "FFmpeg is not configured as LGPL 2.1 or later:" >&2
	grep '^License' configure.log >&2
	exit 1
}

# configure writes config.h only when it changed; so does this, or make would build it all
sed -e 's/^#define HAVE_FAST_UNALIGNED 0$/#define HAVE_FAST_UNALIGNED 1/' \
	-e 's/^#define HAVE_FAST_CLZ 0$/#define HAVE_FAST_CLZ 1/' config.h > config.h.kinora-new
grep -q '^#define HAVE_FAST_UNALIGNED 1$' config.h.kinora-new && grep -q '^#define HAVE_FAST_CLZ 1$' config.h.kinora-new || {
	echo "config.h has no HAVE_FAST_UNALIGNED or HAVE_FAST_CLZ to set" >&2
	exit 1
}
cmp -s config.h.kinora-new config.h.kinora || cp config.h.kinora-new config.h.kinora
cp -p config.h.kinora config.h
rm config.h.kinora-new

# configure drops the arch with --disable-asm (ARCH=c), which libnix needs: it has no aligned
# allocations. ARCH=m68k takes libavcodec/m68k/Makefile in, Kinora's IDCTs (patch 0003).
make -j"$(nproc)" ARCH=m68k > make.log 2>&1 || { grep -E ' error|Error ' make.log | head -40 >&2; exit 1; }
if [ "$CONFIGURATION" = video ] && [ ! -f libavcodec/m68k/simple_idct.o ]; then
	echo "the video configuration has no libavcodec/m68k" >&2
	exit 1
fi
if grep -E 'warning:' make.log | grep -E 'libavcodec/m68k/' >&2; then
	echo "warnings in Kinora's own FFmpeg code (libavcodec/m68k)" >&2
	exit 1
fi
if [ "$PGO" = GENERATE ]; then
	printf '%s\n' "$STAMP" > kinora-profile-stamp
fi
printf '%s\n' "$BUILT" > kinora-build
