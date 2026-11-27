/* SPDX-License-Identifier: MPL-2.0 */
/*
 * audiocodec_session.c - the session calls: the open parses the tags and
 * picks a backend, the rest are forwarded to it one to one.
 */
#include <exec/memory.h>
#include <strutil.h>

#include "audiocodec_private.h"

#define OUTPUT_CHANNELS_MAX 8u /* the most a mix-down can be asked for */

/**
 * params_read() - A session's tags as the structure a backend gets.
 * @base:   the library base
 * @tags:   the tags
 * @params: filled in
 *
 * Return: 0, ACERR_BADPARAM for a value no backend could take.
 */
static LONG params_read(struct AudioCodecBase *base, struct TagItem *tags, struct ACSessionParams *params)
{
	struct Library *UtilityBase = base->UtilityBase;
	*params = (struct ACSessionParams){
	    .Size = sizeof(*params),
	    .Codec = GetTagData(ACA_Codec, ACC_None, tags),
	    .SampleRate = GetTagData(ACA_SampleRate, 0, tags),
	    .Channels = GetTagData(ACA_Channels, 0, tags),
	    .Bits = GetTagData(ACA_Bits, 0, tags),
	    .PcmFlags = GetTagData(ACA_PcmFlags, 0, tags),
	    .OutputFormat = GetTagData(ACA_OutputFormat, ACF_S16, tags),
	    .OutputChannels = GetTagData(ACA_OutputChannels, 0, tags),
	    .ExtradataSize = GetTagData(ACA_ExtradataSize, 0, tags),
	    .Extradata = (CONST_APTR)GetTagData(ACA_Extradata, 0, tags),
	};
	BOOL format_known = params->OutputFormat == ACF_S16 || params->OutputFormat == ACF_S32;
	BOOL extradata_whole = (params->Extradata != NULL) == (params->ExtradataSize != 0);
	if (params->Codec == ACC_None || !format_known || !extradata_whole || params->OutputChannels > OUTPUT_CHANNELS_MAX)
		return ACERR_BADPARAM;
	return 0;
}

/**
 * session_open() - Open a session at the backend that has the codec.
 * @base:   the library base
 * @params: the parsed tags
 * @name:   the backend asked for, NULL for the library's choice
 * @error:  gets the ACERR_* of a refusal
 *
 * Return: the session, NULL when refused.
 */
static struct ACSession *session_open(struct AudioCodecBase *base, struct ACSessionParams *params, const char *name, LONG *error)
{
	struct ExecBase *SysBase = base->SysBase;

	ObtainSemaphore(&base->lock);
	backends_load(base);
	struct ac_backend *backend = backend_choose(base, params->Codec, name);
	ReleaseSemaphore(&base->lock);
	if (backend == NULL)
	{
		*error = ACERR_UNSUPPORTED;
		return NULL;
	}

	struct ACSession *session = AllocVec(sizeof(*session), MEMF_PUBLIC | MEMF_CLEAR);
	if (session == NULL)
	{
		*error = ACERR_NOMEM;
		return NULL;
	}
	struct Library *ACBackendBase = backend->library;
	*error = ACERR_INTERNAL; /* for a backend that refuses without saying why */
	session->handle = ACB_OpenSession(params, error);
	if (session->handle == NULL)
	{
		FreeVec(session);
		return NULL;
	}
	session->backend = backend;
	session->magic = AC_SESSION_MAGIC;
	return session;
}

/**
 * AC_OpenSessionTagList() - Open a decoder for one audio stream.
 * @tags: ACA_Codec and what the container says of the stream (A0)
 * @base: the library base (A6)
 *
 * Return: the session, NULL when refused; ACA_ErrorPtr gets the reason, and
 * ACA_BackendName the backend's name (an empty one for a refusal).
 */
struct ACSession *AC_OpenSessionTagList(struct TagItem *tags asm("a0"), struct AudioCodecBase *base asm("a6"))
{
	struct Library *UtilityBase = base->UtilityBase;
	LONG *error_out = (LONG *)GetTagData(ACA_ErrorPtr, 0, tags);
	struct ACSessionParams params;
	struct ACSession *session = NULL;

	LONG error = params_read(base, tags, &params);
	if (error == 0)
		session = session_open(base, &params, (const char *)GetTagData(ACA_Backend, 0, tags), &error);
	if (error_out != NULL)
		*error_out = session != NULL ? 0 : error;
	char *name_out = (char *)GetTagData(ACA_BackendName, 0, tags);
	if (name_out != NULL)
		strlcpy(name_out, session != NULL ? session->backend->name : "", AC_BACKEND_NAME_SIZE);
	return session;
}

/**
 * session_live() - Whether a pointer a client gave is an open session.
 * @session: the pointer
 *
 * Return: TRUE if it is.
 */
static BOOL session_live(const struct ACSession *session)
{
	return session != NULL && session->magic == AC_SESSION_MAGIC;
}

/**
 * AC_CloseSession() - Close a session.
 * @session: the session; NULL does nothing (A0)
 * @base:    the library base (A6)
 */
VOID AC_CloseSession(struct ACSession *session asm("a0"), struct AudioCodecBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (!session_live(session))
		return;
	struct Library *ACBackendBase = session->backend->library;
	session->magic = 0;
	ACB_CloseSession(session->handle);
	FreeVec(session);
}

/**
 * AC_SendPacket() - Give the decoder one packet of the stream.
 * @session: the session (A0)
 * @packet:  the packet; a Length of 0 is the stream's end (A1)
 * @base:    the library base (A6)
 *
 * Return: 0, ACERR_AGAIN when frames wait to be received first, ACERR_DATA
 * for a packet the decoder cannot read, ACERR_NOMEM, ACERR_BADPARAM or
 * ACERR_INTERNAL.
 */
LONG AC_SendPacket(struct ACSession *session asm("a0"), struct ACPacket *packet asm("a1"), struct AudioCodecBase *base asm("a6"))
{
	(void)base;
	if (!session_live(session) || packet == NULL || packet->Size < sizeof(*packet) || (packet->Data == NULL && packet->Length != 0))
		return ACERR_BADPARAM;
	struct Library *ACBackendBase = session->backend->library;
	return ACB_SendPacket(session->handle, packet);
}

/**
 * AC_ReceiveFrame() - Take the next decoded samples.
 * @session: the session (A0)
 * @frame:   filled in; its Data is the session's until the next call on it (A1)
 * @base:    the library base (A6)
 *
 * Return: 0, ACERR_AGAIN when the decoder needs another packet, ACERR_END
 * after the stream's end, ACERR_NOMEM, ACERR_BADPARAM or ACERR_INTERNAL.
 */
LONG AC_ReceiveFrame(struct ACSession *session asm("a0"), struct ACFrame *frame asm("a1"), struct AudioCodecBase *base asm("a6"))
{
	(void)base;
	if (!session_live(session) || frame == NULL || frame->Size < sizeof(*frame))
		return ACERR_BADPARAM;
	struct Library *ACBackendBase = session->backend->library;
	return ACB_ReceiveFrame(session->handle, frame);
}

/**
 * AC_Flush() - Drop what the decoder holds, as after a seek.
 * @session: the session (A0)
 * @base:    the library base (A6)
 *
 * Return: 0, ACERR_BADPARAM or ACERR_INTERNAL.
 */
LONG AC_Flush(struct ACSession *session asm("a0"), struct AudioCodecBase *base asm("a6"))
{
	(void)base;
	if (!session_live(session))
		return ACERR_BADPARAM;
	struct Library *ACBackendBase = session->backend->library;
	return ACB_Flush(session->handle);
}
