/* SPDX-License-Identifier: MPL-2.0 */
/*
 * filminfo_video.c - FilmInfo's picture: the video codec drivers' decoder rows, and a film's
 * video stream decoded through videocodec.library as Kinora's engine decodes it, as fast as
 * it goes (FilmInfo film VIDEO): its shape, the backends that took the session, each
 * picture's digest if asked, the pictures and the time it took.
 */
#include <stdio.h>
#include <string.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <proto/demuxer.h>
#include <proto/videocodec.h>

#include "filminfo.h"
#include "md5.h"

#define SEEK_AFTER 50 /* pictures decoded before SEEK moves */

struct Device *TimerBase;

/* A format's name for FORMAT */
struct format_name
{
	const char *name; /* as typed */
	ULONG format;     /* VCF_* */
};

static const struct format_name format_names[] = {
    {"I420", VCF_I420}, {"BGRA32", VCF_BGRA32}, {"RGBA32", VCF_RGBA32}, {"RGB16", VCF_RGB16}, {"RGB24", VCF_RGB24},
};

/* The run */
struct run
{
	struct DMXSource *source;      /* the file */
	struct VCSession *session;     /* the decoder */
	ULONG stream;                  /* the video stream's number */
	BOOL md5;                      /* print each picture's digest */
	LONG seek_ms;                  /* where SEEK moves, -1 for no move */
	struct VCPacket *free[64];     /* the decoder's empty input packets */
	ULONG free_count;              /* how many */
	struct DMXPacket read;         /* the file's packet being sent */
	BOOL have_read;                /* read holds one */
	ULONG read_sent;               /* its bytes sent so far */
	BOOL file_ended;               /* the file's end was read */
	BOOL end_sent;                 /* the end mark went to the decoder */
	BOOL done;                     /* the decoder's end came back, or it failed */
	LONG status;                   /* 0, or the error that stopped the run */
	ULONG packets;                 /* the file's packets sent */
	ULONG packets_most;            /* PACKETS: the end after this many, 0 for the stream's end */
	ULONG pictures;                /* pictures out */
	VCTime first;                  /* the first picture's time */
	VCTime last;                   /* the last's */
	ULONG signal;                  /* the session's signal mask */
};

/**
 * now_us() - The E clock, in microseconds.
 *
 * Return: microseconds since some start.
 */
static unsigned long long now_us(void)
{
	struct EClockVal clock;
	ULONG rate = ReadEClock(&clock);
	unsigned long long ticks = ((unsigned long long)clock.ev_hi << 32) | clock.ev_lo;
	return ticks / rate * 1000000ull + ticks % rate * 1000000ull / rate;
}

/**
 * tag_text() - A FourCC as text, four characters and a NUL.
 * @tag:  VC_FOURCC()
 * @text: room for five characters
 *
 * Return: @text; "none" for 0.
 */
static const char *tag_text(ULONG tag, char text[5])
{
	if (tag == 0)
		return "none";
	for (int i = 0; i < 4; i++)
	{
		char c = (char)(tag >> (24 - 8 * i));
		text[i] = c >= ' ' && c <= '~' ? c : '.';
	}
	text[4] = 0;
	return text;
}

/**
 * video_codecs_show() - Print every video codec driver's decoder rows.
 *
 * Return: RETURN_OK, RETURN_WARN for none.
 */
int video_codecs_show(void)
{
	ULONG rows = 0;
	struct VCCodecInfo info = {.Size = sizeof(info)};
	for (ULONG index = 0; VC_QueryCodec(index, &info) == 0; index++, info.Size = sizeof(info))
	{
		if (info.Kind != VCK_Decoder)
			continue;
		char tag[5];
		printf("video codec %lu: backend=%s priority=%ld max=%lux%lu formats=%lx chain=%lx flags=%lx tag=%s\n",
		       (unsigned long)info.Codec, info.Backend, (long)info.Priority, (unsigned long)info.MaxWidth,
		       (unsigned long)info.MaxHeight, (unsigned long)info.OutputFormats, (unsigned long)info.ChainOutputFormats,
		       (unsigned long)info.Flags, tag_text(info.Tag, tag));
		rows++;
	}
	printf("video_decoders=%lu\n", (unsigned long)rows);
	return rows != 0 ? RETURN_OK : RETURN_WARN;
}

/**
 * same_text() - Whether two names are the same, letters in either case.
 * @typed: the name typed
 * @known: a name known, upper case
 *
 * Return: TRUE if they are.
 */
static BOOL same_text(const char *typed, const char *known)
{
	for (; *typed != 0 && *known != 0; typed++, known++)
	{
		char c = *typed >= 'a' && *typed <= 'z' ? (char)(*typed - 'a' + 'A') : *typed;
		if (c != *known)
			return FALSE;
	}
	return *typed == *known;
}

/**
 * format_of() - FORMAT's VCF_*.
 * @name: the name typed, NULL for none
 *
 * Return: VCF_*, VCF_I420 for none, VCF_Unknown for a name not known.
 */
static ULONG format_of(const char *name)
{
	if (name == NULL)
		return VCF_I420;
	for (ULONG i = 0; i < sizeof(format_names) / sizeof(format_names[0]); i++)
		if (same_text(name, format_names[i].name))
			return format_names[i].format;
	return VCF_Unknown;
}

/**
 * video_format_known() - Whether FORMAT, if given, names a format FilmInfo asks for.
 * @args: the template's
 *
 * Return: TRUE for no FORMAT or a known one.
 */
BOOL video_format_known(const LONG *args)
{
	return format_of((const char *)args[ARG_FORMAT]) != VCF_Unknown;
}

/**
 * tag_if() - A tag that counts only when its value was given.
 * @given: the value was given
 * @tag:   the tag
 *
 * Return: the tag, or TAG_IGNORE.
 */
static Tag tag_if(BOOL given, Tag tag)
{
	return given ? tag : TAG_IGNORE;
}

/* The backends the open chose */
struct chosen
{
	char backend[VC_BACKEND_NAME_SIZE];   /* the decoder's */
	char converter[VC_BACKEND_NAME_SIZE]; /* the converter's behind it, empty for none */
};

/**
 * session_open() - Open the decoder for the stream, as Kinora's engine opens one.
 * @stream: the stream
 * @args:   the template's
 * @signal: the signal bit for the session
 * @error:  gets the VCERR_* of a refusal
 * @chosen: gets the backends' names
 *
 * Return: the session, NULL when refused.
 */
static struct VCSession *session_open(const struct DMXStream *stream, const LONG *args, BYTE signal, LONG *error, struct chosen *chosen)
{
	BOOL has_rate = stream->FrameRate.Num > 0 && stream->FrameRate.Den > 0;
	ULONG width = number_of(args, ARG_WIDTH), height = number_of(args, ARG_HEIGHT);
	BOOL sized = width != 0 && height != 0;
	struct TagItem tags[] = {
	    {VCA_Codec, stream->Codec},
	    {VCA_Kind, VCK_Decoder},
	    {VCA_CodecTag, stream->Tag},
	    {VCA_OutputFormat, format_of((const char *)args[ARG_FORMAT])},
	    {tag_if(sized, VCA_OutputWidth), width},
	    {tag_if(sized, VCA_OutputHeight), height},
	    {VCA_Width, stream->Width},
	    {VCA_Height, stream->Height},
	    {VCA_SigMask, 1UL << signal},
	    {VCA_Framed, TRUE},
	    {tag_if(stream->ExtradataSize != 0, VCA_Extradata), (ULONG)stream->Extradata},
	    {tag_if(stream->ExtradataSize != 0, VCA_ExtradataSize), stream->ExtradataSize},
	    {tag_if(has_rate, VCA_FrameRateNum), (ULONG)stream->FrameRate.Num},
	    {tag_if(has_rate, VCA_FrameRateDen), (ULONG)stream->FrameRate.Den},
	    {tag_if(args[ARG_SKIP] != 0, VCA_Skip), number_of(args, ARG_SKIP)},
	    {tag_if(args[ARG_BACKEND] != 0, VCA_Backend), (ULONG)args[ARG_BACKEND]},
	    {VCA_ErrorPtr, (ULONG)error},
	    {VCA_BackendName, (ULONG)chosen->backend},
	    {VCA_ConverterName, (ULONG)chosen->converter},
	    {TAG_DONE, 0},
	};
	return VC_OpenSessionTagList(tags);
}

/**
 * picture_digest() - Print a picture's MD5: its visible luma rows, then each chroma plane's.
 * @run:   the run
 * @frame: the picture, I420
 */
static void picture_digest(struct run *run, const struct VCFrame *frame)
{
	struct md5 md5;
	md5_start(&md5);
	for (ULONG plane = 0; plane < 3; plane++)
	{
		ULONG shift = plane == 0 ? 0 : 1;
		ULONG width = ((ULONG)frame->Crop.Width + shift) >> shift;
		ULONG rows = ((ULONG)frame->Crop.Height + shift) >> shift;
		const UBYTE *row = (const UBYTE *)frame->Data + frame->Offset[plane] + ((ULONG)frame->Crop.Y >> shift) * frame->Pitch[plane] +
		                   ((ULONG)frame->Crop.X >> shift);
		for (ULONG y = 0; y < rows; y++, row += frame->Pitch[plane])
			md5_add(&md5, row, width);
	}
	UBYTE digest[MD5_BYTES];
	md5_end(&md5, digest);
	printf("picture %lu pts_ms=%ld md5=", (unsigned long)run->pictures, milliseconds(frame->Pts));
	for (ULONG i = 0; i < MD5_BYTES; i++)
		printf("%02x", digest[i]);
	printf("\n");
}

/**
 * picture_take() - Count a picture, print its digest if asked, give it back.
 * @run:   the run
 * @frame: the picture
 */
static void picture_take(struct run *run, struct VCFrame *frame)
{
	if (run->md5 && frame->Format == VCF_I420)
		picture_digest(run, frame);
	if (run->pictures == 0)
		run->first = frame->Pts;
	if (frame->Pts != VC_TIME_UNKNOWN)
		run->last = frame->Pts;
	run->pictures++;
	VC_ReleaseFrame(run->session, frame);
}

/**
 * event_take() - Act on an event of the decoder.
 * @run:   the run
 * @event: the event
 */
static void event_take(struct run *run, const struct VCEvent *event)
{
	const struct VCStreamInfo *info = &event->Stream;
	switch (event->Event)
	{
	case VCEV_StreamInfo:
		printf("info width=%lu height=%lu crop=%ld,%ld,%ldx%ld format=%lu colorspace=%lu rate=%ld/%ld par=%ld/%ld flags=%lx\n",
		       (unsigned long)info->Width, (unsigned long)info->Height, (long)info->Crop.X, (long)info->Crop.Y,
		       (long)info->Crop.Width, (long)info->Crop.Height, (unsigned long)info->Format, (unsigned long)info->ColorSpace,
		       (long)info->FrameRate.Num, (long)info->FrameRate.Den, (long)info->Par.Num, (long)info->Par.Den,
		       (unsigned long)info->Flags);
		break;
	case VCEV_EndOfStream:
		run->done = TRUE;
		break;
	default:
		run->status = event->Status != 0 ? event->Status : -1;
		run->done = TRUE;
		break;
	}
}

/**
 * items_take() - Take everything the decoder hands out.
 * @run: the run
 *
 * Return: TRUE when there was anything.
 */
static BOOL items_take(struct run *run)
{
	BOOL any = FALSE;
	struct VCItem item;
	while (VC_Poll(run->session, &item))
	{
		any = TRUE;
		if (item.Type == VCIT_PacketReturned && run->free_count < sizeof(run->free) / sizeof(run->free[0]))
			run->free[run->free_count++] = item.u.Packet;
		else if (item.Type == VCIT_Frame)
			picture_take(run, item.u.Frame);
		else if (item.Type == VCIT_Event)
			event_take(run, &item.u.Event);
	}
	return any;
}

/**
 * read_next() - Read the file's next packet of the stream, unless one is being sent.
 * @run: the run
 *
 * After PACKETS of them the file counts as ended: the end mark goes next.
 */
static void read_next(struct run *run)
{
	if (run->have_read || run->file_ended)
		return;
	if (run->packets_most != 0 && run->packets >= run->packets_most)
	{
		run->file_ended = TRUE; /* PACKETS: as far as the decode goes */
		return;
	}
	run->read.Size = sizeof(run->read);
	LONG error = DMX_ReadPacket(run->source, &run->read);
	run->have_read = error == 0;
	run->read_sent = 0;
	run->file_ended = error != 0;
}

/**
 * piece_send() - Send the next piece of the file's packet: a picture cut over as many input packets as it needs.
 * @run: the run, an empty input packet in hand and a file's packet read
 */
static void piece_send(struct run *run)
{
	struct VCPacket *packet = run->free[--run->free_count];
	ULONG left = run->read.Length - run->read_sent;
	ULONG piece = left < packet->Capacity ? left : packet->Capacity;
	memcpy(packet->Data, (const UBYTE *)run->read.Data + run->read_sent, piece);
	packet->Length = piece;
	packet->Pts = run->read.Pts;
	packet->Flags = (run->read_sent == 0 ? VCPF_FrameStart : 0) | (piece == left ? VCPF_FrameEnd : 0) |
	                ((run->read.Flags & DMXPF_KeyFrame) != 0 ? VCPF_KeyFrame : 0);
	LONG error = VC_SendPacket(run->session, packet);
	if (error != 0)
	{
		run->status = error;
		run->done = TRUE;
	}
	run->read_sent += piece;
	if (piece == left)
	{
		DMX_ReleasePacket(run->source, &run->read);
		run->have_read = FALSE;
		run->packets++;
	}
}

/**
 * end_send() - Send the end mark.
 * @run: the run, an empty input packet in hand
 */
static void end_send(struct run *run)
{
	struct VCPacket *packet = run->free[--run->free_count];
	packet->Length = 0;
	packet->Pts = VC_TIME_UNKNOWN;
	packet->Flags = VCPF_EndOfStream;
	if (VC_SendPacket(run->session, packet) != 0)
		run->done = TRUE;
	run->end_sent = TRUE;
}

/**
 * move_now() - SEEK: flush the decoder and move the file, once, after SEEK_AFTER pictures.
 * @run: the run
 */
static void move_now(struct run *run)
{
	if (run->seek_ms < 0 || run->pictures < SEEK_AFTER)
		return;
	VCTime time = (VCTime)run->seek_ms * MICROSECONDS_PER_MS;
	run->seek_ms = -1;
	if (run->have_read)
		DMX_ReleasePacket(run->source, &run->read);
	run->have_read = FALSE;
	VC_Flush(run->session);
	LONG error = DMX_Seek(run->source, &time, DMXSEEK_Before);
	printf("moved=%ld landed_ms=%ld pictures_before=%lu\n", (long)error, milliseconds(time), (unsigned long)run->pictures);
	run->file_ended = error != 0;
}

/**
 * feed() - Fill the decoder's empty input packets from the file; the end mark after the last.
 * @run: the run
 *
 * Return: TRUE when anything went to the decoder.
 */
static BOOL feed(struct run *run)
{
	BOOL any = FALSE;
	move_now(run);
	while (run->free_count != 0 && !run->end_sent && !run->done)
	{
		read_next(run);
		if (run->have_read)
			piece_send(run);
		else
			end_send(run);
		any = TRUE;
	}
	return any;
}

/**
 * decode_run() - Decode the stream to its end, as fast as the decoder goes.
 * @run: the run, its session open
 *
 * Return: the run's time in microseconds.
 */
static unsigned long long decode_run(struct run *run)
{
	unsigned long long start = now_us();
	while (!run->done)
	{
		BOOL any = items_take(run);
		any |= feed(run);
		if (!any && (Wait(run->signal | SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0)
		{
			run->status = -1;
			break;
		}
	}
	unsigned long long took = now_us() - start;
	if (run->have_read)
		DMX_ReleasePacket(run->source, &run->read);
	return took;
}

/**
 * stream_decode() - Decode one video stream of the film and print what came out.
 * @source: the source
 * @args:   the template's
 *
 * Return: RETURN_OK, RETURN_FAIL when there is no stream, no decoder or the decoding fails.
 */
static int stream_decode(struct DMXSource *source, const LONG *args)
{
	struct DMXStream stream;
	LONG number = stream_find(source, DMXK_Video, args, &stream);
	BYTE signal = AllocSignal(-1);
	if (number < 0 || signal < 0)
	{
		printf("no video stream\n");
		return RETURN_FAIL;
	}
	DMX_SelectStreams(source, 1UL << number);
	char tag[5];
	struct chosen chosen = {.backend = "", .converter = ""};
	LONG error = 0;
	struct run run = {.source = source, .stream = (ULONG)number, .md5 = args[ARG_MD5] != 0, .first = VC_TIME_UNKNOWN,
	                  .last = VC_TIME_UNKNOWN, .signal = 1UL << signal,
	                  .seek_ms = args[ARG_SEEK] != 0 ? (LONG)number_of(args, ARG_SEEK) : -1,
	                  .packets_most = number_of(args, ARG_PACKETS)};
	run.session = session_open(&stream, args, signal, &error, &chosen);
	printf("stream=%ld codec=%lu tag=%s size=%lux%lu open=%ld\n", (long)number, (unsigned long)stream.Codec, tag_text(stream.Tag, tag),
	       (unsigned long)stream.Width, (unsigned long)stream.Height, (long)error);
	if (run.session == NULL)
	{
		FreeSignal(signal);
		return RETURN_FAIL;
	}
	printf("backend=%s converter=%s\n", chosen.backend, chosen.converter[0] != 0 ? chosen.converter : "none");
	unsigned long long took = decode_run(&run);
	VC_CloseSession(run.session);
	FreeSignal(signal);
	long film = run.first != VC_TIME_UNKNOWN && run.last != VC_TIME_UNKNOWN ? milliseconds(run.last - run.first) : -1;
	printf("packets=%lu pictures=%lu first_ms=%ld last_ms=%ld film_ms=%ld us=%lu\n", (unsigned long)run.packets,
	       (unsigned long)run.pictures, milliseconds(run.first), milliseconds(run.last), film, (unsigned long)took);
	printf("decode=%ld\n", (long)run.status);
	return run.status == 0 ? RETURN_OK : RETURN_FAIL;
}

/**
 * timer_open() - The timer device, for the E clock.
 * @request: the request to open it with
 *
 * Return: TRUE when open.
 */
static BOOL timer_open(struct timerequest *request)
{
	memset(request, 0, sizeof(*request));
	request->tr_node.io_Message.mn_Length = sizeof(*request);
	if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, &request->tr_node, 0) != 0)
		return FALSE;
	TimerBase = request->tr_node.io_Device;
	return TRUE;
}

/**
 * video_decode() - Decode one video stream of the film, timed by the E clock, and print what came out.
 * @source: the source
 * @args:   the template's
 *
 * Return: RETURN_OK, RETURN_FAIL without the timer, a stream, a decoder or a good decode.
 */
int video_decode(struct DMXSource *source, const LONG *args)
{
	struct timerequest timer;
	if (!timer_open(&timer))
	{
		printf("timer.device did not open\n");
		return RETURN_FAIL;
	}
	int result = stream_decode(source, args);
	CloseDevice(&timer.tr_node);
	TimerBase = NULL;
	return result;
}
