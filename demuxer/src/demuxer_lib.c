/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * demuxer_lib.c - demuxer.library: its name, its function table and the
 * calls' entry points.
 *
 * libnix's library start-up supplies the resident tag and the standard
 * vectors; ffmpeg/amiga_library.c brings libnix up and keeps FFmpeg to one
 * caller.  Each entry point checks its arguments, takes FFmpeg and passes to
 * demuxer_source.c.  If FFmpeg calls exit() or abort() the call fails with
 * DMXERR_INTERNAL.
 */
#include <exec/libraries.h>
#include <utility/tagitem.h>
#include <proto/utility.h>

#include <amiga_library.h>

#include "demuxer_private.h"

/* What libnix's library start-up asks for */
const char LibName[] = "demuxer.library";
const char LibIdString[] = "demuxer.library " KINORA_VERSION_TEXT " " KINORA_DATE;
const UWORD LibVersion = KINORA_VERSION;
const UWORD LibRevision = KINORA_REVISION;

/* The version cookie the Version command finds */
static const char version_cookie[] __attribute__((used)) = "$VER: demuxer.library " KINORA_VERSION_TEXT " " KINORA_DATE;

struct DMXSource *DMX_OpenTagList(struct TagItem *tags asm("a0"));
VOID DMX_Close(struct DMXSource *source asm("a0"));
LONG DMX_GetInfo(struct DMXSource *source asm("a0"), struct DMXInfo *info asm("a1"));
LONG DMX_GetStream(struct DMXSource *source asm("a0"), ULONG index asm("d0"), struct DMXStream *stream asm("a1"));
LONG DMX_SelectStreams(struct DMXSource *source asm("a0"), ULONG mask asm("d0"));
LONG DMX_ReadPacket(struct DMXSource *source asm("a0"), struct DMXPacket *packet asm("a1"));
VOID DMX_ReleasePacket(struct DMXSource *source asm("a0"), struct DMXPacket *packet asm("a1"));
LONG DMX_Seek(struct DMXSource *source asm("a0"), const VCTime *time asm("a1"), ULONG flags asm("d0"));
LONG DMX_GetChapter(struct DMXSource *source asm("a0"), ULONG index asm("d0"), struct DMXChapter *chapter asm("a1"));

/* The library's functions in the order of sfd/demuxer_lib.sfd, after the standard vectors libnix's start-up lists first */
__attribute__((section(".list___FuncTable__"), used)) APTR demuxer_functions[] = {
    (APTR)DMX_OpenTagList, (APTR)DMX_Close,         (APTR)DMX_GetInfo, (APTR)DMX_GetStream,  (APTR)DMX_SelectStreams,
    (APTR)DMX_ReadPacket,  (APTR)DMX_ReleasePacket, (APTR)DMX_Seek,    (APTR)DMX_GetChapter, (APTR)-1,
};

/**
 * log_drop() - FFmpeg's log callback: nothing is printed from a library.
 * @context: whose line it is
 * @level:   how grave
 * @format:  the line
 * @list:    its arguments
 */
static void log_drop(void *context, int level, const char *format, va_list list)
{
	(void)context;
	(void)level;
	(void)format;
	(void)list;
}

/**
 * library_open() - The library's own init, libnix up.
 *
 * Return: TRUE.
 */
BOOL library_open(void)
{
	av_log_set_callback(log_drop);
	return TRUE;
}

/**
 * library_close() - The library's own cleanup, before libnix goes down.
 */
void library_close(void)
{
}

/**
 * has_room() - Whether a caller's structure is large enough to answer into.
 * @structure: the structure, starting with its Size word; NULL for none given
 *
 * Return: TRUE when it holds more than the Size word.
 */
static BOOL has_room(CONST_APTR structure)
{
	return structure != NULL && *(const ULONG *)structure > sizeof(ULONG);
}

/**
 * DMX_OpenTagList() - Open a file.
 * @tags: DMXA_FileName, and DMXA_ErrorPtr for the reason of a refusal (A0)
 *
 * Return: the source, NULL when the file does not open or is no container the library reads.
 */
struct DMXSource *DMX_OpenTagList(struct TagItem *tags asm("a0"))
{
	CONST_STRPTR name = (CONST_STRPTR)GetTagData(DMXA_FileName, 0, tags);
	LONG *error_out = (LONG *)GetTagData(DMXA_ErrorPtr, 0, tags);
	volatile LONG error = name != NULL ? DMXERR_INTERNAL : DMXERR_BADPARAM; /* set after setjmp(), read after a longjmp() */
	struct DMXSource *volatile source = NULL;

	if (name != NULL)
	{
		if (FFMPEG_ENTER())
		{
			LONG reason = 0;
			source = source_open(name, &reason);
			error = reason;
		}
		ffmpeg_leave();
	}
	if (error_out != NULL)
		*error_out = source != NULL ? 0 : error;
	return source;
}

/**
 * DMX_Close() - Close a source.
 * @source: the source; NULL does nothing (A0)
 *
 * Packets still out are the caller's to release first.
 */
VOID DMX_Close(struct DMXSource *source asm("a0"))
{
	if (source == NULL)
		return;
	if (FFMPEG_ENTER())
		source_close(source);
	ffmpeg_leave();
}

/**
 * DMX_GetInfo() - Describe the file as a whole.
 * @source: the source (A0)
 * @info:   filled in, as far as its Size reaches (A1)
 *
 * Return: 0, DMXERR_BADPARAM or DMXERR_INTERNAL.
 */
LONG DMX_GetInfo(struct DMXSource *source asm("a0"), struct DMXInfo *info asm("a1"))
{
	if (source == NULL || !has_room(info))
		return DMXERR_BADPARAM;
	volatile LONG error = DMXERR_INTERNAL; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
		error = source_info(source, info);
	ffmpeg_leave();
	return error;
}

/**
 * DMX_GetStream() - Describe one stream.
 * @source: the source (A0)
 * @index:  the stream's number, below DMXInfo.Streams (D0)
 * @stream: filled in, as far as its Size reaches (A1)
 *
 * Return: 0, DMXERR_BADPARAM or DMXERR_INTERNAL.
 */
LONG DMX_GetStream(struct DMXSource *source asm("a0"), ULONG index asm("d0"), struct DMXStream *stream asm("a1"))
{
	if (source == NULL || !has_room(stream))
		return DMXERR_BADPARAM;
	volatile LONG error = DMXERR_INTERNAL; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
		error = source_stream(source, index, stream);
	ffmpeg_leave();
	return error;
}

/**
 * DMX_SelectStreams() - Choose the streams whose packets DMX_ReadPacket() gives.
 * @source: the source (A0)
 * @mask:   a bit a stream, bit 0 the first; a new source gives every video, audio and subtitle stream (D0)
 *
 * Return: 0, DMXERR_BADPARAM or DMXERR_INTERNAL.
 */
LONG DMX_SelectStreams(struct DMXSource *source asm("a0"), ULONG mask asm("d0"))
{
	if (source == NULL)
		return DMXERR_BADPARAM;
	volatile LONG error = DMXERR_INTERNAL; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
		error = source_select(source, mask);
	ffmpeg_leave();
	return error;
}

/**
 * DMX_ReadPacket() - Read the next packet of the selected streams, in file order.
 * @source: the source (A0)
 * @packet: filled in; its Data is the library's until DMX_ReleasePacket() (A1)
 *
 * Return: 0, DMXERR_END at the file's end, DMXERR_FILE, DMXERR_NOMEM, DMXERR_BADPARAM or DMXERR_INTERNAL.
 */
LONG DMX_ReadPacket(struct DMXSource *source asm("a0"), struct DMXPacket *packet asm("a1"))
{
	if (source == NULL || packet == NULL || packet->Size < sizeof(*packet))
		return DMXERR_BADPARAM;
	volatile LONG error = DMXERR_INTERNAL; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
		error = source_read(source, packet);
	ffmpeg_leave();
	return error;
}

/**
 * DMX_ReleasePacket() - Give a packet's memory back.
 * @source: the source it came from (A0)
 * @packet: the packet as DMX_ReadPacket() filled it; one released already is left alone (A1)
 */
VOID DMX_ReleasePacket(struct DMXSource *source asm("a0"), struct DMXPacket *packet asm("a1"))
{
	if (source == NULL || packet == NULL || packet->Size < sizeof(*packet) || packet->Private == NULL)
		return;
	if (FFMPEG_ENTER())
		source_release(packet);
	ffmpeg_leave();
}

/**
 * DMX_Seek() - Move to a key frame near a time.
 * @source: the source (A0)
 * @time:   the time (A1)
 * @flags:  DMXSEEK_Before or DMXSEEK_After (D0)
 *
 * The next packet's time says where the source went.  Packets read before
 * stay good until released.
 *
 * Return: 0, DMXERR_SEEK when the file or its handler cannot go there, DMXERR_BADPARAM or DMXERR_INTERNAL.
 */
LONG DMX_Seek(struct DMXSource *source asm("a0"), const VCTime *time asm("a1"), ULONG flags asm("d0"))
{
	if (source == NULL || time == NULL || flags > DMXSEEK_After)
		return DMXERR_BADPARAM;
	volatile LONG error = DMXERR_INTERNAL; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
		error = source_seek(source, *time, flags);
	ffmpeg_leave();
	return error;
}

/**
 * DMX_GetChapter() - Describe one chapter.
 * @source:  the source (A0)
 * @index:   the chapter's number, below DMXInfo.Chapters (D0)
 * @chapter: filled in, as far as its Size reaches (A1)
 *
 * Return: 0, DMXERR_BADPARAM or DMXERR_INTERNAL.
 */
LONG DMX_GetChapter(struct DMXSource *source asm("a0"), ULONG index asm("d0"), struct DMXChapter *chapter asm("a1"))
{
	if (source == NULL || !has_room(chapter))
		return DMXERR_BADPARAM;
	volatile LONG error = DMXERR_INTERNAL; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
		error = source_chapter(source, index, chapter);
	ffmpeg_leave();
	return error;
}
