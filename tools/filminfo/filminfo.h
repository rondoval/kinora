/* SPDX-License-Identifier: MPL-2.0 */
/*
 * filminfo.h - FilmInfo's parts and what they share.
 *
 *   filminfo.c        the arguments, the libraries, the codec tables, what a run does
 *   filminfo_file.c   the container: its streams, chapters and packets (demuxer.library)
 *   filminfo_audio.c  the sound: audiocodec.library's table, a stream decoded
 *   filminfo_video.c  the picture: the video decoders' rows, a stream decoded, each picture's digest
 *   md5.c             the digests
 */
#ifndef KINORA_FILMINFO_H
#define KINORA_FILMINFO_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <libraries/demuxer.h>

#define MICROSECONDS_PER_MS 1000 /* VCTime is microseconds; times are printed in milliseconds */

/* The template's arguments, in its order */
enum
{
	ARG_FILE,
	ARG_AUDIO,
	ARG_VIDEO,
	ARG_STREAM,
	ARG_PACKETS,
	ARG_ALL,
	ARG_SEEK,
	ARG_CHANNELS,
	ARG_BITS,
	ARG_TO,
	ARG_MD5,
	ARG_FORMAT,
	ARG_WIDTH,
	ARG_HEIGHT,
	ARG_SKIP,
	ARG_BACKEND,
	ARG_PRI,
	ARG_COUNT
};

extern struct Library *DemuxerBase;
extern struct Library *AudioCodecBase;
extern struct Library *VideoCodecBase;

/* filminfo.c */
long milliseconds(VCTime time);
ULONG number_of(const LONG *args, int index);
LONG stream_find(struct DMXSource *source, ULONG kind, const LONG *args, struct DMXStream *stream);

/* filminfo_file.c */
int file_show(struct DMXSource *source, const LONG *args);

/* filminfo_audio.c */
int audio_codecs_show(void);
int audio_decode(struct DMXSource *source, const LONG *args);

/* filminfo_video.c */
int video_codecs_show(void);
BOOL video_format_known(const LONG *args);
int video_decode(struct DMXSource *source, const LONG *args);

#endif /* KINORA_FILMINFO_H */
