#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
# The profile-guided build's training material (docs/architecture.md, section 3): every codec
# and container Kinora's FFmpeg offers, made from parts of Tears of Steel (CC-BY 3.0, Blender
# Foundation) that the tests never use: 20 s of live action from 2:40 and 20 s of battle from
# 8:40, retimed to 25 pictures a second, and 30 s of its 5.1 mix from 2:40.
#
#   tests/material/training.sh      everything into build/training-material/
#
# Needs the film and its 5.1 mix (mango.blender.org) in build/training-material/source/ as
# tears_of_steel_720p.mov and tos_51.ac3, and the host's ffmpeg with libx264, libx265,
# libxvid, libtheora, libmp3lame, libopus and libvorbis. What is there already is left alone.
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
OUT="$ROOT/build/training-material"
MOVIE="$OUT/source/tears_of_steel_720p.mov"
MIX="$OUT/source/tos_51.ac3"
FF="ffmpeg -nostdin -loglevel error -y"
for source in "$MOVIE" "$MIX"; do
	[ -f "$source" ] || { echo "missing $source: see the head of this script" >&2; exit 1; }
done
mkdir -p "$OUT"
cd "$OUT"

[ -f source.mkv ] || $FF -ss 160 -t 20 -i "$MOVIE" -ss 520 -t 20 -i "$MOVIE" \
	-filter_complex "[0:v][1:v]concat=n=2:v=1:a=0,setpts=N/25/TB[v]" -map "[v]" -r 25 -c:v ffv1 -level 3 source.mkv
[ -f sound.wav ] || $FF -ss 160 -t 30 -i "$MIX" -c:a pcm_s16le sound.wav

SD="crop=961:534,scale=720:400"           # a 1.8:1 rip, as Xvid films were cut
PAL="crop=950:534,scale=720:576,setsar=64/45,framerate=50,tinterlace=interleave_top,setfield=tff"
HD="crop=950:534,scale=1920:1080,setsar=1,framerate=50,tinterlace=interleave_top,setfield=tff"

film() # <name> <filter> <options...>: the picture, and the sound when the options name it
{
	local name=$1 filter=$2
	shift 2
	[ -s "$name" ] && return
	$FF -i source.mkv -i sound.wav -map 0:v -vf "$filter" "$@" "$name"
}

# the video decoders (ffmpegvideo.codec), each in a container it comes in
XVID="-c:v libxvid -b:v 1200k -bf 2 -g 250 -me_quality 6 -mbd rd -trellis 1"
film xvid.mkv "$SD" $XVID -flags +mv4+aic -map 1:a -c:a libmp3lame -b:a 192k
film xvid_qpel.mkv "$SD" $XVID -flags +mv4+aic+qpel -gmc 1 -an
film xvid_hd.mkv "null" -c:v libxvid -b:v 3500k -bf 2 -g 250 -flags +mv4+aic -an
film mpeg4.mp4 "$SD" -c:v mpeg4 -b:v 1500k -bf 2 -g 250 -flags +mv4 -map 1:a -c:a aac -ac 2 -b:a 160k
film divx3.mkv "$SD" -c:v msmpeg4 -b:v 1200k -g 250 -vtag DIV3 -an
film msmpeg4v2.mkv "$SD" -t 10 -c:v msmpeg4v2 -b:v 1200k -g 250 -an
film mpeg2_576i.ts "$PAL" -c:v mpeg2video -b:v 6M -maxrate 9M -bufsize 1835k -g 12 -bf 2 \
	-flags +ilme+ildct -top 1 -alternate_scan 1 -aspect 16:9 -map 1:a -c:a mp2 -ac 2 -b:a 224k
film mpeg2_1080i.ts "$HD" -t 10 -c:v mpeg2video -b:v 15M -maxrate 20M -bufsize 7M -g 12 -bf 2 \
	-flags +ilme+ildct -top 1 -alternate_scan 1 -map 1:a -c:a ac3 -b:a 448k
film mpeg1.mkv "$SD" -c:v mpeg1video -b:v 2M -bf 2 -g 15 -an
film h263.mkv "crop=950:534,scale=352:288" -t 20 -c:v h263 -b:v 800k -g 250 -an
film flv1.mkv "$SD" -t 20 -c:v flv -b:v 1200k -g 250 -an
film theora.mkv "$SD" -c:v libtheora -b:v 1200k -g 250 -map 1:a -c:a libvorbis -ac 2

# the demuxer's containers as films come: H.264 and H.265 with sound and subtitles
printf '1\n00:00:01,000 --> 00:00:05,000\nTraining\n\n2\n00:00:10,000 --> 00:00:15,000\nMore training\n' > training.srt
[ -f h264.mkv ] || $FF -i source.mkv -i sound.wav -i training.srt -map 0:v -map 1:a -map 1:a -map 2 \
	-vf "crop=950:534,scale=1280:720" -c:v libx264 -b:v 3M -c:a:0 aac -b:a:0 384k -c:a:1 ac3 -ac:a:1 2 -c:s srt h264.mkv
[ -f h265.mp4 ] || $FF -i source.mkv -i sound.wav -map 0:v -map 1:a -vf "crop=950:534,scale=1280:720" \
	-c:v libx265 -x265-params log-level=error -tag:v hvc1 -b:v 2M -c:a eac3 -b:a 384k h265.mp4
[ -f h264.ts ] || $FF -i source.mkv -i sound.wav -map 0:v -map 1:a -map 1:a -vf "crop=950:534,scale=1280:720" \
	-c:v libx264 -b:v 3M -c:a:0 ac3 -b:a:0 448k -c:a:1 mp2 -ac:a:1 2 h264.ts

# the audio decoders (audiocodec's ffmpegaudio.codec): each codec Kinora plays, from the 5.1 mix.
# FFmpeg encodes no HE-AAC, so its SBR stays untrained (laid out as cold)
clip() # <name> <layout> <encoder options...>
{
	local name=$1 layout=$2
	shift 2
	[ -s "$name.mkv" ] || $FF -i sound.wav -af "aformat=channel_layouts=$layout" "$@" "$name.mkv"
}
clip aac_st stereo -c:a aac -b:a 160k
clip aac_51 5.1 -c:a aac -b:a 384k
clip ac3_51 5.1 -c:a ac3 -b:a 448k
clip eac3_51 5.1 -c:a eac3 -b:a 384k
clip dts_51 5.1 -c:a dca -strict -2 -b:a 1509k
clip mp3_st stereo -c:a libmp3lame -b:a 192k
clip mp2_st stereo -c:a mp2 -b:a 224k
clip opus_51 5.1 -c:a libopus -b:a 256k
clip vorbis_st stereo -c:a libvorbis
clip flac_51 5.1 -c:a flac
clip truehd_51 5.1 -c:a truehd -strict -2
clip pcm_st stereo -c:a pcm_s16le

ls -la "$OUT" | grep -v source
