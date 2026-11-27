/* SPDX-License-Identifier: MPL-2.0 */
/*
 * filminfo_file.c - FilmInfo film: the container as demuxer.library reads it, its streams and
 * chapters, and its packets listed or counted.
 */
#include <stdio.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/demuxer.h>

#include "filminfo.h"

#define STREAMS_COUNTED 32 /* the streams ALL keeps a count for */

static const char *const kind_names[] = {"?", "video", "audio", "subtitle", "other"};

/**
 * show_streams() - Print the file's streams.
 * @source: the source
 * @count:  how many it has
 */
static void show_streams(struct DMXSource *source, ULONG count)
{
	for (ULONG i = 0; i < count; i++)
	{
		struct DMXStream stream = {.Size = sizeof(stream)};
		if (DMX_GetStream(source, i, &stream) != 0)
			continue;
		const char *kind = kind_names[stream.Kind < sizeof(kind_names) / sizeof(kind_names[0]) ? stream.Kind : 0];
		printf("stream %lu: %s codec=%lu tag=%08lx flags=%lx language=%s extradata=%lu duration_ms=%ld", (unsigned long)i, kind,
		       (unsigned long)stream.Codec, (unsigned long)stream.Tag, (unsigned long)stream.Flags, stream.Language,
		       (unsigned long)stream.ExtradataSize, milliseconds(stream.Duration));
		if (stream.Kind == DMXK_Video)
			printf(" %lux%lu par=%ld/%ld rate=%ld/%ld colorspace=%lu", (unsigned long)stream.Width, (unsigned long)stream.Height,
			       (long)stream.Par.Num, (long)stream.Par.Den, (long)stream.FrameRate.Num, (long)stream.FrameRate.Den,
			       (unsigned long)stream.ColorSpace);
		if (stream.Kind == DMXK_Audio)
			printf(" rate=%lu channels=%lu bits=%lu pcm=%lx", (unsigned long)stream.SampleRate, (unsigned long)stream.Channels,
			       (unsigned long)stream.Bits, (unsigned long)stream.PcmFlags);
		printf(" title=%s\n", stream.Title);
	}
}

/**
 * show_chapters() - Print the file's chapters.
 * @source: the source
 * @count:  how many it has
 */
static void show_chapters(struct DMXSource *source, ULONG count)
{
	for (ULONG i = 0; i < count; i++)
	{
		struct DMXChapter chapter = {.Size = sizeof(chapter)};
		if (DMX_GetChapter(source, i, &chapter) == 0)
			printf("chapter %lu: start_ms=%ld end_ms=%ld title=%s\n", (unsigned long)i, milliseconds(chapter.Start),
			       milliseconds(chapter.End), chapter.Title);
	}
}

/**
 * read_packets() - Read packets and print them or count them.
 * @source: the source
 * @listed: how many to print, one a line
 * @all:    read to the file's end and print each stream's count
 *
 * Return: the DMXERR_* the reading ended with, DMXERR_END for the file's end, 0 when @listed were read.
 */
static LONG read_packets(struct DMXSource *source, ULONG listed, BOOL all)
{
	ULONG counts[STREAMS_COUNTED] = {0}, total = 0;
	LONG error = 0;
	while ((all || total < listed) && (SetSignal(0, 0) & SIGBREAKF_CTRL_C) == 0)
	{
		struct DMXPacket packet = {.Size = sizeof(packet)};
		error = DMX_ReadPacket(source, &packet);
		if (error != 0)
			break;
		if (total < listed)
			printf("packet %lu: stream=%lu bytes=%lu pts_ms=%ld dts_ms=%ld flags=%lx\n", (unsigned long)total,
			       (unsigned long)packet.Stream, (unsigned long)packet.Length, milliseconds(packet.Pts), milliseconds(packet.Dts),
			       (unsigned long)packet.Flags);
		if (packet.Stream < STREAMS_COUNTED)
			counts[packet.Stream]++;
		total++;
		DMX_ReleasePacket(source, &packet);
	}
	if (all)
		for (ULONG i = 0; i < STREAMS_COUNTED; i++)
			if (counts[i] != 0)
				printf("packets %lu = %lu\n", (unsigned long)i, (unsigned long)counts[i]);
	return error;
}

/**
 * file_show() - Print the container, its streams and chapters, and the packets the arguments ask for.
 * @source: the source
 * @args:   the template's: SEEK, PACKETS, ALL
 *
 * Return: RETURN_OK, or RETURN_FAIL when a call fails.
 */
int file_show(struct DMXSource *source, const LONG *args)
{
	struct DMXInfo info = {.Size = sizeof(info)};
	DMX_GetInfo(source, &info);
	printf("format=%s streams=%lu chapters=%lu duration_ms=%ld size=%08lx:%08lx seekable=%d\n", info.Format,
	       (unsigned long)info.Streams, (unsigned long)info.Chapters, milliseconds(info.Duration), (unsigned long)info.FileSizeHigh,
	       (unsigned long)info.FileSizeLow, info.Seekable != 0);
	show_streams(source, info.Streams);
	show_chapters(source, info.Chapters);

	LONG error = 0;
	if (args[ARG_SEEK] != 0)
	{
		VCTime time = (VCTime)number_of(args, ARG_SEEK) * MICROSECONDS_PER_MS;
		error = DMX_Seek(source, &time, DMXSEEK_Before);
		printf("seek=%ld\n", (long)error);
	}
	if (error == 0)
		error = read_packets(source, number_of(args, ARG_PACKETS), args[ARG_ALL] != 0);
	printf("read=%ld\n", (long)error);
	return error == 0 || error == DMXERR_END ? RETURN_OK : RETURN_FAIL;
}
