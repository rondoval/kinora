/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_lib.c - ffmpegaudio.codec, audiocodec.library's backend with FFmpeg's
 * audio decoders: its name, its function table and the calls' entry points.
 *
 * libnix's library start-up supplies the resident tag and the standard
 * vectors; ffmpeg/amiga_library.c brings libnix up and keeps FFmpeg to one
 * caller.  Each entry point checks its arguments, takes FFmpeg and passes to
 * codec_session.c.  If FFmpeg calls exit() or abort() the call fails with
 * ACERR_INTERNAL.
 */
#include <exec/libraries.h>

#include <amiga_library.h>

#include "codec_private.h"

/* What libnix's library start-up asks for */
const char LibName[] = "ffmpegaudio.codec";
const char LibIdString[] = "ffmpegaudio.codec " KINORA_VERSION_TEXT " " KINORA_DATE;
const UWORD LibVersion = KINORA_VERSION;
const UWORD LibRevision = KINORA_REVISION;

/* The version cookie the Version command finds */
static const char version_cookie[] __attribute__((used)) = "$VER: ffmpegaudio.codec " KINORA_VERSION_TEXT " " KINORA_DATE;

LONG ACB_GetInfo(struct ACBackendInfo *info asm("a0"));
LONG ACB_QueryCodec(ULONG index asm("d0"), struct ACCodecInfo *info asm("a0"));
APTR ACB_OpenSession(struct ACSessionParams *params asm("a0"), LONG *error asm("a1"));
VOID ACB_CloseSession(APTR session asm("a0"));
LONG ACB_SendPacket(APTR session asm("a0"), struct ACPacket *packet asm("a1"));
LONG ACB_ReceiveFrame(APTR session asm("a0"), struct ACFrame *frame asm("a1"));
LONG ACB_Flush(APTR session asm("a0"));

/* The library's functions in the order of sfd/audiocodec_backend_lib.sfd, after the standard vectors libnix's start-up lists first */
__attribute__((section(".list___FuncTable__"), used)) APTR codec_functions[] = {
    (APTR)ACB_GetInfo,    (APTR)ACB_QueryCodec,   (APTR)ACB_OpenSession, (APTR)ACB_CloseSession,
    (APTR)ACB_SendPacket, (APTR)ACB_ReceiveFrame, (APTR)ACB_Flush,       KINORA_PGO_DUMP_ENTRY (APTR)-1,
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
 * ACB_GetInfo() - Say what the backend is.
 * @info: filled in, as far as its Size reaches (A0)
 *
 * Return: 0, ACERR_BADPARAM for a structure too small.
 */
LONG ACB_GetInfo(struct ACBackendInfo *info asm("a0"))
{
	if (info == NULL || info->Size < sizeof(*info))
		return ACERR_BADPARAM;
	info->ABIVersion = ACB_ABI_VERSION;
	info->Codecs = table_rows();
	return 0;
}

/**
 * ACB_QueryCodec() - One row of the backend's codec table.
 * @index: the row, below ACBackendInfo.Codecs (D0)
 * @info:  filled in, as far as its Size reaches (A0)
 *
 * Return: 0, ACERR_BADPARAM.
 */
LONG ACB_QueryCodec(ULONG index asm("d0"), struct ACCodecInfo *info asm("a0"))
{
	if (info == NULL || info->Size <= sizeof(ULONG))
		return ACERR_BADPARAM;
	return table_row(index, info);
}

/**
 * ACB_OpenSession() - Open a decoder.
 * @params: the session's parsed tags (A0)
 * @error:  gets the ACERR_* of a refusal (A1)
 *
 * Return: the session, NULL when refused.
 */
APTR ACB_OpenSession(struct ACSessionParams *params asm("a0"), LONG *error asm("a1"))
{
	if (params == NULL || error == NULL || params->Size < sizeof(*params))
	{
		if (error != NULL)
			*error = ACERR_BADPARAM;
		return NULL;
	}
	struct session *volatile session = NULL; /* set after setjmp(), read after a longjmp() */
	volatile LONG reason = ACERR_INTERNAL;
	if (FFMPEG_ENTER())
	{
		LONG why = 0;
		session = session_open(params, &why);
		reason = why;
	}
	ffmpeg_leave();
	*error = session != NULL ? 0 : reason;
	return session;
}

/**
 * ACB_CloseSession() - Close a decoder.
 * @session: the session; NULL does nothing (A0)
 */
VOID ACB_CloseSession(APTR session asm("a0"))
{
	if (session == NULL)
		return;
	if (FFMPEG_ENTER())
		session_close(session);
	ffmpeg_leave();
}

/**
 * ACB_SendPacket() - Give the decoder one packet.
 * @session: the session (A0)
 * @packet:  the packet, checked by the frontend (A1)
 *
 * Return: 0 or an ACERR_*.
 */
LONG ACB_SendPacket(APTR session asm("a0"), struct ACPacket *packet asm("a1"))
{
	volatile LONG error = ACERR_INTERNAL; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
		error = session_send(session, packet);
	ffmpeg_leave();
	return error;
}

/**
 * ACB_ReceiveFrame() - Take the next decoded samples.
 * @session: the session (A0)
 * @frame:   filled in, checked by the frontend (A1)
 *
 * Return: 0 or an ACERR_*.
 */
LONG ACB_ReceiveFrame(APTR session asm("a0"), struct ACFrame *frame asm("a1"))
{
	volatile LONG error = ACERR_INTERNAL; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
		error = session_receive(session, frame);
	ffmpeg_leave();
	return error;
}

/**
 * ACB_Flush() - Drop what the decoder holds.
 * @session: the session (A0)
 *
 * Return: 0 or an ACERR_*.
 */
LONG ACB_Flush(APTR session asm("a0"))
{
	volatile LONG error = ACERR_INTERNAL; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
		error = session_flush(session);
	ffmpeg_leave();
	return error;
}
