/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * demuxer_reader.c - the file under a source: dos.library reads, and
 * positions past 2 GiB.
 *
 * Seek() takes a signed 32-bit offset.  A handler that knows the 64-bit
 * position packets (smb2fs and every other filesysbox handler, the large-file
 * build of PFS3) goes anywhere with them.  One that does not is moved with
 * Seek(): from the beginning below 2 GiB, and forward from where it is in
 * steps where the handler follows (PFS3 19 and 20 do not, past 2 GiB); a
 * forward move it still cannot make is read through.  A move backwards to
 * 2 GiB or beyond on such a handler fails.
 */
#include <string.h>
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "demuxer_private.h"

#define ACTION_CHANGE_FILE_POSITION64 8001 /* dp_Arg2: the position, dp_Arg3: OFFSET_* */
#define ACTION_GET_FILE_SIZE64 8004        /* dp_Res1: the size */
#define SEEK_LIMIT 0x7fffffffLL            /* the furthest Seek() goes in one call */
#define SEEK_STEP 0x40000000LL             /* a relative step where more are needed */

/* The packet of the 64-bit actions: struct DosPacket with dp_Res1 and dp_Arg2 widened */
struct DosPacket64
{
	struct Message *dp_Link; /* its message */
	struct MsgPort *dp_Port; /* where the answer goes */
	LONG dp_Type;            /* ACTION_* */
	LONG dp_Res0;            /* where struct DosPacket has dp_Res1: a handler that does not know the action answers here */
	LONG dp_Res2;            /* IoErr() */
	LONG dp_Pad1;            /* keeps dp_Res1 on 8 bytes */
	int64_t dp_Res1;         /* the result */
	LONG dp_Arg1;            /* the file's fh_Arg1 */
	LONG dp_Pad2;            /* keeps dp_Arg2 on 8 bytes */
	int64_t dp_Arg2;         /* a position or size */
	LONG dp_Arg3;            /* a mode */
	LONG dp_Arg4;            /* unused */
	int64_t dp_Arg5;         /* unused */
};

/* A 64-bit packet with the message that carries it */
struct wide_packet
{
	struct Message message;    /* sent to the handler */
	struct DosPacket64 packet; /* its ln_Name */
};

/**
 * wide_send() - Send a 64-bit packet to the file's handler and wait for the answer.
 * @reader:   the file
 * @wide:     the packet, filled in here
 * @action:   ACTION_*
 * @position: its dp_Arg2
 * @mode:     its dp_Arg3
 *
 * The caller is a Process: the answer comes to its pr_MsgPort.
 */
static void wide_send(const struct reader *reader, struct wide_packet *wide, LONG action, int64_t position, LONG mode)
{
	struct FileHandle *handle = BADDR(reader->file);
	struct Process *self = (struct Process *)FindTask(NULL);

	memset(wide, 0, sizeof(*wide));
	wide->message.mn_Node.ln_Name = (char *)&wide->packet;
	wide->packet.dp_Link = &wide->message;
	wide->packet.dp_Port = &self->pr_MsgPort;
	wide->packet.dp_Type = action;
	wide->packet.dp_Arg1 = handle->fh_Arg1;
	wide->packet.dp_Arg2 = position;
	wide->packet.dp_Arg3 = mode;
	PutMsg(handle->fh_Type, &wide->message);
	WaitPort(&self->pr_MsgPort);
	GetMsg(&self->pr_MsgPort);
}

/**
 * reader_measure() - Learn the file's size and whether its handler takes the 64-bit packets.
 * @reader: the file, open
 *
 * Return: FALSE when neither the packet nor ExamineFH() gives a size.
 */
static BOOL reader_measure(struct reader *reader)
{
	struct wide_packet wide;
	wide_send(reader, &wide, ACTION_GET_FILE_SIZE64, 0, 0);
	if (wide.packet.dp_Res2 == 0 && wide.packet.dp_Res1 > 0)
	{
		reader->wide = TRUE;
		reader->size = wide.packet.dp_Res1;
		return TRUE;
	}

	struct FileInfoBlock *info = AllocDosObject(DOS_FIB, NULL);
	if (info == NULL)
		return FALSE;
	BOOL examined = ExamineFH(reader->file, info) != 0;
	reader->size = (int64_t)(ULONG)info->fib_Size; /* up to 4 GiB less one */
	FreeDosObject(DOS_FIB, info);
	return examined;
}

/**
 * reader_open() - Open a file for reading.
 * @reader: the reader to fill in
 * @name:   the file
 *
 * Return: FALSE (IoErr() says why) when it does not open or has no size.
 */
BOOL reader_open(struct reader *reader, CONST_STRPTR name)
{
	memset(reader, 0, sizeof(*reader));
	reader->file = Open(name, MODE_OLDFILE);
	if (reader->file == 0)
		return FALSE;
	if (reader_measure(reader))
		return TRUE;
	reader_close(reader);
	return FALSE;
}

/**
 * reader_close() - Close the file and free what the reader made.
 * @reader: the reader; closing one twice does nothing
 */
void reader_close(struct reader *reader)
{
	if (reader->file != 0)
		Close(reader->file);
	reader->file = 0;
	if (reader->scratch != NULL)
		FreeMem(reader->scratch, READER_SCRATCH_SIZE);
	reader->scratch = NULL;
}

/**
 * reader_read() - Read from where the file is.
 * @reader: the file
 * @buffer: where the bytes go
 * @length: how many at most
 *
 * Return: the bytes read, 0 at the end, -1 for an error (IoErr()).
 */
LONG reader_read(struct reader *reader, APTR buffer, LONG length)
{
	LONG got = Read(reader->file, buffer, length);
	if (got > 0)
		reader->position += got;
	return got;
}

/**
 * reader_seekable() - Whether the file can be moved in at all.
 * @reader: the file
 *
 * Return: TRUE with the 64-bit packets, or for a file Seek() covers whole.
 */
BOOL reader_seekable(const struct reader *reader)
{
	return reader->wide || reader->size <= SEEK_LIMIT;
}

/**
 * seek_forward() - Move forward with Seek() from where the file is, a step at a time.
 * @reader:   the file
 * @position: where to, past the reader's position
 *
 * Return: FALSE at the first step the handler refuses; the position then is
 * where the steps before it led.
 */
static BOOL seek_forward(struct reader *reader, int64_t position)
{
	while (reader->position < position)
	{
		int64_t step = position - reader->position < SEEK_STEP ? position - reader->position : SEEK_STEP;
		SetIoErr(0);
		if (Seek(reader->file, (LONG)step, OFFSET_CURRENT) == -1 && IoErr() != 0)
			return FALSE;
		reader->position += step;
	}
	return TRUE;
}

/**
 * read_forward() - Move forward by reading and dropping what lies between.
 * @reader:   the file
 * @position: where to, past the reader's position
 *
 * Return: FALSE when the file ends or fails before that, or for no memory.
 */
static BOOL read_forward(struct reader *reader, int64_t position)
{
	if (reader->scratch == NULL)
		reader->scratch = AllocMem(READER_SCRATCH_SIZE, MEMF_ANY);
	if (reader->scratch == NULL)
		return FALSE;
	while (reader->position < position)
	{
		int64_t left = position - reader->position;
		LONG want = left < (int64_t)READER_SCRATCH_SIZE ? (LONG)left : (LONG)READER_SCRATCH_SIZE;
		if (reader_read(reader, reader->scratch, want) <= 0)
			return FALSE;
	}
	return TRUE;
}

/**
 * seek_wide() - Move with the 64-bit position packet.
 * @reader:   the file, its handler one that takes the packet
 * @position: where to, from the file's beginning
 *
 * Return: FALSE when the handler refuses.
 */
static BOOL seek_wide(struct reader *reader, int64_t position)
{
	struct wide_packet wide;
	wide_send(reader, &wide, ACTION_CHANGE_FILE_POSITION64, position, OFFSET_BEGINNING);
	if (wide.packet.dp_Res1 == 0)
		return FALSE;
	reader->position = position;
	return TRUE;
}

/**
 * seek_near() - Move with one Seek() from the file's beginning.
 * @reader:   the file
 * @position: where to, SEEK_LIMIT at most
 *
 * Return: FALSE when the handler refuses.
 */
static BOOL seek_near(struct reader *reader, int64_t position)
{
	SetIoErr(0);
	if (Seek(reader->file, (LONG)position, OFFSET_BEGINNING) == -1 && IoErr() != 0)
		return FALSE;
	reader->position = position;
	return TRUE;
}

/**
 * reader_seek() - Move the file to a position.
 * @reader:   the file
 * @position: where to, from the file's beginning
 *
 * Return: FALSE when the handler cannot go there; the reader's position is
 * then where the file really is.
 */
BOOL reader_seek(struct reader *reader, int64_t position)
{
	if (position < 0 || position > reader->size)
		return FALSE;
	if (position == reader->position)
		return TRUE;

	if (reader->wide)
		return seek_wide(reader, position);
	if (position <= SEEK_LIMIT)
		return seek_near(reader, position);
	if (position < reader->position)
		return FALSE;
	return seek_forward(reader, position) || read_forward(reader, position);
}
