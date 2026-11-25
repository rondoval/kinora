/* SPDX-License-Identifier: MPL-2.0 */
/*
 * DMXInfo - what demuxer.library makes of a file.
 *
 *   DMXInfo FILE/A,PACKETS/N,ALL/S,SEEK/K/N
 *
 * Prints the container, its streams and chapters.  PACKETS n lists the first
 * n packets, ALL reads the whole file and counts each stream's, SEEK seconds
 * moves there first.
 */
#include <stdio.h>
#include <string.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <exec/libraries.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/demuxer.h>

#define STREAMS_COUNTED 32       /* the streams ALL keeps a count for */
#define MICROSECONDS_PER_MS 1000 /* VCTime is microseconds; times are printed in milliseconds */
#define MS_PER_SECOND 1000

static const char version_cookie[] __attribute__((used)) = "$VER: DMXInfo " KINORA_VERSION_TEXT " " KINORA_DATE;
static const char template[] = "FILE/A,PACKETS/N,ALL/S,SEEK/K/N";

enum
{
	ARG_FILE,
	ARG_PACKETS,
	ARG_ALL,
	ARG_SEEK,
	ARG_COUNT
};

struct Library *DemuxerBase;

static const char *const kind_names[] = {"?", "video", "audio", "subtitle", "other"};

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
 * show() - Open the file and print what the arguments ask for.
 * @args: the template's
 *
 * Return: RETURN_OK, or RETURN_FAIL when the file does not open or a call fails.
 */
static int show(const LONG *args)
{
	LONG error = 0;
	struct DMXSource *source = DMX_OpenTags(DMXA_FileName, (ULONG)args[ARG_FILE], DMXA_ErrorPtr, (ULONG)&error, TAG_DONE);
	if (source == NULL)
	{
		printf("open failed: %ld\n", (long)error);
		return RETURN_FAIL;
	}
	struct DMXInfo info = {.Size = sizeof(info)};
	DMX_GetInfo(source, &info);
	printf("format=%s streams=%lu chapters=%lu duration_ms=%ld size=%08lx:%08lx seekable=%d\n", info.Format,
	       (unsigned long)info.Streams, (unsigned long)info.Chapters, milliseconds(info.Duration), (unsigned long)info.FileSizeHigh,
	       (unsigned long)info.FileSizeLow, info.Seekable != 0);
	show_streams(source, info.Streams);
	show_chapters(source, info.Chapters);

	if (args[ARG_SEEK] != 0)
	{
		VCTime time = (VCTime)*(LONG *)args[ARG_SEEK] * MS_PER_SECOND * MICROSECONDS_PER_MS;
		error = DMX_Seek(source, &time, DMXSEEK_Before);
		printf("seek=%ld\n", (long)error);
	}
	ULONG listed = args[ARG_PACKETS] != 0 ? (ULONG)*(LONG *)args[ARG_PACKETS] : 0;
	if (error == 0)
		error = read_packets(source, listed, args[ARG_ALL] != 0);
	printf("read=%ld\n", (long)error);
	DMX_Close(source);
	return error == 0 || error == DMXERR_END ? RETURN_OK : RETURN_FAIL;
}

/**
 * main() - Read the arguments, open demuxer.library and show the file.
 *
 * Return: RETURN_OK, or RETURN_FAIL for arguments that make no sense, no library or a file that fails.
 */
int main(void)
{
	LONG args[ARG_COUNT] = {0};
	struct RDArgs *parsed = ReadArgs((CONST_STRPTR)template, args, NULL);
	if (parsed == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR)"DMXInfo");
		return RETURN_FAIL;
	}
	int result = RETURN_FAIL;
	DemuxerBase = OpenLibrary((CONST_STRPTR)"demuxer.library", 0);
	if (DemuxerBase != NULL)
	{
		result = show(args);
		CloseLibrary(DemuxerBase);
	}
	else
	{
		printf("demuxer.library did not open\n");
	}
	FreeArgs(parsed);
	return result;
}
