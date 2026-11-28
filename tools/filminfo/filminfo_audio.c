/* SPDX-License-Identifier: MPL-2.0 */
/*
 * filminfo_audio.c - FilmInfo's sound: audiocodec.library's codec table, and a film's audio
 * stream decoded through it (FilmInfo film AUDIO).
 */
#include <stdio.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/demuxer.h>
#include <proto/audiocodec.h>

#include "filminfo.h"

#define BITS_LONG 32 /* BITS for ACF_S32 */

/* What a decode run counts */
struct totals
{
	ULONG packets;       /* packets sent */
	ULONG frames;        /* frames received */
	ULONG samples;       /* their sample frames */
	ULONG bad;           /* packets the decoder could not read */
	VCTime first;        /* the first frame's time */
	struct ACFrame last; /* the last frame, for its rate and channels */
	BPTR output;         /* where the samples go, 0 for nowhere */
};

/**
 * audio_codecs_show() - Print audiocodec.library's codec table.
 *
 * Return: RETURN_OK, RETURN_WARN for an empty table.
 */
int audio_codecs_show(void)
{
	ULONG rows = 0;
	struct ACCodecInfo info = {.Size = sizeof(info)};
	while (AC_QueryCodec(rows, &info) == 0)
	{
		printf("audio codec %lu: %s priority=%ld backend=%s\n", (unsigned long)info.Codec, info.Name, (long)info.Priority,
		       info.Backend);
		info.Size = sizeof(info);
		rows++;
	}
	printf("audio_codecs=%lu\n", (unsigned long)rows);
	return rows != 0 ? RETURN_OK : RETURN_WARN;
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
 * @most:    PACKETS: the end after this many, 0 for the stream's end
 * @totals:  the run's counts
 *
 * Return: ACERR_END when everything was decoded, else the error that stopped it.
 */
static LONG decode_packets(struct DMXSource *source, struct ACSession *session, ULONG most, struct totals *totals)
{
	LONG error = ACERR_AGAIN;
	while (error == ACERR_AGAIN && (SetSignal(0, 0) & SIGBREAKF_CTRL_C) == 0)
	{
		struct DMXPacket read = {.Size = sizeof(read)};
		struct ACPacket packet = {.Size = sizeof(packet)}; /* Length 0, the end, unless a packet was read */
		BOOL have = (most == 0 || totals->packets < most) && DMX_ReadPacket(source, &read) == 0;
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
 * session_open() - Open the decoder for the stream, as Kinora's engine opens one, with what the arguments ask.
 * @stream:  the stream
 * @args:    the template's: CHANNELS, BITS, BACKEND
 * @error:   gets the ACERR_* of a refusal
 * @backend: gets the name of the backend the session went to, AC_BACKEND_NAME_SIZE bytes
 *
 * Return: the session, NULL when refused.
 */
static struct ACSession *session_open(const struct DMXStream *stream, const LONG *args, LONG *error, char *backend)
{
	ULONG format = number_of(args, ARG_BITS) == BITS_LONG ? ACF_S32 : ACF_S16;
	struct TagItem tags[] = {
	    {ACA_Codec, stream->Codec},
	    {ACA_Extradata, (ULONG)stream->Extradata},
	    {ACA_ExtradataSize, stream->ExtradataSize},
	    {ACA_SampleRate, stream->SampleRate},
	    {ACA_Channels, stream->Channels},
	    {ACA_Bits, stream->Bits},
	    {ACA_PcmFlags, stream->PcmFlags},
	    {ACA_OutputFormat, format},
	    {ACA_OutputChannels, number_of(args, ARG_CHANNELS)},
	    {ACA_BlockAlign, stream->BlockAlign},
	    {args[ARG_BACKEND] != 0 ? ACA_Backend : TAG_IGNORE, (ULONG)args[ARG_BACKEND]},
	    {ACA_ErrorPtr, (ULONG)error},
	    {ACA_BackendName, (ULONG)backend},
	    {TAG_DONE, 0},
	};
	return AC_OpenSessionTagList(tags);
}

/**
 * audio_decode() - Decode one audio stream of the film and print what came out.
 * @source: the source
 * @args:   the template's
 *
 * Return: RETURN_OK, RETURN_FAIL when there is no stream, no decoder or the decoding fails.
 */
int audio_decode(struct DMXSource *source, const LONG *args)
{
	struct DMXStream stream;
	LONG number = stream_find(source, DMXK_Audio, args, &stream);
	if (number < 0)
	{
		printf("no audio stream\n");
		return RETURN_FAIL;
	}
	DMX_SelectStreams(source, 1UL << number);

	LONG error = 0;
	char backend[AC_BACKEND_NAME_SIZE] = "";
	struct ACSession *session = session_open(&stream, args, &error, backend);
	printf("stream=%ld codec=%lu open=%ld backend=%s\n", (long)number, (unsigned long)stream.Codec, (long)error, backend);
	if (session == NULL)
		return RETURN_FAIL;

	struct totals totals = {.first = VC_TIME_UNKNOWN};
	if (args[ARG_TO] != 0)
		totals.output = Open((CONST_STRPTR)args[ARG_TO], MODE_NEWFILE);
	error = decode_packets(source, session, number_of(args, ARG_PACKETS), &totals);
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
