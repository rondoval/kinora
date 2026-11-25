/* SPDX-License-Identifier: MPL-2.0 */
/*
 * audio_device.c - the sound through ahi.device's writes, which share the
 * unit with other programs: where the setup asks for that, and where AHI's
 * low-level calls cannot be had (audio_low.c).
 *
 * Two blocks of about 40 ms are in flight, each linked behind the other as
 * AHI asks for gapless play.  A stream is played at the centre, which AHI's
 * pan law puts 6 dB low, stereo and 7.1 alike.
 */
#include <exec/memory.h>
#include <dos/dos.h>
#include <devices/ahi.h>
#include <proto/exec.h>

#include "audio_private.h"

#define PAN_CENTRE 0x8000 /* ahir_Position: where a stream belongs; AHI plays it there 6 dB low (its pan law) */

/**
 * block_send() - Fill a block and queue it behind the one in flight.
 * @out:     the output
 * @block:   the block, back from AHI or never sent
 * @playing: the block it follows, NULL for the first
 */
static void block_send(struct audio_out *out, struct block *block, struct block *playing)
{
	chunk_fill(out, &block->chunk);
	struct AHIRequest *request = &block->request;
	request->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
	request->ahir_Std.io_Command = CMD_WRITE;
	request->ahir_Std.io_Data = block->chunk.samples;
	request->ahir_Std.io_Length = out->chunk_frames * out->frame_bytes;
	request->ahir_Std.io_Offset = 0;
	request->ahir_Frequency = out->setup.rate;
	request->ahir_Type = out->sample_type;
	request->ahir_Volume = (Fixed)out->volume;
	request->ahir_Position = PAN_CENTRE;
	request->ahir_Link = playing != NULL ? &playing->request : NULL;
	SendIO(&request->ahir_Std);
	block->sent = TRUE;
}

/**
 * block_in_flight() - Whether a block is with AHI and has not come back.
 * @block: the block
 *
 * Return: TRUE if it is.
 */
static BOOL block_in_flight(struct block *block)
{
	return block->sent && CheckIO((struct IORequest *)&block->request) == NULL;
}

/**
 * blocks_start() - Send all blocks, the first with no link and each other one behind the one before.
 * @out: the output, the unit open and no block in flight
 */
static void blocks_start(struct audio_out *out)
{
	struct device_output *device = &out->device;
	block_send(out, &device->blocks[0], NULL);
	for (ULONG i = 1; i < BLOCKS; i++)
		block_send(out, &device->blocks[i], &device->blocks[i - 1]);
	device->newest = BLOCKS - 1;
}

/**
 * block_done() - A block came back: it goes out again at the chain's end.
 * @out:  the output
 * @done: the block that came back
 *
 * It is linked behind the newest block, which AHI starts now.  ahi.device
 * mishandles a link to a write that it has finished but not yet replied: it
 * gives the new write the old one's channel number, and a write linked
 * behind that one is never played.  The newest block ends a block's length
 * from now, so AHI's unit process would have to be held up that long; the
 * blocks are long for it.  The block after this one starts now, and the
 * clock is told its time.  When the chain has run dry this one goes with no
 * link and starts at once, so its own time is the clock's.
 */
static void block_done(struct audio_out *out, struct block *done)
{
	struct device_output *device = &out->device;
	ULONG index = (ULONG)(done - device->blocks);
	struct block *next = &device->blocks[(index + 1) % BLOCKS];
	struct block *newest = &device->blocks[device->newest];
	BOOL chained = block_in_flight(newest);
	BOOL next_playing = block_in_flight(next);
	done->sent = FALSE;
	out->watch.chunks_done++;
	block_send(out, done, chained ? newest : NULL);
	device->newest = index;

	ObtainSemaphore(&out->lock);
	VCTime delay = out->delay;
	VCTime started = VC_TIME_UNKNOWN; /* a flush may have taken the blocks' times away */
	if (next_playing)
		started = next->chunk.time;
	else if (!chained)
		started = done->chunk.time;
	ReleaseSemaphore(&out->lock);
	if (started != VC_TIME_UNKNOWN)
		clock_follow(out->setup.clock, started + delay);
}

/**
 * unit_open() - Open ahi.device on the setup's unit for the blocks.
 * @out: the output, its port made
 *
 * Return: 0, or OpenDevice()'s error.
 */
static LONG unit_open(struct audio_out *out)
{
	struct device_output *device = &out->device;
	struct AHIRequest *first = &device->blocks[0].request;
	*first = (struct AHIRequest){0};
	first->ahir_Std.io_Message.mn_ReplyPort = device->port;
	first->ahir_Std.io_Message.mn_Length = sizeof(*first);
	first->ahir_Version = AHI_VERSION;
	LONG error = OpenDevice((CONST_STRPTR)AHINAME, out->setup.unit, &first->ahir_Std, 0);
	if (error != 0)
		return error;
	device->unit_open = TRUE;
	for (ULONG i = 1; i < BLOCKS; i++)
		device->blocks[i].request = *first;
	return 0;
}

/**
 * unit_close() - Take the blocks in flight back from AHI and close ahi.device.
 * @out: the output; a unit not open is left alone
 */
static void unit_close(struct audio_out *out)
{
	struct device_output *device = &out->device;
	for (ULONG i = 0; i < BLOCKS; i++)
	{
		struct block *block = &device->blocks[i];
		if (!block->sent)
			continue;
		if (CheckIO((struct IORequest *)&block->request) == NULL)
			AbortIO((struct IORequest *)&block->request);
		WaitIO((struct IORequest *)&block->request);
		block->sent = FALSE;
	}
	if (device->unit_open)
		CloseDevice((struct IORequest *)&device->blocks[0].request);
	device->unit_open = FALSE;
}

/**
 * unit_restart() - Close the unit, open it again and send the blocks anew.
 * @out: the output
 *
 * For an AHI that stopped answering: the engine saw no block come back for
 * STALL_TIME.  Taking the blocks back alone leaves ahi.device's channels as
 * a mishandled link left them; closing the unit ends its state when Kinora
 * is its only user.  A unit that does not open again is tried at the next
 * restart.
 */
static void unit_restart(struct audio_out *out)
{
	out->watch.restarts++;
	out->watch.step = RESTART_CLOSING;
	unit_close(out);
	out->watch.step = RESTART_OPENING;
	if (unit_open(out) != 0)
		return;
	blocks_start(out);
	out->watch.step = RESTART_DONE;
}

/**
 * device_open() - Make the blocks and open ahi.device for them.
 * @out: the output
 *
 * Return: 0, or the reason: OpenDevice()'s error, ERROR_NO_FREE_STORE.
 */
LONG device_open(struct audio_out *out)
{
	struct device_output *device = &out->device;
	device->port = CreateMsgPort();
	if (device->port == NULL)
		return ERROR_NO_FREE_STORE;
	for (ULONG i = 0; i < BLOCKS; i++)
		if (!chunk_make(out, &device->blocks[i].chunk))
			return ERROR_NO_FREE_STORE;
	return unit_open(out);
}

/**
 * device_close() - Close ahi.device and free the blocks.
 * @out: the output, as far as device_open() got
 */
void device_close(struct audio_out *out)
{
	struct device_output *device = &out->device;
	unit_close(out);
	for (ULONG i = 0; i < BLOCKS; i++)
		chunk_free(&device->blocks[i].chunk);
	if (device->port != NULL)
		DeleteMsgPort(device->port);
	device->port = NULL;
}

/**
 * device_play() - The audio task's loop: blocks out, blocks back, until the end message comes.
 * @out:     the output, ahi.device open
 * @control: the task's own port, where the end message comes
 *
 * Return: the end message.
 */
struct Message *device_play(struct audio_out *out, struct MsgPort *control)
{
	blocks_start(out);
	for (;;)
	{
		ULONG signals = Wait(1UL << out->device.port->mp_SigBit | 1UL << control->mp_SigBit | RESTART_SIGNAL);
		struct Message *message;
		if (signals & RESTART_SIGNAL)
			unit_restart(out);
		while ((message = GetMsg(out->device.port)) != NULL)
			block_done(out, (struct block *)message);
		message = GetMsg(control);
		if (message != NULL)
			return message;
	}
}
