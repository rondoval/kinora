/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * demuxer_private.h - demuxer.library's own structures.
 *
 *   demuxer_lib.c      the library: its name, its function table, the calls' entry points
 *   demuxer_source.c   a source: FFmpeg's demuxer over the reader, streams, packets, seeking
 *   demuxer_codecs.c   FFmpeg's codec numbers and colours as the public headers' own
 *   demuxer_order.c    the times MPEG video's pictures are shown at, where the file gives them in decode order
 *   demuxer_reader.c   the file: dos.library reads and positions past 2 GiB
 *
 * Everything in demuxer_source.c, demuxer_codecs.c and demuxer_order.c runs
 * inside FFmpeg's lock (amiga_library.h); the entry points in demuxer_lib.c
 * take it.
 */
#ifndef DEMUXER_PRIVATE_H
#define DEMUXER_PRIVATE_H

#include <stdint.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/demuxer.h>

#include <libavformat/avformat.h>
#include <libavcodec/bsf.h>

#define READER_SCRATCH_SIZE (256u * 1024u) /* bytes read and dropped at a time, to move forward where the handler cannot */

/* The file under a source */
struct reader
{
	BPTR file;        /* the open file */
	int64_t position; /* where the next read starts */
	int64_t size;     /* the file's bytes */
	BOOL wide;        /* the handler takes the 64-bit position packets */
	UBYTE *scratch;   /* READER_SCRATCH_SIZE bytes for reading forward, made when first needed */
};

BOOL reader_open(struct reader *reader, CONST_STRPTR name);
void reader_close(struct reader *reader);
LONG reader_read(struct reader *reader, APTR buffer, LONG length);
BOOL reader_seek(struct reader *reader, int64_t position);
BOOL reader_seekable(const struct reader *reader);

#define SOURCE_IO_BUFFER (256u * 1024u) /* what FFmpeg reads from the reader at a time */
#define SOURCE_STREAMS_MAX 32u          /* the streams DMX_SelectStreams()'s mask can name */
#define ORDER_HELD_MOST 16u             /* the packets of a run an order holds: a longer run goes as the file timed it */

/* What an MPEG video packet holds: its first picture's coding type */
enum picture_kind
{
	PICTURE_NONE,          /* no picture: headers alone, or a picture not coded */
	PICTURE_INTRA,         /* an I-picture: decoding can start here */
	PICTURE_PREDICTED,     /* a P- or S-picture: a reference too */
	PICTURE_BIDIRECTIONAL, /* a B-picture */
};

/* Where an order stands since the open or the last move */
enum order_start
{
	ORDER_BEFORE_INTRA, /* waiting for an I-picture: what comes before it is dropped */
	ORDER_AFTER_INTRA,  /* the I-picture came: B-pictures up to the next reference are dropped, their references unread */
	ORDER_FLOWING,      /* everything passes */
};

/* A packet an order holds */
struct order_entry
{
	AVPacket *packet;       /* the packet */
	enum picture_kind kind; /* what it holds */
	BOOL passes;            /* it goes to the caller; FALSE for a dropped B-picture, kept to time its run */
};

/* One MPEG video stream's pictures on their way: a reference and what follows it held until their times are settled */
struct order
{
	enum AVCodecID codec;                    /* MPEG-1, MPEG-2 or MPEG-4 Part 2 video */
	struct order_entry held[ORDER_HELD_MOST]; /* in decode order: the ready packets, then the run, its reference first */
	unsigned int count;                      /* how many are held */
	unsigned int ready;                      /* of them, from the first, those whose times are settled */
	enum order_start start;                  /* how far the stream is since the open or the last move */
};

/* An open file */
struct DMXSource
{
	struct reader reader;      /* the file */
	AVFormatContext *format;   /* FFmpeg's demuxer */
	AVIOContext *io;           /* its reads, through the reader */
	AVBSFContext **filters;    /* per stream: what makes its packets what the decoders take (codec_filter()), NULL for none */
	struct order **orders;     /* per stream: the order its pictures' times are settled in (demuxer_order.c), NULL for none */
	ULONG selected;            /* the streams whose packets DMX_ReadPacket() gives, a bit each */
	BOOL ended;                /* the file's last packet was read: what the orders hold goes, then DMXERR_END */
};

struct DMXSource *source_open(CONST_STRPTR name, LONG *error);
void source_close(struct DMXSource *source);
LONG source_info(struct DMXSource *source, struct DMXInfo *info);
LONG source_stream(struct DMXSource *source, ULONG index, struct DMXStream *stream);
LONG source_select(struct DMXSource *source, ULONG mask);
LONG source_read(struct DMXSource *source, struct DMXPacket *packet);
void source_release(struct DMXPacket *packet);
LONG source_seek(struct DMXSource *source, VCTime time, ULONG flags);
LONG source_chapter(struct DMXSource *source, ULONG index, struct DMXChapter *chapter);

ULONG codec_kind(enum AVMediaType type);
ULONG codec_number(const AVCodecParameters *parameters);
ULONG codec_tag(uint32_t tag);
ULONG codec_color_space(const AVCodecParameters *parameters);
ULONG codec_pcm_flags(enum AVCodecID id);
const char *codec_filter(const AVCodecParameters *parameters);

BOOL order_needed(const AVCodecParameters *parameters);
struct order *order_make(const AVCodecParameters *parameters);
void order_free(struct order **order);
void order_flush(struct order *order);
void order_put(struct order *order, AVPacket *packet);
void order_end(struct order *order);
AVPacket *order_take(struct order *order);

#endif /* DEMUXER_PRIVATE_H */
