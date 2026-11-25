/* SPDX-License-Identifier: MPL-2.0 */
/*
 * audiocodec_lib.c - audiocodec.library: the resident tag, init, open, close
 * and expunge.
 *
 * Backends are opened at the first call that needs one, not here: reading
 * the drawer takes dos.library, which only a Process may call.  They are
 * closed at the expunge.
 */
#include <exec/memory.h>
#include <exec/resident.h>

#include "audiocodec_private.h"

LONG doNotExecute(void);
/**
 * doNotExecute() - The file's first code: run as a program, it returns at once.
 *
 * Return: -1.
 */
LONG __attribute__((used, section(".text.entry"))) doNotExecute(void)
{
	return -1;
}

extern const UBYTE endOfCode;
static const char library_name[] = AUDIOCODEC_NAME;
static const char library_id[] = "$VER: " AUDIOCODEC_NAME " " KINORA_VERSION_TEXT " " KINORA_DATE;
static const APTR init_table[4];

const struct Resident audiocodec_resident __attribute__((used, section(".text.modhdr"))) = {
    RTC_MATCHWORD, (struct Resident *)&audiocodec_resident,
    (APTR)&endOfCode,
    RTF_AUTOINIT,
    KINORA_VERSION,
    NT_LIBRARY,
    0,
    (APTR)library_name,
    (APTR)library_id,
    (APTR)init_table,
};

/**
 * library_free() - Free the library base's memory, the function table below it included.
 * @base: the library base, no longer in the system's list
 */
static void library_free(struct AudioCodecBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	FreeMem((APTR)((ULONG)base - base->library.lib_NegSize), (ULONG)base->library.lib_NegSize + base->library.lib_PosSize);
}

/**
 * library_expunge() - Expunge: free the library when nothing holds it, else mark it for later.
 * @base: the library base (A6)
 *
 * Return: the segment list to unload, 0 when the expunge waits for the last close.
 */
static ULONG library_expunge(struct AudioCodecBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	ULONG segments = base->segments;

	if (base->library.lib_OpenCnt > 0)
	{
		base->library.lib_Flags |= LIBF_DELEXP;
		return 0;
	}
	backends_free(base);
	CloseLibrary(base->UtilityBase);
	Remove((struct Node *)base);
	library_free(base);
	return segments;
}

/**
 * library_init() - Init: set the base up and open what the library uses.
 * @library:  the library base the loader built (D0)
 * @segments: the library's segment list (A0)
 * @SysBase:  exec.library (A6)
 *
 * Return: the library base, NULL without utility.library.
 */
static struct Library *library_init(struct Library *library asm("d0"), ULONG segments asm("a0"), struct ExecBase *SysBase asm("a6"))
{
	struct AudioCodecBase *base = (struct AudioCodecBase *)library;

	base->segments = segments;
	base->SysBase = SysBase;
	base->library.lib_Revision = KINORA_REVISION;
	base->UtilityBase = OpenLibrary((CONST_STRPTR) "utility.library", 36);
	if (base->UtilityBase == NULL)
	{
		library_free(base);
		return NULL;
	}
	InitSemaphore(&base->lock);
	backends_init(base);
	return library;
}

/**
 * library_open() - Open: count the opener and cancel a pending expunge.
 * @version: the version asked for, not checked (D0)
 * @base:    the library base (A6)
 *
 * Return: the library base.
 */
static struct AudioCodecBase *library_open(ULONG version asm("d0"), struct AudioCodecBase *base asm("a6"))
{
	(void)version;
	base->library.lib_OpenCnt++;
	base->library.lib_Flags &= (UBYTE)~LIBF_DELEXP;
	return base;
}

/**
 * library_close() - Close: drop an opener; expunge when it was the last and an expunge waits.
 * @base: the library base (A6)
 *
 * Return: the segment list when the library was expunged, else 0.
 */
static ULONG library_close(struct AudioCodecBase *base asm("a6"))
{
	base->library.lib_OpenCnt--;
	if (base->library.lib_OpenCnt == 0 && (base->library.lib_Flags & LIBF_DELEXP))
		return library_expunge(base);
	return 0;
}

/**
 * library_reserved() - The reserved vector, which AmigaOS never calls.
 *
 * Return: 0.
 */
static ULONG library_reserved(void)
{
	return 0;
}

/* The vectors in the order of sfd/audiocodec_lib.sfd */
static const APTR function_table[] = {
    (APTR)library_open,    (APTR)library_close, (APTR)library_expunge, (APTR)library_reserved, (APTR)AC_OpenSessionTagList,
    (APTR)AC_CloseSession, (APTR)AC_SendPacket, (APTR)AC_ReceiveFrame, (APTR)AC_Flush,         (APTR)AC_QueryCodec,
    (APTR)-1,
};

static const APTR init_table[4] = {
    (APTR)sizeof(struct AudioCodecBase),
    (APTR)function_table,
    NULL,
    (APTR)library_init,
};
