/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_private.h - ffmpegaudio.codec's own structures.
 *
 *   codec_lib.c       the library: its name, its function table, the calls' entry points
 *   codec_table.c     the codec rows, and FFmpeg's decoder for an ACC_*
 *   codec_session.c   a session: FFmpeg's decoder, then its converter to the samples asked for
 *
 * Everything in codec_table.c and codec_session.c runs inside FFmpeg's lock
 * (amiga_library.h); the entry points in codec_lib.c take it.
 */
#ifndef CODEC_PRIVATE_H
#define CODEC_PRIVATE_H

#include <exec/types.h>
#include <libraries/audiocodec_backend.h>

#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>

/* What the converter was last set up for: remade when a frame differs */
struct converter_input
{
	enum AVSampleFormat format; /* the decoder's samples */
	int rate;                   /* sample frames a second */
	AVChannelLayout layout;     /* the decoder's channels */
};

/* An open decoder */
struct session
{
	AVCodecContext *decoder;       /* FFmpeg's decoder */
	AVPacket *packet;              /* the packet being sent */
	AVFrame *frame;                /* the frame being received */
	SwrContext *converter;         /* to interleaved float samples of the channels asked for */
	struct converter_input input;  /* what the converter takes */
	AVChannelLayout output_layout; /* what it gives */
	BOOL converter_mixes;          /* it mixes the decoder's channels down */
	ULONG output_format;           /* ACF_* */
	ULONG output_channels;         /* mix down to this many; 0: as the stream */
	ULONG stream_channels;         /* the stream's, as the container says; 0 when it does not */
	float *mixed;                  /* the converter's samples of the last frame */
	unsigned int mixed_room;       /* bytes there */
	UBYTE *samples;                /* those samples in the format asked for */
	unsigned int samples_room;     /* bytes there */
	int64_t next_time;             /* the time after the last frame's samples, AV_NOPTS_VALUE when unknown */
};

ULONG table_rows(void);
LONG table_row(ULONG index, struct ACCodecInfo *info);
const AVCodec *table_decoder(const struct ACSessionParams *params);

struct session *session_open(const struct ACSessionParams *params, LONG *error);
void session_close(struct session *session);
LONG session_send(struct session *session, const struct ACPacket *packet);
LONG session_receive(struct session *session, struct ACFrame *frame);
LONG session_flush(struct session *session);

#endif /* CODEC_PRIVATE_H */
