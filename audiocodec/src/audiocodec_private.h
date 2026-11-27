/* SPDX-License-Identifier: MPL-2.0 */
/*
 * audiocodec_private.h - audiocodec.library's own structures.
 *
 *   audiocodec_lib.c       the resident tag, init, open, close, expunge
 *   audiocodec_backend.c   the backends: found in LIBS:audiocodec/, opened, chosen for a codec
 *   audiocodec_session.c   the session calls: tags parsed, the rest forwarded
 *
 * The library is freestanding: no C library, no globals.  Every function
 * gets the bases it calls from the library base.  A backend serialises
 * itself, so the only lock here guards the backend list.
 */
#ifndef AUDIOCODEC_PRIVATE_H
#define AUDIOCODEC_PRIVATE_H

#define __NOLIBBASE__
#define EXEC_BASE_NAME SysBase /* a local in every function, from the library base */

#include <proto/exec.h>
#include <proto/utility.h>

#include <exec/libraries.h>
#include <exec/semaphores.h>
#include <exec/lists.h>

#include <libraries/audiocodec.h>
#include <libraries/audiocodec_backend.h>

/* A backend's calls go through the generated inline macros with a local base of this name */
#define AUDIOCODEC_BACKEND_BASE_NAME ACBackendBase
#include <proto/audiocodec_backend.h>

#define AC_FALLBACK_BACKEND "ffmpegaudio.codec" /* opened by name when the drawer cannot be read (a Task caller) */
#define AC_SESSION_MAGIC 0x41435345u       /* 'ACSE': marks a live struct ACSession */

/* One backend in the list: its library, kept open, and its codec table, read once */
struct ac_backend
{
	struct MinNode node;             /* in AudioCodecBase.backends */
	struct Library *library;         /* the backend's base, for its ACB_* calls */
	struct ACCodecInfo *rows;        /* its codec table; NULL if empty */
	ULONG row_count;                 /* rows in the table */
	char name[AC_BACKEND_NAME_SIZE]; /* its file name in LIBS:audiocodec/ */
};

/* What a client's session pointer points to */
struct ACSession
{
	ULONG magic;                /* AC_SESSION_MAGIC while open; 0 once closed, so a stale pointer is refused */
	struct ac_backend *backend; /* the backend serving it */
	APTR handle;                /* the backend's session, passed to every ACB_* call */
};

/* The library base */
struct AudioCodecBase
{
	struct Library library;      /* Exec's part */
	ULONG segments;              /* the segment list, returned at expunge */
	struct ExecBase *SysBase;    /* exec.library */
	struct Library *UtilityBase; /* utility.library V36: tag lists */
	struct SignalSemaphore lock; /* guards backends and scanned */
	struct MinList backends;     /* struct ac_backend, in name order */
	BOOL scanned;                /* LIBS:audiocodec/ was read */
};

void backends_init(struct AudioCodecBase *base);
void backends_free(struct AudioCodecBase *base);
void backends_load(struct AudioCodecBase *base);
struct ac_backend *backend_choose(struct AudioCodecBase *base, ULONG codec, const char *name);

struct ACSession *AC_OpenSessionTagList(struct TagItem *tags asm("a0"), struct AudioCodecBase *base asm("a6"));
VOID AC_CloseSession(struct ACSession *session asm("a0"), struct AudioCodecBase *base asm("a6"));
LONG AC_SendPacket(struct ACSession *session asm("a0"), struct ACPacket *packet asm("a1"), struct AudioCodecBase *base asm("a6"));
LONG AC_ReceiveFrame(struct ACSession *session asm("a0"), struct ACFrame *frame asm("a1"), struct AudioCodecBase *base asm("a6"));
LONG AC_Flush(struct ACSession *session asm("a0"), struct AudioCodecBase *base asm("a6"));
LONG AC_QueryCodec(ULONG index asm("d0"), struct ACCodecInfo *info asm("a0"), struct AudioCodecBase *base asm("a6"));

#endif /* AUDIOCODEC_PRIVATE_H */
