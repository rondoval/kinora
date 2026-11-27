/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * demuxer_private.h - demuxer.library's own structures.
 *
 *   demuxer_lib.c      the library: its name, its function table, the calls' entry points
 *   demuxer_source.c   a source: FFmpeg's demuxer over the reader, streams, packets, seeking
 *   demuxer_codecs.c   FFmpeg's codec numbers and colours as the public headers' own
 *   demuxer_reader.c   the file: dos.library reads and positions past 2 GiB
 *
 * Everything in demuxer_source.c and demuxer_codecs.c runs inside FFmpeg's
 * lock (amiga_library.h); the entry points in demuxer_lib.c take it.
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

/* An open file */
struct DMXSource
{
	struct reader reader;      /* the file */
	AVFormatContext *format;   /* FFmpeg's demuxer */
	AVIOContext *io;           /* its reads, through the reader */
	AVBSFContext **filters;    /* per stream: what makes its packets what the decoders take (codec_filter()), NULL for none */
	ULONG selected;            /* the streams whose packets DMX_ReadPacket() gives, a bit each */
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

#endif /* DEMUXER_PRIVATE_H */
