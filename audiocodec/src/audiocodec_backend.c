/* SPDX-License-Identifier: MPL-2.0 */
/*
 * audiocodec_backend.c - the backends: found in LIBS:audiocodec/, opened,
 * checked for the ABI, their codec tables kept, and chosen for a session by
 * the priority of their codec rows.
 *
 * The drawer is read with dos.library, which only a Process may call.  A
 * Task that comes first gets the fallback backend by name, and the drawer is
 * read at the next call from a Process.  Everything here runs with the
 * base's lock held, except backends_init() and backends_free().
 */
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

#include <memory.h>
#include <minlist.h>
#include <strutil.h>

#include "audiocodec_private.h"

#define BACKEND_OF(node) ((struct ac_backend *)(node))
#define ROWS_MAX 64 /* codec rows read from one backend: a bound against one that never says "no more" */

/**
 * is_backend_name() - Whether a file in the drawer is a backend.
 * @name: the file name, without a path
 *
 * Return: TRUE if it is longer than ".codec" and ends in it, in any case.
 */
static BOOL is_backend_name(const char *name)
{
	ULONG length = (ULONG)strlen(name), suffix = (ULONG)sizeof(AUDIOCODEC_BACKEND_SUFFIX) - 1u;
	return length > suffix && _Stricmp((CONST_STRPTR)(name + length - suffix), (CONST_STRPTR)AUDIOCODEC_BACKEND_SUFFIX) == 0;
}

/**
 * name_matches() - Whether a name a program gave means a backend.
 * @backend: the backend's file name ("ffmpeg.codec")
 * @name:    the name asked for, with or without ".codec", in any case
 *
 * Return: TRUE if it does.
 */
static BOOL name_matches(const char *backend, const char *name)
{
	LONG length = (LONG)strlen(name);
	return _Strnicmp((CONST_STRPTR)backend, (CONST_STRPTR)name, length) == 0 &&
	       (backend[length] == 0 || _Stricmp((CONST_STRPTR)(backend + length), (CONST_STRPTR)AUDIOCODEC_BACKEND_SUFFIX) == 0);
}

/**
 * backend_named() - Find a backend in the list by its name.
 * @base: the library base
 * @name: the name, with or without ".codec", in any case
 *
 * Return: the backend, NULL if none of that name is open.
 */
static struct ac_backend *backend_named(struct AudioCodecBase *base, const char *name)
{
	for (struct MinNode *node = base->backends.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
		if (name_matches(BACKEND_OF(node)->name, name))
			return BACKEND_OF(node);
	return NULL;
}

/**
 * backend_drop() - Close a backend that is not in the list and free it.
 * @base:    the library base
 * @backend: the backend; its library may be NULL
 */
static void backend_drop(struct AudioCodecBase *base, struct ac_backend *backend)
{
	struct ExecBase *SysBase = base->SysBase;
	FreeVec(backend->rows);
	if (backend->library != NULL)
		CloseLibrary(backend->library);
	FreeVec(backend);
}

/**
 * backend_open() - Open a backend's library and check that it speaks this ABI.
 * @base:    the library base
 * @backend: the backend, its name set
 * @rows:    gets the number of its codec rows
 *
 * Opens "audiocodec/<name>": Exec looks for it in every directory of LIBS:.
 *
 * Return: TRUE with the library open and the ABI's major this header's.
 */
static BOOL backend_open(struct AudioCodecBase *base, struct ac_backend *backend, ULONG *rows)
{
	struct ExecBase *SysBase = base->SysBase;
	char path[sizeof(AUDIOCODEC_BACKEND_DRAWER "/") + AC_BACKEND_NAME_SIZE];
	strlcpy(path, AUDIOCODEC_BACKEND_DRAWER "/", sizeof(path));
	strlcpy(path + sizeof(AUDIOCODEC_BACKEND_DRAWER "/") - 1u, backend->name, AC_BACKEND_NAME_SIZE);

	backend->library = OpenLibrary((CONST_STRPTR)path, 0);
	if (backend->library == NULL)
		return FALSE;

	struct Library *ACBackendBase = backend->library;
	struct ACBackendInfo info;
	memset(&info, 0, sizeof(info));
	info.Size = sizeof(info);
	if (ACB_GetInfo(&info) != 0 || !AC_HAS_FIELD(info.Size, struct ACBackendInfo, Codecs))
		return FALSE;
	*rows = info.Codecs;
	return ACB_ABI_MAJOR(info.ABIVersion) == ACB_ABI_VERSION_MAJOR;
}

/**
 * rows_read() - Copy a backend's codec table.
 * @base:    the library base
 * @backend: the backend, its library open
 * @rows:    the rows it reported
 *
 * A backend that answers fewer rows than it reported keeps those it gave.
 *
 * Return: TRUE with the table kept (an empty one included), FALSE without memory.
 */
static BOOL rows_read(struct AudioCodecBase *base, struct ac_backend *backend, ULONG rows)
{
	struct ExecBase *SysBase = base->SysBase;
	struct Library *ACBackendBase = backend->library;

	if (rows > ROWS_MAX)
		rows = ROWS_MAX;
	if (rows == 0)
		return TRUE;
	backend->rows = AllocVec(rows * sizeof(struct ACCodecInfo), MEMF_PUBLIC | MEMF_CLEAR);
	if (backend->rows == NULL)
		return FALSE;
	for (ULONG i = 0; i < rows; i++)
	{
		struct ACCodecInfo *row = &backend->rows[i];
		row->Size = sizeof(*row);
		if (ACB_QueryCodec(i, row) != 0)
			break;
		strlcpy(row->Backend, backend->name, sizeof(row->Backend));
		backend->row_count = i + 1u;
	}
	return TRUE;
}

/**
 * backend_insert() - Put a backend into the list in name order.
 * @base:    the library base
 * @backend: the backend, not yet in the list
 *
 * Name order makes the choice between rows of equal priority the same every time.
 */
static void backend_insert(struct AudioCodecBase *base, struct ac_backend *backend)
{
	struct ExecBase *SysBase = base->SysBase;
	struct MinNode *node = base->backends.mlh_Head;
	while (node->mln_Succ != NULL && strcmp(BACKEND_OF(node)->name, backend->name) < 0)
		node = node->mln_Succ;
	Insert((struct List *)&base->backends, (struct Node *)&backend->node, (struct Node *)node->mln_Pred);
}

/**
 * backend_add() - Open one backend and keep it if it speaks the ABI.
 * @base: the library base
 * @name: the backend's file name, without the drawer
 *
 * A name already in the list is skipped.
 */
static void backend_add(struct AudioCodecBase *base, const char *name)
{
	struct ExecBase *SysBase = base->SysBase;
	if (strlen(name) >= AC_BACKEND_NAME_SIZE || backend_named(base, name) != NULL)
		return;

	struct ac_backend *backend = AllocVec(sizeof(*backend), MEMF_PUBLIC | MEMF_CLEAR);
	if (backend == NULL)
		return;
	strlcpy(backend->name, name, sizeof(backend->name));
	ULONG rows = 0;
	if (backend_open(base, backend, &rows) && rows_read(base, backend, rows))
		backend_insert(base, backend);
	else
		backend_drop(base, backend);
}

/**
 * directory_scan() - Add every backend file of one directory.
 * @base:    the library base
 * @DOSBase: dos.library
 * @lock:    a shared lock on the directory
 */
static void directory_scan(struct AudioCodecBase *base, struct DosLibrary *DOSBase, BPTR lock)
{
	struct FileInfoBlock *entry = AllocDosObject(DOS_FIB, NULL);
	if (entry == NULL)
		return;
	if (Examine(lock, entry) && entry->fib_DirEntryType > 0)
		while (ExNext(lock, entry))
			if (entry->fib_DirEntryType < 0 && is_backend_name((const char *)entry->fib_FileName))
				backend_add(base, (const char *)entry->fib_FileName);
	FreeDosObject(DOS_FIB, entry);
}

/**
 * drawer_lock() - Lock the backends' drawer in one directory of LIBS:.
 * @DOSBase: dos.library
 * @place:   where GetDeviceProc() is in the assign
 *
 * Return: the lock, 0 when that directory has no such drawer.
 */
static BPTR drawer_lock(struct DosLibrary *DOSBase, const struct DevProc *place)
{
	if (place->dvp_Lock == 0)
		return Lock((CONST_STRPTR) "LIBS:" AUDIOCODEC_BACKEND_DRAWER, SHARED_LOCK); /* a volume or device, not an assign */
	BPTR old = CurrentDir(place->dvp_Lock);
	BPTR lock = Lock((CONST_STRPTR)AUDIOCODEC_BACKEND_DRAWER, SHARED_LOCK);
	CurrentDir(old);
	return lock;
}

/**
 * drawer_scan() - Add every backend found in LIBS:audiocodec/.
 * @base: the library base
 *
 * LIBS: is usually an assign of several directories, and each may have the
 * drawer: GetDeviceProc() walks them all.  The caller is a Process.
 */
static void drawer_scan(struct AudioCodecBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct DosLibrary *DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR) "dos.library", 36);
	if (DOSBase == NULL)
		return;

	struct DevProc *place = NULL;
	while ((place = GetDeviceProc((CONST_STRPTR) "LIBS:" AUDIOCODEC_BACKEND_DRAWER, place)) != NULL)
	{
		BPTR lock = drawer_lock(DOSBase, place);
		if (lock != 0)
		{
			directory_scan(base, DOSBase, lock);
			UnLock(lock);
		}
		if ((place->dvp_Flags & DVPF_ASSIGN) == 0)
			break;
	}
	FreeDeviceProc(place);
	CloseLibrary((struct Library *)DOSBase);
	base->scanned = TRUE;
}

/**
 * backends_init() - Start with no backends and the drawer not read.
 * @base: the library base, at the library's init
 */
void backends_init(struct AudioCodecBase *base)
{
	_NewMinList(&base->backends);
	base->scanned = FALSE;
}

/**
 * backends_free() - Close every backend.
 * @base: the library base, at the expunge: no session is open
 */
void backends_free(struct AudioCodecBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	struct MinNode *node;
	while ((node = (struct MinNode *)RemHead((struct List *)&base->backends)) != NULL)
		backend_drop(base, BACKEND_OF(node));
	base->scanned = FALSE;
}

/**
 * backends_load() - Make the backend list ready for use.
 * @base: the library base, its lock held
 *
 * Reads the drawer if that has not been done and the caller is a Process.
 * With the list still empty it opens the fallback backend by name, which
 * needs no DOS.
 */
void backends_load(struct AudioCodecBase *base)
{
	struct ExecBase *SysBase = base->SysBase;
	if (base->scanned)
		return;
	if (FindTask(NULL)->tc_Node.ln_Type == NT_PROCESS)
		drawer_scan(base);
	if (base->backends.mlh_Head->mln_Succ == NULL)
		backend_add(base, AC_FALLBACK_BACKEND);
}

/**
 * backend_choose() - The backend for a codec.
 * @base:  the library base, its lock held, the list loaded
 * @codec: ACC_*
 * @name:  the backend the program asked for (ACA_Backend), NULL to leave the choice to the library
 *
 * Of every row for the codec the one with the highest priority; a tie keeps
 * the backend first in name order.
 *
 * Return: the backend, NULL when none has the codec.
 */
struct ac_backend *backend_choose(struct AudioCodecBase *base, ULONG codec, const char *name)
{
	struct ac_backend *best = NULL;
	LONG best_priority = 0;
	for (struct MinNode *node = base->backends.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct ac_backend *backend = BACKEND_OF(node);
		if (name != NULL && !name_matches(backend->name, name))
			continue;
		for (ULONG i = 0; i < backend->row_count; i++)
		{
			const struct ACCodecInfo *row = &backend->rows[i];
			if (row->Codec == codec && (best == NULL || row->Priority > best_priority))
			{
				best = backend;
				best_priority = row->Priority;
			}
		}
	}
	return best;
}

/**
 * AC_QueryCodec() - One row of the codec table of all backends.
 * @index: the row, 0 upward; rows come backend by backend, in name order (D0)
 * @info:  filled in, as far as its Size reaches (A0)
 * @base:  the library base (A6)
 *
 * Return: 0, ACERR_BADPARAM past the last row or for a structure too small.
 */
LONG AC_QueryCodec(ULONG index asm("d0"), struct ACCodecInfo *info asm("a0"), struct AudioCodecBase *base asm("a6"))
{
	struct ExecBase *SysBase = base->SysBase;
	if (info == NULL || !AC_HAS_FIELD(info->Size, struct ACCodecInfo, Codec))
		return ACERR_BADPARAM;

	LONG error = ACERR_BADPARAM;
	ObtainSemaphore(&base->lock);
	backends_load(base);
	for (struct MinNode *node = base->backends.mlh_Head; node->mln_Succ != NULL && error != 0; node = node->mln_Succ)
	{
		struct ac_backend *backend = BACKEND_OF(node);
		if (index < backend->row_count)
		{
			ULONG size = info->Size < sizeof(*info) ? info->Size : (ULONG)sizeof(*info);
			memcpy(info, &backend->rows[index], size);
			info->Size = size;
			error = 0;
		}
		else
			index -= backend->row_count;
	}
	ReleaseSemaphore(&base->lock);
	return error;
}
