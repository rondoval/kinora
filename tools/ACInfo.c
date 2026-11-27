/* SPDX-License-Identifier: MPL-2.0 */
/*
 * ACInfo - what audiocodec.library decodes, and a file's audio decoded through it.
 *
 *   ACInfo FILE,STREAM/K/N,CHANNELS/K/N,BITS/K/N,TO/K
 *
 * Without FILE it prints the codec table.  With FILE it reads the file with
 * demuxer.library, decodes one audio stream (STREAM, else the first) and
 * prints what came out.  CHANNELS asks for a mix-down, BITS for 16 or 32 bit
 * samples, TO names a file the raw samples are written to.
 */
#include <stdio.h>
#include <string.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/demuxer.h>
#include <proto/audiocodec.h>

#define MICROSECONDS_PER_MS 1000 /* VCTime is microseconds; times are printed in milliseconds */
#define BITS_LONG 32             /* BITS for ACF_S32 */

static const char version_cookie[] __attribute__((used)) = "$VER: ACInfo " KINORA_VERSION_TEXT " " KINORA_DATE;
static const char template[] = "FILE,STREAM/K/N,CHANNELS/K/N,BITS/K/N,TO/K";

enum
{
	ARG_FILE,
	ARG_STREAM,
	ARG_CHANNELS,
	ARG_BITS,
	ARG_TO,
	ARG_COUNT
};

struct Library *DemuxerBase;
struct Library *AudioCodecBase;

/* What a decode run counts */
struct totals
{
	ULONG packets;    /* packets sent */
	ULONG frames;     /* frames received */
	ULONG samples;    /* their sample frames */
	ULONG bad;        /* packets the decoder could not read */
	VCTime first;     /* the first frame's time */
	struct ACFrame last; /* the last frame, for its rate and channels */
	BPTR output;      /* where the samples go, 0 for nowhere */
};

/**
 * milliseconds() - A time for printing.
 * @time: the time
 *
 * Return: its milliseconds, -1 for VC_TIME_UNKNOWN.
 */
static long milliseconds(VCTime time)
{
	return time == VC_TIME_UNKNOWN ? -1L : (long)(time / MICROSECONDS_PER_MS);
}

/**
 * show_codecs() - Print the codec table.
 *
 * Return: RETURN_OK, RETURN_WARN for an empty table.
 */
static int show_codecs(void)
{
	ULONG rows = 0;
	struct ACCodecInfo info = {.Size = sizeof(info)};
	while (AC_QueryCodec(rows, &info) == 0)
	{
		printf("codec %lu: %s priority=%ld backend=%s\n", (unsigned long)info.Codec, info.Name, (long)info.Priority, info.Backend);
		info.Size = sizeof(info);
		rows++;
	}
	printf("codecs=%lu\n", (unsigned long)rows);
	return rows != 0 ? RETURN_OK : RETURN_WARN;
}

/**
 * audio_stream() - Find the stream to decode.
 * @source: the source
 * @wanted: the stream's number, -1 for the first audio stream
 * @stream: filled in
 *
 * Return: the stream's number, -1 when the file has no such audio stream.
 */
static LONG audio_stream(struct DMXSource *source, LONG wanted, struct DMXStream *stream)
{
	struct DMXInfo info = {.Size = sizeof(info)};
	DMX_GetInfo(source, &info);
	for (ULONG i = 0; i < info.Streams; i++)
	{
		stream->Size = sizeof(*stream);
		if (DMX_GetStream(source, i, stream) == 0 && stream->Kind == DMXK_Audio && (wanted < 0 || wanted == (LONG)i))
			return (LONG)i;
	}
	return -1;
}

/**
 * receive_frames() - Take every frame the decoder has and count it.
 * @session: the session
 * @totals:  the run's counts
 *
 * Return: ACERR_AGAIN or ACERR_END when the decoder has no more, else the error it ended with.
 */
static LONG receive_frames(struct ACSession *session, struct totals *totals)
{
	for (;;)
	{
		struct ACFrame frame = {.Size = sizeof(frame)};
		LONG error = AC_ReceiveFrame(session, &frame);
		if (error != 0)
			return error;
		if (totals->frames == 0)
			totals->first = frame.Pts;
		totals->frames++;
		totals->samples += frame.Samples;
		totals->last = frame;
		ULONG bytes = frame.Samples * frame.Channels * (frame.Format == ACF_S32 ? sizeof(LONG) : sizeof(WORD));
		if (totals->output != 0 && Write(totals->output, frame.Data, (LONG)bytes) != (LONG)bytes)
			return ACERR_INTERNAL;
	}
}

/**
 * decode_packets() - Send the stream's packets to the decoder, then its end.
 * @source:  the source, the stream selected
 * @session: the session
 * @totals:  the run's counts
 *
 * Return: ACERR_END when everything was decoded, else the error that stopped it.
 */
static LONG decode_packets(struct DMXSource *source, struct ACSession *session, struct totals *totals)
{
	LONG error = ACERR_AGAIN;
	while (error == ACERR_AGAIN && (SetSignal(0, 0) & SIGBREAKF_CTRL_C) == 0)
	{
		struct DMXPacket read = {.Size = sizeof(read)};
		struct ACPacket packet = {.Size = sizeof(packet)}; /* Length 0, the end, unless a packet was read */
		BOOL have = DMX_ReadPacket(source, &read) == 0;
		if (have)
		{
			packet.Data = read.Data;
			packet.Length = read.Length;
			packet.Pts = read.Pts;
			totals->packets++;
		}
		error = AC_SendPacket(session, &packet);
		if (have)
			DMX_ReleasePacket(source, &read);
		if (error == ACERR_DATA)
			totals->bad++;
		if (error == 0 || error == ACERR_DATA)
			error = receive_frames(session, totals);
	}
	return error;
}

/**
 * decode_stream() - Decode one audio stream of the source and print the result.
 * @source: the source
 * @args:   the template's
 *
 * Return: RETURN_OK, RETURN_FAIL when there is no stream, no decoder or the decoding fails.
 */
static int decode_stream(struct DMXSource *source, const LONG *args)
{
	struct DMXStream stream;
	LONG number = audio_stream(source, args[ARG_STREAM] != 0 ? *(LONG *)args[ARG_STREAM] : -1, &stream);
	if (number < 0)
	{
		printf("no audio stream\n");
		return RETURN_FAIL;
	}
	DMX_SelectStreams(source, 1UL << number);

	LONG error = 0;
	char backend[AC_BACKEND_NAME_SIZE] = "";
	ULONG format = args[ARG_BITS] != 0 && *(LONG *)args[ARG_BITS] == BITS_LONG ? ACF_S32 : ACF_S16;
	struct ACSession *session =
	    AC_OpenSessionTags(ACA_Codec, stream.Codec, ACA_Extradata, (ULONG)stream.Extradata, ACA_ExtradataSize, stream.ExtradataSize,
	                       ACA_SampleRate, stream.SampleRate, ACA_Channels, stream.Channels, ACA_Bits, stream.Bits, ACA_PcmFlags,
	                       stream.PcmFlags, ACA_OutputFormat, format, ACA_OutputChannels,
	                       args[ARG_CHANNELS] != 0 ? (ULONG) * (LONG *)args[ARG_CHANNELS] : 0, ACA_ErrorPtr, (ULONG)&error, ACA_BackendName,
	                       (ULONG)backend, TAG_DONE);
	printf("stream=%ld codec=%lu open=%ld backend=%s\n", (long)number, (unsigned long)stream.Codec, (long)error, backend);
	if (session == NULL)
		return RETURN_FAIL;

	struct totals totals = {.first = VC_TIME_UNKNOWN};
	if (args[ARG_TO] != 0)
		totals.output = Open((CONST_STRPTR)args[ARG_TO], MODE_NEWFILE);
	error = decode_packets(source, session, &totals);
	if (totals.output != 0)
		Close(totals.output);
	AC_CloseSession(session);

	printf("packets=%lu bad=%lu frames=%lu samples=%lu first_ms=%ld\n", (unsigned long)totals.packets, (unsigned long)totals.bad,
	       (unsigned long)totals.frames, (unsigned long)totals.samples, milliseconds(totals.first));
	printf("rate=%lu channels=%lu layout=%lx format=%lu\n", (unsigned long)totals.last.SampleRate, (unsigned long)totals.last.Channels,
	       (unsigned long)totals.last.Layout, (unsigned long)totals.last.Format);
	printf("decode=%ld\n", (long)error);
	return error == ACERR_END ? RETURN_OK : RETURN_FAIL;
}

/**
 * decode_file() - Open the file with demuxer.library and decode its audio.
 * @args: the template's
 *
 * Return: RETURN_OK, or RETURN_FAIL without the library, the file or a good decode.
 */
static int decode_file(const LONG *args)
{
	int result = RETURN_FAIL;
	DemuxerBase = OpenLibrary((CONST_STRPTR) "demuxer.library", 0);
	if (DemuxerBase == NULL)
	{
		printf("demuxer.library did not open\n");
		return result;
	}
	LONG error = 0;
	struct DMXSource *source = DMX_OpenTags(DMXA_FileName, (ULONG)args[ARG_FILE], DMXA_ErrorPtr, (ULONG)&error, TAG_DONE);
	if (source != NULL)
	{
		result = decode_stream(source, args);
		DMX_Close(source);
	}
	else
	{
		printf("open failed: %ld\n", (long)error);
	}
	CloseLibrary(DemuxerBase);
	return result;
}

/**
 * main() - Read the arguments, open audiocodec.library and do what they ask.
 *
 * Return: RETURN_OK, or RETURN_FAIL for arguments that make no sense, no library or a failed decode.
 */
int main(void)
{
	LONG args[ARG_COUNT] = {0};
	struct RDArgs *parsed = ReadArgs((CONST_STRPTR)template, args, NULL);
	if (parsed == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "ACInfo");
		return RETURN_FAIL;
	}
	int result = RETURN_FAIL;
	AudioCodecBase = OpenLibrary((CONST_STRPTR)AUDIOCODEC_NAME, 0);
	if (AudioCodecBase != NULL)
	{
		result = args[ARG_FILE] != 0 ? decode_file(args) : show_codecs();
		CloseLibrary(AudioCodecBase);
	}
	else
	{
		printf("audiocodec.library did not open\n");
	}
	FreeArgs(parsed);
	return result;
}
