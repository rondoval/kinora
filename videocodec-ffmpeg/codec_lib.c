/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_lib.c - ffmpegvideo.codec, videocodec.library's backend with FFmpeg's video
 * decoders (docs/architecture.md, section 14): its name, its function table and the calls'
 * entry points.
 *
 * libnix's library start-up supplies the resident tag and the standard vectors;
 * ffmpeg/amiga_library.c brings libnix up and keeps FFmpeg to one caller.  The entry points
 * check their arguments and pass to codec_session.c.  The backend decodes for sessions only:
 * no image calls, no imported objects, nothing to let go of when idle.
 */
#include <string.h>

#include <exec/libraries.h>

#include <amiga_library.h>
#include <libavutil/avstring.h>
#include <libavutil/log.h>

#include "codec_private.h"

#define COUNTERS 3 /* pictures, damaged, waits */

/* What libnix's library start-up asks for */
const char LibName[] = BACKEND_NAME;
const char LibIdString[] = BACKEND_NAME " " KINORA_VERSION_TEXT " " KINORA_DATE;
const UWORD LibVersion = KINORA_VERSION;
const UWORD LibRevision = KINORA_REVISION;

/* The version cookie the Version command finds */
static const char version_cookie[] __attribute__((used)) = "$VER: " BACKEND_NAME " " KINORA_VERSION_TEXT " " KINORA_DATE;

LONG VCB_GetInfo(struct VCInfo *info asm("a0"));
LONG VCB_QueryCodec(ULONG index asm("d0"), struct VCCodecInfo *info asm("a0"));
APTR VCB_OpenSession(struct VCSessionParams *params asm("a0"), LONG *error asm("a1"));
VOID VCB_CloseSession(APTR session asm("a0"));
struct VCPacket *VCB_ImportPacket(APTR session asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), ULONG capacity asm("d1"));
struct VCFrame *VCB_ImportFrame(APTR session asm("a0"), ULONG side asm("d0"), APTR data asm("a1"));
LONG VCB_SendPacket(APTR session asm("a0"), struct VCPacket *packet asm("a1"));
LONG VCB_SendFrame(APTR session asm("a0"), struct VCFrame *frame asm("a1"));
LONG VCB_Poll(APTR session asm("a0"), struct VCItem *item asm("a1"));
VOID VCB_ReleaseFrame(APTR session asm("a0"), struct VCFrame *frame asm("a1"));
VOID VCB_ReleasePacket(APTR session asm("a0"), struct VCPacket *packet asm("a1"));
LONG VCB_Flush(APTR session asm("a0"));
LONG VCB_SetAttrs(APTR session asm("a0"), struct VCSessionParams *params asm("a1"));
LONG VCB_Image(struct VCImageParams *params asm("a0"));
VOID VCB_Idle(void);
struct VCFrame *VCB_OutputFrame(APTR session asm("a0"), ULONG index asm("d0"));

/* The library's functions in the order of videocodec_backend.sfd, after the standard vectors libnix's start-up lists first */
__attribute__((section(".list___FuncTable__"), used)) APTR codec_functions[] = {
    (APTR)VCB_GetInfo,      (APTR)VCB_QueryCodec,    (APTR)VCB_OpenSession, (APTR)VCB_CloseSession,
    (APTR)VCB_ImportPacket, (APTR)VCB_ImportFrame,   (APTR)VCB_SendPacket,  (APTR)VCB_SendFrame,
    (APTR)VCB_Poll,         (APTR)VCB_ReleaseFrame,  (APTR)VCB_ReleasePacket, (APTR)VCB_Flush,
    (APTR)VCB_SetAttrs,     (APTR)VCB_Image,         (APTR)VCB_Idle,        (APTR)VCB_OutputFrame,
    KINORA_PGO_DUMP_ENTRY (APTR)-1,
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
	sessions_init();
	memory_open();
	return TRUE;
}

/**
 * library_close() - The library's own cleanup, before libnix goes down: the frontend closed every session.
 */
void library_close(void)
{
	memory_close();
}

/**
 * counter_set() - One of the backend's counters, where the caller has room for it.
 * @info:  the caller's structure
 * @index: the counter's place
 * @name:  its name
 * @value: its value
 */
static void counter_set(struct VCInfo *info, ULONG index, const char *name, ULONG value)
{
	if (info->Counters == NULL || index >= info->MaxCounters)
		return;
	av_strlcpy((char *)info->Counters[index].Name, name, sizeof(info->Counters[index].Name));
	info->Counters[index].Value = value;
}

/**
 * VCB_GetInfo() - Say what the backend is: its ABI, its sessions, FFmpeg's version, its counters.
 * @info: filled in, as far as its Size reaches (A0)
 *
 * The counters: pictures handed out, packets FFmpeg could not decode, and how often a
 * session's process waited for a free frame.
 *
 * Return: 0, VCERR_BADPARAM for a structure too small.
 */
LONG VCB_GetInfo(struct VCInfo *info asm("a0"))
{
	if (info == NULL || !VC_HAS_FIELD(info->Size, struct VCInfo, Codecs))
		return VCERR_BADPARAM;
	info->ABIVersion = VCB_ABI_VERSION;
	av_strlcpy((char *)info->Backend, BACKEND_NAME, sizeof(info->Backend));
	info->Sessions = session_count();
	av_strlcpy((char *)info->Interface, "FFmpeg ", sizeof(info->Interface));
	av_strlcat((char *)info->Interface, av_version_info(), sizeof(info->Interface));
	struct counts counts;
	session_counts(&counts);
	counter_set(info, 0, "pictures", counts.pictures);
	counter_set(info, 1, "damaged", counts.damaged);
	counter_set(info, 2, "waits", counts.waits);
	info->NumCounters = COUNTERS;
	info->Codecs = table_rows();
	if (VC_HAS_FIELD(info->Size, struct VCInfo, MemFree))
	{
		info->MemTotal = 0; /* the 68k's own memory: no hardware's to report */
		info->MemFree = 0;
	}
	return 0;
}

/**
 * VCB_QueryCodec() - One row of the backend's codec table.
 * @index: the row, below VCInfo.Codecs (D0)
 * @info:  filled in, as far as its Size reaches (A0)
 *
 * Return: 0, VCERR_BADPARAM.
 */
LONG VCB_QueryCodec(ULONG index asm("d0"), struct VCCodecInfo *info asm("a0"))
{
	if (info == NULL || info->Size <= sizeof(ULONG))
		return VCERR_BADPARAM;
	return table_row(index, info);
}

/**
 * VCB_OpenSession() - Open a decoder.
 * @params: the session's parsed tags (A0)
 * @error:  gets the VCERR_* of a refusal (A1)
 *
 * Return: the session, NULL when refused.
 */
APTR VCB_OpenSession(struct VCSessionParams *params asm("a0"), LONG *error asm("a1"))
{
	LONG ignored;
	if (error == NULL)
		error = &ignored;
	if (params == NULL || !VC_HAS_FIELD(params->Size, struct VCSessionParams, CodecTag))
	{
		*error = VCERR_BADPARAM;
		return NULL;
	}
	return session_open(params, error);
}

/**
 * VCB_CloseSession() - Close a decoder; its frames and packets go with it.
 * @session: the session; NULL does nothing (A0)
 */
VOID VCB_CloseSession(APTR session asm("a0"))
{
	if (session != NULL)
		session_close(session);
}

/**
 * VCB_ImportPacket() - Import the client's memory as a packet: not done here.
 * @session:  the session (A0)
 * @side:     VCS_* (D0)
 * @data:     the memory (A1)
 * @capacity: its bytes (D1)
 *
 * Return: NULL: the session's own packets serve.
 */
struct VCPacket *VCB_ImportPacket(APTR session asm("a0"), ULONG side asm("d0"), APTR data asm("a1"), ULONG capacity asm("d1"))
{
	(void)session;
	(void)side;
	(void)data;
	(void)capacity;
	return NULL;
}

/**
 * VCB_ImportFrame() - Import the client's memory as a frame: not done here.
 * @session: the session (A0)
 * @side:    VCS_* (D0)
 * @data:    the memory (A1)
 *
 * Return: NULL: FFmpeg decodes into the session's own frames, which a converter imports.
 */
struct VCFrame *VCB_ImportFrame(APTR session asm("a0"), ULONG side asm("d0"), APTR data asm("a1"))
{
	(void)session;
	(void)side;
	(void)data;
	return NULL;
}

/**
 * VCB_SendPacket() - Give the decoder a packet.
 * @session: the session (A0)
 * @packet:  one of its input packets, filled (A1)
 *
 * Return: 0 or a VCERR_*.
 */
LONG VCB_SendPacket(APTR session asm("a0"), struct VCPacket *packet asm("a1"))
{
	if (session == NULL || packet == NULL)
		return VCERR_BADPARAM;
	return session_send(session, packet);
}

/**
 * VCB_SendFrame() - A frame into a decoder: refused.
 * @session: the session (A0)
 * @frame:   the frame (A1)
 *
 * Return: VCERR_BADSTATE.
 */
LONG VCB_SendFrame(APTR session asm("a0"), struct VCFrame *frame asm("a1"))
{
	(void)session;
	(void)frame;
	return VCERR_BADSTATE;
}

/**
 * VCB_Poll() - Take the next item: a packet back, a picture, an event.
 * @session: the session (A0)
 * @item:    filled in (A1)
 *
 * Return: 1 with the item, 0 when nothing waits.
 */
LONG VCB_Poll(APTR session asm("a0"), struct VCItem *item asm("a1"))
{
	if (session == NULL || item == NULL)
		return 0;
	return session_poll(session, item);
}

/**
 * VCB_ReleaseFrame() - The client is done with a picture.
 * @session: the session (A0)
 * @frame:   one of its frames (A1)
 */
VOID VCB_ReleaseFrame(APTR session asm("a0"), struct VCFrame *frame asm("a1"))
{
	if (session != NULL && frame != NULL)
		session_release_frame(session, frame);
}

/**
 * VCB_ReleasePacket() - An output packet back: a decoder gives none.
 * @session: the session (A0)
 * @packet:  the packet (A1)
 */
VOID VCB_ReleasePacket(APTR session asm("a0"), struct VCPacket *packet asm("a1"))
{
	(void)session;
	(void)packet;
}

/**
 * VCB_Flush() - Drop what the decoder holds, for a move.
 * @session: the session (A0)
 *
 * Return: 0 or a VCERR_*.
 */
LONG VCB_Flush(APTR session asm("a0"))
{
	return session != NULL ? session_flush(session) : VCERR_BADPARAM;
}

/**
 * VCB_SetAttrs() - Change a running decoder: its signal, what it may skip.
 * @session: the session (A0)
 * @params:  the change (A1)
 *
 * Return: 0 or a VCERR_*.
 */
LONG VCB_SetAttrs(APTR session asm("a0"), struct VCSessionParams *params asm("a1"))
{
	if (session == NULL || params == NULL || !VC_HAS_FIELD(params->Size, struct VCSessionParams, SigMask))
		return VCERR_BADPARAM;
	return session_set_attrs(session, params);
}

/**
 * VCB_Image() - The image calls: none here, a stream decoder only.
 * @params: the call (A0)
 *
 * Return: VCERR_UNSUPPORTED.
 */
LONG VCB_Image(struct VCImageParams *params asm("a0"))
{
	(void)params;
	return VCERR_UNSUPPORTED;
}

/**
 * VCB_Idle() - The frontend's last opener closed it: nothing is kept between calls here.
 */
VOID VCB_Idle(void)
{
}

/**
 * VCB_OutputFrame() - One of a session's frames, for a converter to import (ABI 1.1).
 * @session: the session (A0)
 * @index:   0 upward (D0)
 *
 * Return: the frame, NULL past the last or before the stream's size is known.
 */
struct VCFrame *VCB_OutputFrame(APTR session asm("a0"), ULONG index asm("d0"))
{
	return session != NULL ? session_output_frame(session, index) : NULL;
}
