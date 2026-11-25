/* SPDX-License-Identifier: MPL-2.0 */
/*
 * libraries/audiocodec_backend.h - what an audiocodec.library backend is.
 *
 * A backend is an Exec library named <name>.codec in LIBS:audiocodec/.  Its
 * function table is audiocodec/sfd/audiocodec_backend_lib.sfd: ACB_* calls,
 * arguments in registers, the library base in A6.
 *
 * How the frontend uses a backend
 *
 * - At its first use the frontend opens every file in the drawer whose name
 *   ends in .codec and keeps it open until its own expunge.
 * - It calls ACB_GetInfo() first.  A backend is kept only if it reports the
 *   ABI major of this header and ACBackendInfo.Codecs, the number of rows
 *   ACB_QueryCodec() answers.  The table is read once.
 * - A session goes to the backend whose row for the codec has the highest
 *   ACCodecInfo.Priority (ties by backend name), or to the one the program
 *   names with ACA_Backend.
 * - The frontend parses the tags.  The backend gets a struct ACSessionParams
 *   and never sees a tag.  Every other call is forwarded one to one.
 *
 * What a backend promises
 *
 * - Every structure crossing this interface starts with Size and grows only
 *   at its end.  New functions go at the end of the table.
 * - Packets, frames, their order and their memory are as
 *   libraries/audiocodec.h promises the client.  Errors are ACERR_*.
 * - Any task may call a backend at any time; the backend serialises itself.
 * - A row's Backend is the frontend's to fill: a backend leaves it empty.
 */
#ifndef LIBRARIES_AUDIOCODEC_BACKEND_H
#define LIBRARIES_AUDIOCODEC_BACKEND_H

#include <libraries/audiocodec.h>

/* The ABI version a backend reports: major << 16 | minor.  The frontend keeps a backend only if the major is its own. */
#define ACB_ABI_VERSION_MAJOR 1
#define ACB_ABI_VERSION_MINOR 0
#define ACB_ABI_VERSION ((ACB_ABI_VERSION_MAJOR << 16) | ACB_ABI_VERSION_MINOR)
#define ACB_ABI_MAJOR(version) ((version) >> 16)

/* ACCodecInfo.Priority */
#define ACB_PRIORITY_HARDWARE 50 /* something other than the 68k does the work */
#define ACB_PRIORITY_SOFTWARE 0  /* the 68k does the work */

/* What a backend is (ACB_GetInfo()) */
struct ACBackendInfo
{
	ULONG Size;       /* of this structure */
	ULONG ABIVersion; /* ACB_ABI_VERSION as the backend was built with */
	ULONG Codecs;     /* the rows ACB_QueryCodec() answers */
};

/* A session's tags, parsed (ACB_OpenSession()).  Each field is the tag of its name; 0 means not given. */
struct ACSessionParams
{
	ULONG Size;           /* of this structure */
	ULONG Codec;          /* ACC_* */
	ULONG SampleRate;     /* as the container says */
	ULONG Channels;       /* as the container says */
	ULONG Bits;           /* ACC_PCM: bits a sample */
	ULONG PcmFlags;       /* ACC_PCM: ACPCM_* */
	ULONG OutputFormat;   /* ACF_*, never 0: the frontend puts the default in */
	ULONG OutputChannels; /* mix down to this many; 0: as the stream */
	ULONG ExtradataSize;  /* bytes at Extradata */
	CONST_APTR Extradata; /* the codec's configuration; valid in ACB_OpenSession() only */
};

#endif /* LIBRARIES_AUDIOCODEC_BACKEND_H */
