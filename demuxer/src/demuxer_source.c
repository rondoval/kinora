/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * demuxer_source.c - a source: FFmpeg's demuxer over the reader, its streams
 * and packets as the public structures, seeking.
 *
 * Every function here runs inside FFmpeg's lock.  FFmpeg reads the file
 * through io_read() and io_seek() only, so the reader decides how the file is
 * reached.  H.264 and H.265 streams that the file holds as length-prefixed
 * NAL units go through FFmpeg's bitstream filter to an Annex-B byte stream,
 * their configuration with them, and MPEG-4 Part 2 streams through the one
 * that unpacks packed B-pictures (codec_filter()).
 */
#include <string.h>
#include <proto/dos.h>
#include <libavutil/avstring.h>

#include "demuxer_private.h"

static const AVRational MICROSECONDS = {1, 1000000}; /* VCTime's unit as an FFmpeg time base */

/* ---- the file, as FFmpeg reads it ------------------------------------------------ */

/**
 * io_read() - FFmpeg's read callback.
 * @opaque: the reader
 * @buffer: where the bytes go
 * @size:   how many at most
 *
 * Return: the bytes read, AVERROR_EOF at the end, AVERROR(EIO) for an error.
 */
static int io_read(void *opaque, uint8_t *buffer, int size)
{
	LONG got = reader_read(opaque, buffer, size);
	if (got > 0)
		return (int)got;
	return got == 0 ? AVERROR_EOF : AVERROR(EIO);
}

/**
 * io_seek() - FFmpeg's seek callback.
 * @opaque: the reader
 * @offset: where to, by @whence
 * @whence: SEEK_SET, SEEK_CUR or SEEK_END; AVSEEK_SIZE asks for the file's size
 *
 * Return: the new position (the size for AVSEEK_SIZE), -1 when the file cannot go there.
 */
static int64_t io_seek(void *opaque, int64_t offset, int whence)
{
	struct reader *reader = opaque;
	if (whence == AVSEEK_SIZE)
		return reader->size;
	whence &= ~AVSEEK_FORCE;
	if (whence == SEEK_CUR)
		offset += reader->position;
	else if (whence == SEEK_END)
		offset += reader->size;
	return reader_seek(reader, offset) ? reader->position : -1;
}

/* ---- opening and closing ---------------------------------------------------------- */

/**
 * filters_free() - Free the streams' bitstream filters.
 * @source: the source
 */
static void filters_free(struct DMXSource *source)
{
	if (source->filters == NULL)
		return;
	for (unsigned int i = 0; i < source->format->nb_streams; i++)
		av_bsf_free(&source->filters[i]);
	av_freep(&source->filters);
}

/**
 * filter_make() - The bitstream filter of one stream, ready for its packets.
 * @stream: the stream
 * @name:   the filter's name
 *
 * Return: the filter, NULL for no memory or a configuration the filter refuses.
 */
static AVBSFContext *filter_make(const AVStream *stream, const char *name)
{
	AVBSFContext *filter = NULL;
	const AVBitStreamFilter *kind = av_bsf_get_by_name(name);
	if (kind == NULL || av_bsf_alloc(kind, &filter) < 0)
		return NULL;
	filter->time_base_in = stream->time_base;
	if (avcodec_parameters_copy(filter->par_in, stream->codecpar) < 0 || av_bsf_init(filter) < 0)
		av_bsf_free(&filter);
	return filter;
}

/**
 * filters_make() - Give every stream that needs one its filter (codec_filter()).
 * @source: the source, its streams known
 *
 * Return: 0, or DMXERR_NOMEM.
 */
static LONG filters_make(struct DMXSource *source)
{
	unsigned int count = source->format->nb_streams;
	source->filters = av_calloc(count ? count : 1, sizeof(*source->filters));
	if (source->filters == NULL)
		return DMXERR_NOMEM;
	for (unsigned int i = 0; i < count; i++)
	{
		const AVStream *stream = source->format->streams[i];
		const char *name = codec_filter(stream->codecpar);
		if (name == NULL)
			continue;
		source->filters[i] = filter_make(stream, name);
		if (source->filters[i] == NULL)
			return DMXERR_NOMEM;
	}
	return 0;
}

/**
 * streams_default() - Select what a player plays: every video, audio and subtitle stream the mask can name.
 * @source: the source, its streams known
 */
static void streams_default(struct DMXSource *source)
{
	ULONG mask = 0;
	for (unsigned int i = 0; i < source->format->nb_streams && i < SOURCE_STREAMS_MAX; i++)
		if (codec_kind(source->format->streams[i]->codecpar->codec_type) != DMXK_Other)
			mask |= 1UL << i;
	(void)source_select(source, mask);
}

/**
 * format_open() - Put FFmpeg's demuxer over the open reader and learn the streams.
 * @source: the source, its reader open
 *
 * Return: 0, DMXERR_NOMEM, or DMXERR_FORMAT for a file FFmpeg makes nothing of.
 */
static LONG format_open(struct DMXSource *source)
{
	uint8_t *buffer = av_malloc(SOURCE_IO_BUFFER);
	if (buffer == NULL)
		return DMXERR_NOMEM;
	source->io = avio_alloc_context(buffer, SOURCE_IO_BUFFER, 0, &source->reader, io_read, NULL, io_seek);
	if (source->io == NULL)
	{
		av_free(buffer);
		return DMXERR_NOMEM;
	}
	source->io->seekable = reader_seekable(&source->reader) ? AVIO_SEEKABLE_NORMAL : 0;

	source->format = avformat_alloc_context();
	if (source->format == NULL)
		return DMXERR_NOMEM;
	source->format->pb = source->io;
	if (avformat_open_input(&source->format, NULL, NULL, NULL) < 0) /* frees the context when it fails */
		return DMXERR_FORMAT;
	if (avformat_find_stream_info(source->format, NULL) < 0)
		return DMXERR_FORMAT;
	return filters_make(source);
}

/**
 * source_close() - Close a source and free everything it holds.
 * @source: the source, as far as source_open() got with it
 */
void source_close(struct DMXSource *source)
{
	if (source->format != NULL)
	{
		filters_free(source);
		avformat_close_input(&source->format);
	}
	if (source->io != NULL)
	{
		av_freep(&source->io->buffer);
		avio_context_free(&source->io);
	}
	reader_close(&source->reader);
	av_free(source);
}

/**
 * source_open() - Open a file as a source.
 * @name:  the file
 * @error: gets the DMXERR_* when it fails
 *
 * Return: the source, NULL when the file does not open or is no container this library reads.
 */
struct DMXSource *source_open(CONST_STRPTR name, LONG *error)
{
	struct DMXSource *source = av_mallocz(sizeof(*source));
	if (source == NULL)
	{
		*error = DMXERR_NOMEM;
		return NULL;
	}
	*error = reader_open(&source->reader, name) ? format_open(source) : DMXERR_FILE;
	if (*error != 0)
	{
		source_close(source);
		return NULL;
	}
	streams_default(source);
	return source;
}

/* ---- describing ------------------------------------------------------------------ */

/**
 * time_of() - One of FFmpeg's timestamps as a VCTime.
 * @value: the timestamp, AV_NOPTS_VALUE for none
 * @base:  its time base
 *
 * Return: microseconds, VC_TIME_UNKNOWN for none.
 */
static VCTime time_of(int64_t value, AVRational base)
{
	return value != AV_NOPTS_VALUE ? av_rescale_q(value, base, MICROSECONDS) : VC_TIME_UNKNOWN;
}

/**
 * text_copy() - Copy a metadata entry into a fixed field, cut to fit.
 * @field: the field
 * @size:  its bytes
 * @from:  the dictionary
 * @key:   the entry's name
 */
static void text_copy(char *field, size_t size, AVDictionary *from, const char *key)
{
	const AVDictionaryEntry *entry = av_dict_get(from, key, NULL, 0);
	if (entry != NULL)
		av_strlcpy(field, entry->value, size);
}

/**
 * answer_copy() - Hand a filled structure to the caller, as much of it as the caller's has room for.
 * @to:    the caller's structure, its Size set
 * @from:  the library's, filled
 * @size:  the library's size of it
 *
 * Both start with their Size word; the caller's is kept.
 */
static void answer_copy(APTR to, CONST_APTR from, ULONG size)
{
	ULONG room = *(const ULONG *)to;
	memcpy((UBYTE *)to + sizeof(ULONG), (const UBYTE *)from + sizeof(ULONG), (room < size ? room : size) - sizeof(ULONG));
}

/**
 * source_info() - Describe the file as a whole.
 * @source: the source
 * @info:   the caller's structure, its Size set
 *
 * Return: 0.
 */
LONG source_info(struct DMXSource *source, struct DMXInfo *info)
{
	const AVFormatContext *format = source->format;
	struct DMXInfo answer = {
	    .Streams = format->nb_streams,
	    .Chapters = format->nb_chapters,
	    .Duration = time_of(format->duration, AV_TIME_BASE_Q),
	    .FileSizeHigh = (ULONG)(source->reader.size >> 32),
	    .FileSizeLow = (ULONG)source->reader.size,
	    .Seekable = reader_seekable(&source->reader),
	};
	av_strlcpy(answer.Format, format->iformat->name, sizeof(answer.Format));
	answer_copy(info, &answer, sizeof(answer));
	return 0;
}

/**
 * stream_flags() - The DMXSF_* of a stream.
 * @stream: the stream
 *
 * Return: the flags.
 */
static ULONG stream_flags(const AVStream *stream)
{
	ULONG flags = 0;
	if (stream->disposition & AV_DISPOSITION_DEFAULT)
		flags |= DMXSF_Default;
	if (stream->disposition & AV_DISPOSITION_FORCED)
		flags |= DMXSF_Forced;
	return flags;
}

/**
 * source_stream() - Describe one stream.
 * @source: the source
 * @index:  the stream's number
 * @stream: the caller's structure, its Size set
 *
 * The configuration is the stream's filter's where it has one (codec_filter()).
 *
 * Return: 0, or DMXERR_BADPARAM for a number past the last stream.
 */
LONG source_stream(struct DMXSource *source, ULONG index, struct DMXStream *stream)
{
	if (index >= source->format->nb_streams)
		return DMXERR_BADPARAM;
	AVStream *from = source->format->streams[index];
	const AVCodecParameters *parameters = source->filters[index] != NULL ? source->filters[index]->par_out : from->codecpar;
	AVRational aspect = av_guess_sample_aspect_ratio(source->format, from, NULL);
	struct DMXStream answer = {
	    .Kind = codec_kind(parameters->codec_type),
	    .Codec = codec_number(parameters),
	    .Tag = codec_tag(from->codecpar->codec_tag),
	    .Flags = stream_flags(from),
	    .Extradata = parameters->extradata_size > 0 ? parameters->extradata : NULL,
	    .ExtradataSize = (ULONG)parameters->extradata_size,
	    .Duration = time_of(from->duration, from->time_base),
	    .Width = (ULONG)parameters->width,
	    .Height = (ULONG)parameters->height,
	    .Par = {aspect.num, aspect.den},
	    .FrameRate = {from->avg_frame_rate.num, from->avg_frame_rate.den},
	    .ColorSpace = codec_color_space(parameters),
	    .SampleRate = (ULONG)parameters->sample_rate,
	    .Channels = (ULONG)parameters->ch_layout.nb_channels,
	    .Bits = (ULONG)parameters->bits_per_coded_sample,
	    .PcmFlags = codec_pcm_flags(parameters->codec_id),
	};
	text_copy(answer.Language, sizeof(answer.Language), from->metadata, "language");
	text_copy(answer.Title, sizeof(answer.Title), from->metadata, "title");
	answer_copy(stream, &answer, sizeof(answer));
	return 0;
}

/**
 * source_chapter() - Describe one chapter.
 * @source:  the source
 * @index:   the chapter's number
 * @chapter: the caller's structure, its Size set
 *
 * Return: 0, or DMXERR_BADPARAM for a number past the last chapter.
 */
LONG source_chapter(struct DMXSource *source, ULONG index, struct DMXChapter *chapter)
{
	if (index >= source->format->nb_chapters)
		return DMXERR_BADPARAM;
	const AVChapter *from = source->format->chapters[index];
	struct DMXChapter answer = {
	    .Start = time_of(from->start, from->time_base),
	    .End = time_of(from->end, from->time_base),
	};
	text_copy(answer.Title, sizeof(answer.Title), from->metadata, "title");
	answer_copy(chapter, &answer, sizeof(answer));
	return 0;
}

/* ---- packets ---------------------------------------------------------------------- */

/**
 * source_select() - Choose the streams whose packets DMX_ReadPacket() gives.
 * @source: the source
 * @mask:   a bit a stream, bit 0 the first
 *
 * FFmpeg is told too, so that it does not keep what nobody reads.
 *
 * Return: 0.
 */
LONG source_select(struct DMXSource *source, ULONG mask)
{
	source->selected = mask;
	for (unsigned int i = 0; i < source->format->nb_streams; i++)
	{
		BOOL wanted = i < SOURCE_STREAMS_MAX && (mask & (1UL << i)) != 0;
		source->format->streams[i]->discard = wanted ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
	}
	return 0;
}

/**
 * packet_next() - Read the file's next packet of a selected stream, through its stream's filter.
 * @source: the source
 * @packet: an empty FFmpeg packet to fill
 *
 * Return: 0 with the packet, DMXERR_END at the file's end, DMXERR_FILE for a read that failed.
 */
static LONG packet_next(struct DMXSource *source, AVPacket *packet)
{
	for (;;)
	{
		int result = av_read_frame(source->format, packet);
		if (result < 0)
			return result == AVERROR_EOF ? DMXERR_END : DMXERR_FILE;
		unsigned int index = (unsigned int)packet->stream_index;
		if (index >= SOURCE_STREAMS_MAX || (source->selected & (1UL << index)) == 0)
		{
			av_packet_unref(packet);
			continue;
		}
		AVBSFContext *filter = source->filters[index];
		if (filter == NULL)
			return 0;
		if (av_bsf_send_packet(filter, packet) == 0 && av_bsf_receive_packet(filter, packet) == 0)
			return 0;
		av_packet_unref(packet); /* the filter kept it or made nothing of it: the next one */
	}
}

/**
 * source_read() - Give the caller the next packet.
 * @source: the source
 * @packet: the caller's structure, its Size set
 *
 * Return: 0, DMXERR_END, DMXERR_FILE or DMXERR_NOMEM.
 */
LONG source_read(struct DMXSource *source, struct DMXPacket *packet)
{
	AVPacket *read = av_packet_alloc();
	if (read == NULL)
		return DMXERR_NOMEM;
	LONG error = packet_next(source, read);
	if (error != 0)
	{
		av_packet_free(&read);
		return error;
	}
	AVRational base = source->format->streams[read->stream_index]->time_base;
	struct DMXPacket answer = {
	    .Data = read->data,
	    .Length = (ULONG)read->size,
	    .Stream = (ULONG)read->stream_index,
	    .Flags = ((read->flags & AV_PKT_FLAG_KEY) ? DMXPF_KeyFrame : 0) | ((read->flags & AV_PKT_FLAG_CORRUPT) ? DMXPF_Corrupt : 0),
	    .Pts = time_of(read->pts, base),
	    .Dts = time_of(read->dts, base),
	    .Duration = read->duration > 0 ? time_of(read->duration, base) : VC_TIME_UNKNOWN,
	    .Private = read,
	};
	answer_copy(packet, &answer, sizeof(answer));
	return 0;
}

/**
 * source_release() - Take a packet back.
 * @packet: the caller's structure as source_read() filled it; one released already is left alone
 */
void source_release(struct DMXPacket *packet)
{
	AVPacket *read = packet->Private;
	av_packet_free(&read);
	packet->Private = NULL;
	packet->Data = NULL;
	packet->Length = 0;
}

/**
 * source_seek() - Move to a key frame near a time.
 * @source: the source
 * @time:   the time
 * @flags:  DMXSEEK_*
 *
 * The streams' filters start afresh.  The next packet's time says where the
 * source went.
 *
 * Return: 0, or DMXERR_SEEK when the file or its handler cannot go there.
 */
LONG source_seek(struct DMXSource *source, VCTime time, ULONG flags)
{
	if (av_seek_frame(source->format, -1, time, flags == DMXSEEK_After ? 0 : AVSEEK_FLAG_BACKWARD) < 0)
		return DMXERR_SEEK;
	for (unsigned int i = 0; i < source->format->nb_streams; i++)
		if (source->filters[i] != NULL)
			av_bsf_flush(source->filters[i]);
	return 0;
}
