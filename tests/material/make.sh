#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# The suite's material: test films, and what the host makes of them to compare with.
#
#   tests/material/make.sh         after scripts/docker-build.sh (it needs the patched FFmpeg source)
#
# The films come from the host's ffmpeg. The references come from host builds of the very
# FFmpeg source the Amiga libraries are built from, in the same configurations: ffprobe's
# packet counts, each audio film's packets (pkdump) decoded by the same decoders (adec-host), so
# that the Amiga's samples can be asked to match to the last bit but one, and each video
# film's packets decoded by the same decoders (vdec-host), an MD5 a picture, which the
# Amiga's must equal.
#
# Needs gcc, make and ffmpeg (with libx264, libx265, libmp3lame, libopus, libvorbis, libxvid,
# libtheora, libvpx) on the host. Everything lands in build/test-material/; what is there is
# left alone while the configurations below and the clips' list stay as they are, so delete
# the directory to make it all again.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
HERE="$ROOT/tests/material"
SOURCE="$ROOT/build/ffmpeg/source"
OUT="$ROOT/build/test-material"
HOST="$ROOT/build/host"
[ -f "$SOURCE/configure" ] || { echo "build first: scripts/docker-build.sh" >&2; exit 1; }

COMMON="--disable-everything --disable-programs --disable-doc --disable-network --disable-pthreads
	--disable-autodetect --disable-asm --disable-inline-asm --disable-debug --disable-avdevice
	--disable-avfilter --disable-swscale --disable-iconv --enable-static --pkg-config=false"
# scripts/build-ffmpeg.sh's configurations, and the file protocol the host's demuxer reads by
DEMUXER="--disable-swresample --enable-demuxer=matroska,mov,mpegts --enable-protocol=file
	--enable-parser=h264,hevc,aac,aac_latm,ac3,mpegaudio,dca,opus,vorbis,flac,mpegvideo,mpeg4video
	--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,mpeg4_unpack_bframes"
AUDIO="--disable-avformat --enable-swresample
	--enable-decoder=aac,aac_latm,ac3_fixed,eac3,dca,mp3,mp2,opus,vorbis,flac,truehd
	--enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_u8,pcm_f32le,pcm_f32be"
VIDEO="--disable-avformat --disable-swresample
	--enable-decoder=mpeg1video,mpeg2video,mpeg4,msmpeg4v1,msmpeg4v2,msmpeg4v3,h263,h263i,flv,theora
	--enable-parser=mpegvideo,mpeg4video"
CLIPS="video clips 2" # the clips' list below: a new one makes everything again

# The material made from another list or configurations is made again
STAMP=$(printf '%s\n' "$DEMUXER" "$AUDIO" "$VIDEO" "$CLIPS" "$(cat "$SOURCE/.kinora-source")" | sha256sum | cut -d' ' -f1)
if [ -f "$OUT/.complete" ] && [ "$(cat "$OUT/.complete")" = "$STAMP" ]; then
	echo "material in $OUT (complete)"
	exit 0
fi
rm -rf "$OUT"
mkdir -p "$OUT" "$HOST"

host_ffmpeg() # <name> <options>
{
	local wanted
	wanted=$(printf '%s\n%s\n' "$2" "$(cat "$SOURCE/.kinora-source")")
	if [ -f "$HOST/$1/libavutil/libavutil.a" ] && [ "$(cat "$HOST/$1/kinora-config" 2>/dev/null)" = "$wanted" ]; then
		return
	fi
	rm -rf "${HOST:?}/$1"
	mkdir -p "$HOST/$1"
	# shellcheck disable=SC2086
	(cd "$HOST/$1" && "$SOURCE/configure" $COMMON $2 > configure.log && make -j"$(nproc)" > make.log 2>&1)
	printf '%s\n' "$wanted" > "$HOST/$1/kinora-config"
}
host_ffmpeg demuxer "$DEMUXER"
host_ffmpeg audio "$AUDIO"
host_ffmpeg video "$VIDEO"

gcc -O2 -I"$HOST/demuxer" -I"$SOURCE" "$HERE/pkdump.c" -o "$OUT/pkdump" \
	-L"$HOST/demuxer/libavformat" -L"$HOST/demuxer/libavcodec" -L"$HOST/demuxer/libavutil" -lavformat -lavcodec -lavutil -lm
gcc -O2 -I"$HOST/audio" -I"$SOURCE" "$HERE/adec.c" -o "$OUT/adec-host" \
	-L"$HOST/audio/libavcodec" -L"$HOST/audio/libswresample" -L"$HOST/audio/libavutil" -lavcodec -lswresample -lavutil -lm
gcc -O2 -I"$HOST/video" -I"$SOURCE" "$HERE/vdec.c" -o "$OUT/vdec-host" \
	-L"$HOST/video/libavcodec" -L"$HOST/video/libavutil" -lavcodec -lavutil -lm

cd "$OUT"
FF="ffmpeg -nostdin -loglevel error -y"
PROBE="ffprobe -v error"

# three containers, four seconds each: H.264 + AAC + SubRip, H.265 + AC-3 5.1, H.264 + E-AC-3 in a transport stream
printf '1\n00:00:01,000 --> 00:00:03,000\nHello Kinora\n' > hello.srt
PICTURE="-f lavfi -i testsrc2=s=640x360:r=25:d=4"
TONE="-f lavfi -i sine=f=440:d=4"
$FF $PICTURE $TONE -i hello.srt -c:v libx264 -c:a aac -c:s srt a.mkv
$FF $PICTURE $TONE -c:v libx265 -x265-params log-level=error -tag:v hvc1 -c:a ac3 -ac 6 b.mp4
$FF $PICTURE $TONE -c:v libx264 -c:a eac3 c.ts
for clip in a.mkv b.mp4 c.ts; do
	$PROBE -count_packets -show_entries stream=codec_name,nb_read_packets -of csv=p=0 "$clip" > "$clip.ffprobe"
done

# twenty seconds of pink noise and a tone per audio codec, with the host's decode of each
SOUND="-f lavfi -i anoisesrc=d=20:c=pink:r=48000:a=0.3 -f lavfi -i sine=f=440:d=20:r=48000"
audio_clip() # <name> <layout> <encoder options>
{
	local name=$1 layout=$2
	shift 2
	$FF $SOUND -filter_complex "[0][1]amix=inputs=2,aformat=channel_layouts=$layout" -t 20 "$@" "$name.mkv"
	./pkdump "$name.mkv" "$name.kpk"
	./adec-host "$name.kpk" "$name.host.raw" 1 > /dev/null
}
audio_clip aac_st stereo -c:a aac -b:a 160k
audio_clip aac_51 5.1 -c:a aac -b:a 384k
audio_clip ac3_51 5.1 -c:a ac3 -b:a 448k
audio_clip eac3_51 5.1 -c:a eac3 -b:a 384k
audio_clip dts_51 5.1 -c:a dca -strict -2 -b:a 1509k
audio_clip mp3_st stereo -c:a libmp3lame -b:a 192k
audio_clip opus_51 5.1 -c:a libopus -b:a 256k
audio_clip flac_st stereo -c:a flac
audio_clip vorbis_st stereo -c:a libvorbis

# the player's film: forty seconds of 720p with a frame counter, two audio tracks, subtitles, three chapters;
# and the same picture with one audio track and the subtitles in a file beside it
printf '1\n00:00:01,000 --> 00:00:04,000\nHello from <i>Kinora</i>\n\n2\n00:00:05,000 --> 00:00:09,000\nZwei Zeilen: Gr\xc3\xbc\xc3\x9fe, caf\xc3\xa9\nand a second line\n\n3\n00:00:10,000 --> 00:00:14,000\nA rather long line of subtitle text that should be wrapped because it does not fit the width of a small window at all\n\n4\n00:00:20,000 --> 00:00:30,000\nLate cue at twenty seconds\n' > plain.srt
# a cue no 8-bit character set holds: Polish, Russian and Greek in one line
printf '1\n00:00:01,000 --> 00:00:39,000\nZa\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 g\xc4\x99\xc5\x9bl\xc4\x85 \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 \xce\xa9\xce\xbc\xce\xad\xce\xb3\xce\xb1\n' > world.srt
# cues with the styles that are honoured: slanted, plain and heavy to compare, at the top
printf '1\n00:00:01,000 --> 00:00:08,000\n<i>Slanted words</i>\n\n2\n00:00:09,000 --> 00:00:16,000\nHeavy words\n\n3\n00:00:17,000 --> 00:00:24,000\n<b>Heavy words</b>\n\n4\n00:00:25,000 --> 00:00:32,000\n{\\an8}Words at the top\n' > styles.srt
printf ';FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=10000\ntitle=Opening\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=10000\nEND=25000\ntitle=Middle\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=25000\nEND=40000\ntitle=Closing\n' > chapters.txt
$FF -f lavfi -i testsrc2=size=1280x720:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 40 \
	-c:v libx264 -pix_fmt yuv420p -g 50 -b:v 2M -c:a aac -ac 2 plain.mkv
$FF -i plain.mkv -f lavfi -t 40 -i sine=frequency=880:sample_rate=48000 -i plain.srt -i chapters.txt \
	-map 0:v -map 0:a -map 1:a -map 2 -map_metadata 3 -map_chapters 3 -c:v copy -c:a:0 copy -c:a:1 ac3 -ac:a:1 2 -c:s srt \
	-metadata:s:a:0 language=eng -metadata:s:a:0 title=Low -metadata:s:a:1 language=pol -metadata:s:a:1 title=High \
	-metadata:s:s:0 language=eng film.mkv
# a short film for the playlist
$FF -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=660:sample_rate=48000 -t 6 \
	-c:v libx264 -pix_fmt yuv420p -g 25 -c:a aac -ac 2 short.mkv

# six seconds of each video codec the 68k decodes (ffmpegvideo.codec), a moving picture with
# B-pictures where the codec has them, in the containers they come in; the host's decode of
# each, an MD5 a picture. VP8 is there to be refused.
MOVING="-f lavfi -i testsrc2=s=720x400:r=25:d=6"
CIF="-f lavfi -i testsrc2=s=352x288:r=25:d=6"
PAL="-f lavfi -i testsrc2=s=720x576:r=50:d=6"
INTERLACE="tinterlace=interleave_top,setfield=tff"
video_clip() # <name> <input> <encoder options>
{
	local name=$1 input=$2
	shift 2
	# shellcheck disable=SC2086
	$FF $input -an "$@" "$name"
	./pkdump "$name" "$name.kpv" video > /dev/null
	./vdec-host "$name.kpv" "$name.host.md5" > "$name.vdec"
}
video_clip mpeg1.mkv "$MOVING" -c:v mpeg1video -b:v 2M -bf 2 -g 15
video_clip mpeg2.ts "$PAL" -vf "$INTERLACE" -r 25 -c:v mpeg2video -b:v 5M -bf 2 -g 12 -flags +ilme+ildct -top 1 -alternate_scan 1
video_clip mpeg4.mkv "$MOVING" -c:v mpeg4 -b:v 1500k -bf 2 -g 50 -flags +mv4+qpel
# Xvid without B-pictures: libxvid through ffmpeg stores their decode order as Matroska's times
video_clip xvid.mkv "$MOVING" -c:v libxvid -b:v 1200k -bf 0 -g 50
video_clip divx3.mkv "$MOVING" -c:v msmpeg4 -b:v 1200k -g 50 -vtag DIV3
video_clip msmpeg4v2.mkv "$MOVING" -c:v msmpeg4v2 -b:v 1200k -g 50 -vtag MP42
video_clip h263.mkv "$CIF" -c:v h263 -b:v 800k -g 50
video_clip flv1.mkv "$MOVING" -c:v flv -b:v 1200k -g 50
video_clip theora.mkv "$MOVING" -c:v libtheora -b:v 1200k -g 50
$FF $MOVING -an -c:v libvpx -b:v 1200k vp8.webm

printf '%s\n' "$STAMP" > .complete
echo "material in $OUT"
