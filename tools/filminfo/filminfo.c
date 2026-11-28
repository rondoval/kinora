/* SPDX-License-Identifier: MPL-2.0 */
/*
 * FilmInfo - what Kinora's libraries make of a film, and what they decode.
 *
 *   FilmInfo FILE,AUDIO/S,VIDEO/S,STREAM/K/N,PACKETS/K/N,ALL/S,SEEK/K/N,CHANNELS/K/N,BITS/K/N,TO/K,
 *            MD5/S,FORMAT/K,WIDTH/K/N,HEIGHT/K/N,SKIP/K/N,BACKEND/K,PRI/K/N
 *
 *   FilmInfo             the codecs: audiocodec.library's table and the video codec drivers'
 *                        decoder rows (VCInfo CODECS lists all of theirs, for people)
 *   FilmInfo film        the container as demuxer.library reads it, its streams and chapters;
 *                        PACKETS n lists the first n packets, ALL reads the whole file and
 *                        counts each stream's, SEEK ms moves there first
 *   FilmInfo film AUDIO  its sound decoded (STREAM n, else the first stream): CHANNELS n asks
 *                        for a mix-down, BITS 16 or 32 for the samples, TO a file for them
 *   FilmInfo film VIDEO  its video decoded as fast as it goes, a picture a packet as Kinora
 *                        sends them (STREAM n, else the first): MD5 prints each picture's
 *                        digest (I420: its visible luma rows, then each chroma plane's, as
 *                        FFmpeg's framemd5 hashes them), FORMAT asks for another format
 *                        (BGRA32, RGBA32, RGB16, RGB24) and WIDTH and HEIGHT for another size,
 *                        which puts a converter behind the decoder, SKIP n has the decoder
 *                        leave pictures out (VCA_Skip: 1 those nothing refers to, 2 all but
 *                        the key frames), SEEK ms moves there after the first 50 pictures
 *
 * A decode goes to the stream's end; PACKETS n stops it after the stream's first n packets
 * (a whole film decodes as long as it lasts, or longer, on the 68k).  Ctrl-C ends it too,
 * with what came out so far printed.  BACKEND names the codec driver to decode with (ACA_Backend, VCA_Backend); PRI runs at that
 * priority.  Each line is name=value pairs, some behind a word and a number ("stream 0:"):
 * what the tests read, and what a report of a film that does not play needs.
 */
#include <stdio.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/demuxer.h>
#include <libraries/audiocodec.h>
#include <libraries/videocodec.h>

#include "filminfo.h"

static const char version_cookie[] __attribute__((used)) = "$VER: FilmInfo " KINORA_VERSION_TEXT " " KINORA_DATE;
static const char template[] = "FILE,AUDIO/S,VIDEO/S,STREAM/K/N,PACKETS/K/N,ALL/S,SEEK/K/N,CHANNELS/K/N,BITS/K/N,TO/K,"
                               "MD5/S,FORMAT/K,WIDTH/K/N,HEIGHT/K/N,SKIP/K/N,BACKEND/K,PRI/K/N";

struct Library *DemuxerBase;
struct Library *AudioCodecBase;
struct Library *VideoCodecBase;

/* What decodes one kind of stream */
struct decoding
{
	const char *library;                                     /* the library it goes through */
	struct Library **base;                                   /* where its base is kept */
	int (*decode)(struct DMXSource *source, const LONG *args); /* the decode */
};

static const struct decoding sound = {AUDIOCODEC_NAME, &AudioCodecBase, audio_decode};
static const struct decoding picture = {VIDEOCODEC_NAME, &VideoCodecBase, video_decode};

/**
 * milliseconds() - A time for printing.
 * @time: the time
 *
 * Return: its milliseconds, -1 for VC_TIME_UNKNOWN.
 */
long milliseconds(VCTime time)
{
	return time == VC_TIME_UNKNOWN ? -1L : (long)(time / MICROSECONDS_PER_MS);
}

/**
 * number_of() - A numeric argument's value.
 * @args:  the template's
 * @index: ARG_*
 *
 * Return: the number, 0 when it was not given.
 */
ULONG number_of(const LONG *args, int index)
{
	return args[index] != 0 ? (ULONG) * (const LONG *)args[index] : 0;
}

/**
 * stream_find() - Find the stream to decode: the one STREAM names, else the file's first of the kind.
 * @source: the source
 * @kind:   DMXK_Audio or DMXK_Video
 * @args:   the template's
 * @stream: filled in
 *
 * Return: the stream's number, -1 when the file has no such stream.
 */
LONG stream_find(struct DMXSource *source, ULONG kind, const LONG *args, struct DMXStream *stream)
{
	LONG wanted = args[ARG_STREAM] != 0 ? *(const LONG *)args[ARG_STREAM] : -1;
	struct DMXInfo info = {.Size = sizeof(info)};
	DMX_GetInfo(source, &info);
	for (ULONG i = 0; i < info.Streams; i++)
	{
		stream->Size = sizeof(*stream);
		if (DMX_GetStream(source, i, stream) == 0 && stream->Kind == kind && (wanted < 0 || wanted == (LONG)i))
			return (LONG)i;
	}
	return -1;
}

/**
 * library_open() - Open a library, and say so when it does not open.
 * @name: its name
 *
 * Return: its base, NULL when it did not open.
 */
static struct Library *library_open(const char *name)
{
	struct Library *base = OpenLibrary((CONST_STRPTR)name, 0);
	if (base == NULL)
		printf("%s did not open\n", name);
	return base;
}

/**
 * library_close() - Close a library opened with library_open().
 * @base: where its base is kept; NULL there does nothing, and NULL is left there
 */
static void library_close(struct Library **base)
{
	if (*base != NULL)
		CloseLibrary(*base);
	*base = NULL;
}

/**
 * codecs_show() - Print both codec tables: audiocodec.library's and the video decoders'.
 *
 * Return: RETURN_OK; the worse of RETURN_WARN for an empty table and RETURN_FAIL for a library that did not open.
 */
static int codecs_show(void)
{
	AudioCodecBase = library_open(AUDIOCODEC_NAME);
	VideoCodecBase = library_open(VIDEOCODEC_NAME);
	int audio = AudioCodecBase != NULL ? audio_codecs_show() : RETURN_FAIL;
	int video = VideoCodecBase != NULL ? video_codecs_show() : RETURN_FAIL;
	library_close(&AudioCodecBase);
	library_close(&VideoCodecBase);
	return audio > video ? audio : video;
}

/**
 * stream_decode() - Decode one stream of the file through the library that decodes its kind.
 * @source:   the source
 * @args:     the template's
 * @decoding: the library and the decode
 *
 * Return: the decode's result, RETURN_FAIL without the library.
 */
static int stream_decode(struct DMXSource *source, const LONG *args, const struct decoding *decoding)
{
	*decoding->base = library_open(decoding->library);
	int result = *decoding->base != NULL ? decoding->decode(source, args) : RETURN_FAIL;
	library_close(decoding->base);
	return result;
}

/**
 * film_run() - Open the film with demuxer.library and show it, or decode its sound or video.
 * @args: the template's
 *
 * Return: RETURN_OK, or RETURN_FAIL without the library, the file, or a good decode.
 */
static int film_run(const LONG *args)
{
	DemuxerBase = library_open("demuxer.library");
	if (DemuxerBase == NULL)
		return RETURN_FAIL;
	int result = RETURN_FAIL;
	LONG error = 0;
	struct DMXSource *source = DMX_OpenTags(DMXA_FileName, (ULONG)args[ARG_FILE], DMXA_ErrorPtr, (ULONG)&error, TAG_DONE);
	if (source == NULL)
		printf("open failed: %ld\n", (long)error);
	else if (args[ARG_AUDIO] != 0)
		result = stream_decode(source, args, &sound);
	else if (args[ARG_VIDEO] != 0)
		result = stream_decode(source, args, &picture);
	else
		result = file_show(source, args);
	if (source != NULL)
		DMX_Close(source);
	library_close(&DemuxerBase);
	return result;
}

/**
 * run_at_priority() - Do what the arguments ask, at PRI's priority if given.
 * @args: the template's
 *
 * Return: the run's result.
 */
static int run_at_priority(const LONG *args)
{
	struct Task *me = FindTask(NULL);
	BYTE priority = me->tc_Node.ln_Pri;
	if (args[ARG_PRI] != 0)
		SetTaskPri(me, *(const LONG *)args[ARG_PRI]);
	int result = args[ARG_FILE] != 0 ? film_run(args) : codecs_show();
	SetTaskPri(me, priority);
	return result;
}

/**
 * __chkabort() - libnix's Ctrl-C check in its stdio, made to do nothing.
 *
 * FilmInfo looks for Ctrl-C itself and ends a decode with its session closed and what came
 * out printed; libnix's own would end the program inside a printf(), the session left open
 * and the lines lost.
 */
void __chkabort(void)
{
}

/**
 * main() - Read the arguments and do what they ask.
 *
 * Return: RETURN_OK, or RETURN_FAIL for arguments that make no sense, no library, a file that fails or a failed decode.
 */
int main(void)
{
	LONG args[ARG_COUNT] = {0};
	struct RDArgs *parsed = ReadArgs((CONST_STRPTR)template, args, NULL);
	if (parsed == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "FilmInfo");
		return RETURN_FAIL;
	}
	int result = RETURN_FAIL;
	if (args[ARG_AUDIO] != 0 && args[ARG_VIDEO] != 0)
		printf("AUDIO or VIDEO: one at a time\n");
	else if (!video_format_known(args))
		printf("FORMAT is one of I420, BGRA32, RGBA32, RGB16, RGB24\n");
	else
		result = run_at_priority(args);
	FreeArgs(parsed);
	return result;
}
