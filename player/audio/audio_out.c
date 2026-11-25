/* SPDX-License-Identifier: MPL-2.0 */
/*
 * audio_out.c - the player's sound (audio_out.h has the model): the
 * engine's calls, the ring, and the audio task, which plays through one of
 * the two ways to AHI (audio_private.h).
 *
 * The engine and the audio task share the ring, the run flag, the volume,
 * the delay and the chunks' times under one semaphore.  The task is started
 * and ended with one message: it answers it when AHI is open (or did not
 * open), and again, sent a second time, when it has let go of everything.
 * The start comes to the task's pr_MsgPort, before it calls anything; the
 * end to a port of its own.  pr_MsgPort is dos.library's: AHI reads its
 * prefs and loads its drivers in the task, and a message arriving there in
 * the middle of that ends in "unexpected packet received".
 */
#include <string.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/ahi.h>
#include <libraries/audiocodec.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "audio_private.h"

#define RING_PART_OF_SECOND 2 /* the ring holds half a second */
#define TASK_PRIORITY 10      /* above the priorities AmiKit's Executive moves tasks between */
#define TASK_STACK 16384
#define STALL_TIME 2000000    /* microseconds without a block coming back from AHI, or a buffer starting, after which AHI is started anew */
#define MARK_TOLERANCE 2000   /* microseconds a write's time may stray and still follow on (Matroska rounds to 1 ms) */

/**
 * at_or_after() - Whether one position is at or after another, across the positions' wrap.
 * @position: the position
 * @other:    the one it is compared with
 *
 * Return: TRUE if @position is at @other or after it.
 */
static BOOL at_or_after(ULONG position, ULONG other)
{
	return (LONG)(position - other) >= 0;
}

/**
 * mark_follow() - The film time of a sample frame, following on from a mark.
 * @mark:     a mark at or before the frame
 * @position: the frame's position
 * @rate:     sample frames per second
 *
 * Return: the time.
 */
static VCTime mark_follow(const struct mark *mark, ULONG position, ULONG rate)
{
	return mark->time + (VCTime)(position - mark->position) * MICROSECONDS_PER_SECOND / rate;
}

/**
 * ring_mark() - Note the film time of a write's first sample frame, unless it follows on from the last mark.
 * @ring:     the ring
 * @position: the frame's position
 * @time:     its film time
 * @rate:     sample frames per second
 *
 * With the table full the write follows on from the last mark.
 */
static void ring_mark(struct ring *ring, ULONG position, VCTime time, ULONG rate)
{
	if (ring->mark_count != 0)
	{
		VCTime off = time - mark_follow(&ring->marks[ring->mark_count - 1], position, rate);
		if (off <= MARK_TOLERANCE && off >= -MARK_TOLERANCE)
			return;
	}
	if (ring->mark_count < MARKS)
		ring->marks[ring->mark_count++] = (struct mark){.position = position, .time = time};
}

/**
 * ring_head_time() - The film time of the head sample frame.
 * @ring: the ring
 * @rate: sample frames per second
 *
 * The head is timed from the last mark at or before it.  A mark is done
 * with only once the head has reached the one after it, so the marks
 * before that last one are dropped, and it moves up to the first place.
 * It is then moved towards the head in whole seconds: the distance stays
 * short of the positions' wrap, and whole seconds take nothing from the
 * times.
 *
 * Return: the time, VC_TIME_UNKNOWN when no mark is at or before the head.
 */
static VCTime ring_head_time(struct ring *ring, ULONG rate)
{
	ULONG timing = 0; /* the last mark at or before the head: the one that times it */
	while (timing + 1 < ring->mark_count && at_or_after(ring->head_position, ring->marks[timing + 1].position))
		timing++;
	if (timing != 0)
	{
		ring->mark_count -= timing;
		memmove(ring->marks, ring->marks + timing, ring->mark_count * sizeof(ring->marks[0])); /* at a gap only: the head reached a later mark */
	}
	if (ring->mark_count == 0 || !at_or_after(ring->head_position, ring->marks[0].position))
		return VC_TIME_UNKNOWN;
	struct mark *first = &ring->marks[0];
	ULONG seconds = (ring->head_position - first->position) / rate;
	first->position += seconds * rate;
	first->time += (VCTime)seconds * MICROSECONDS_PER_SECOND;
	return mark_follow(first, ring->head_position, rate);
}

/**
 * ring_take() - Move a chunk's worth of sample frames from the ring.
 * @out:     the output, its lock held
 * @samples: gets chunk_frames sample frames
 * @time:    gets the film's time of the first, VC_TIME_UNKNOWN when the ring gave none
 *
 * What the ring cannot fill is silence.
 */
static void ring_take(struct audio_out *out, APTR samples, VCTime *time)
{
	/* what the chunk takes: nothing while stopped, else as much as waits, a chunk's worth at most */
	ULONG frames = out->running ? out->ring.count : 0;
	if (frames > out->chunk_frames)
		frames = out->chunk_frames;

	VCTime head_time = ring_head_time(&out->ring, out->setup.rate);
	*time = frames != 0 ? head_time : VC_TIME_UNKNOWN;

	/* the frames may wrap: first those from the head to the ring's end, then the rest from its start */
	ULONG first = out->ring.capacity - out->ring.head;
	if (first > frames)
		first = frames;
	UBYTE *bytes = samples;
	memcpy(bytes, out->ring.samples + out->ring.head * out->frame_bytes, first * out->frame_bytes);
	memcpy(bytes + first * out->frame_bytes, out->ring.samples, (frames - first) * out->frame_bytes); /* 0 bytes when nothing wraps */
	memset(bytes + frames * out->frame_bytes, 0, (out->chunk_frames - frames) * out->frame_bytes);     /* the chunk's rest: silence */

	/* the frames taken leave the ring */
	out->ring.head = (out->ring.head + frames) % out->ring.capacity;
	out->ring.count -= frames;
	out->ring.head_position += frames;
}

/**
 * chunk_make() - Give a chunk room for its sample frames, silent.
 * @out:   the output, chunk_frames set by the way to AHI taken
 * @chunk: the chunk
 *
 * Return: TRUE, FALSE without memory.
 */
BOOL chunk_make(struct audio_out *out, struct chunk *chunk)
{
	chunk->samples = AllocVec(out->chunk_frames * out->frame_bytes, MEMF_PUBLIC | MEMF_CLEAR);
	chunk->time = VC_TIME_UNKNOWN;
	return chunk->samples != NULL;
}

/**
 * chunk_free() - Free a chunk's room.
 * @chunk: the chunk, made or not
 */
void chunk_free(struct chunk *chunk)
{
	FreeVec(chunk->samples);
	chunk->samples = NULL;
}

/**
 * chunk_fill() - Fill a chunk from the ring, silence for what it cannot give, and tell the engine there is room again.
 * @out:   the output
 * @chunk: the chunk, not with AHI
 */
void chunk_fill(struct audio_out *out, struct chunk *chunk)
{
	ObtainSemaphore(&out->lock);
	ring_take(out, chunk->samples, &chunk->time);
	ReleaseSemaphore(&out->lock);
	Signal(out->setup.listener, out->setup.signal);
}

/**
 * output_open() - Take AHI: its low-level calls, or ahi.device's writes where the setup shares the unit or those are refused.
 * @out: the output, in the audio task
 *
 * Return: 0, or ahi.device's reason for refusing its writes.
 */
static LONG output_open(struct audio_out *out)
{
	if (!out->setup.shared)
	{
		out->chunk_frames = out->setup.rate / LOW_BUFFERS_A_SECOND;
		out->low_level = low_open(out) == 0;
		if (out->low_level)
			return 0;
		low_close(out);
	}
	out->chunk_frames = out->setup.rate / BLOCKS_A_SECOND;
	return device_open(out);
}

/**
 * task_entry() - The audio task: take AHI, answer, play, let go, answer again.
 */
static void task_entry(void)
{
	struct MsgPort *own = &((struct Process *)FindTask(NULL))->pr_MsgPort; /* for the start only: dos.library's from then on */
	WaitPort(own);
	struct task_message *start = (struct task_message *)GetMsg(own);
	struct audio_out *out = start->out;

	out->control = CreateMsgPort();
	start->error = out->control != NULL ? output_open(out) : ERROR_NO_FREE_STORE;
	struct Message *end = &start->message;
	if (start->error == 0)
	{
		ReplyMsg(&start->message);
		end = out->low_level ? low_play(out, out->control) : device_play(out, out->control);
	}
	if (out->low_level)
		low_close(out);
	else
		device_close(out);
	Forbid(); /* until the task is gone: the engine frees what it runs on */
	DeleteMsgPort(out->control);
	out->control = NULL;
	ReplyMsg(end);
}

/**
 * task_start() - Start the audio task and wait until it has ahi.device.
 * @out: the output
 *
 * Return: 0, or the reason the task did not come up.
 */
static LONG task_start(struct audio_out *out)
{
	out->reply_port = CreateMsgPort();
	if (out->reply_port == NULL)
		return ERROR_NO_FREE_STORE;
	out->task = CreateNewProcTags(NP_Entry, (ULONG)task_entry, NP_Name, (ULONG) "Kinora audio", NP_Priority, TASK_PRIORITY,
	                              NP_StackSize, TASK_STACK, TAG_DONE);
	if (out->task == NULL)
		return ERROR_NO_FREE_STORE;
	out->message.message.mn_ReplyPort = out->reply_port;
	out->message.message.mn_Length = sizeof(out->message);
	out->message.out = out;
	PutMsg(&out->task->pr_MsgPort, &out->message.message);
	WaitPort(out->reply_port);
	GetMsg(out->reply_port);
	if (out->message.error != 0)
		out->task = NULL; /* it answered once and is gone */
	return out->message.error;
}

/**
 * audio_out_open() - Open the sound output and start its task.
 * @setup: the unit, the samples' shape, the clock and whom to signal
 * @error: gets the reason of a refusal: a dos.library or ahi.device error code
 *
 * Return: the output, stopped and empty; NULL when refused.
 */
struct audio_out *audio_out_open(const struct audio_setup *setup, LONG *error)
{
	struct audio_out *out = AllocVec(sizeof(*out), MEMF_PUBLIC | MEMF_CLEAR);
	*error = ERROR_NO_FREE_STORE;
	if (out == NULL)
		return NULL;
	out->setup = *setup;
	out->delay = setup->delay;
	InitSemaphore(&out->lock);
	out->sample_type = setup->channels == AUDIO_SURROUND_CHANNELS ? AHIST_L7_1 : setup->wide ? AHIST_S32S : AHIST_S16S;
	out->frame_bytes = setup->channels * (setup->wide ? sizeof(LONG) : sizeof(WORD));
	out->chunk_frames = setup->rate / BLOCKS_A_SECOND; /* the larger of the two ways' sizes; the task sets its own */
	out->ring.capacity = setup->rate / RING_PART_OF_SECOND;
	out->volume = AUDIO_VOLUME_FULL;
	out->ring.samples = AllocVec(out->ring.capacity * out->frame_bytes, MEMF_PUBLIC);
	if (out->ring.samples != NULL && out->chunk_frames != 0)
		*error = task_start(out);
	if (*error != 0)
	{
		audio_out_close(out);
		return NULL;
	}
	return out;
}

/**
 * audio_out_close() - End the audio task and free the output.
 * @out: the output, whole or half made; NULL does nothing
 */
void audio_out_close(struct audio_out *out)
{
	if (out == NULL)
		return;
	if (out->task != NULL)
	{
		PutMsg(out->control, &out->message.message);
		WaitPort(out->reply_port);
		GetMsg(out->reply_port);
	}
	if (out->reply_port != NULL)
		DeleteMsgPort(out->reply_port);
	FreeVec(out->ring.samples);
	FreeVec(out);
}

/**
 * audio_out_room() - How much the ring can take.
 * @out: the output
 *
 * Return: sample frames.
 */
ULONG audio_out_room(struct audio_out *out)
{
	ObtainSemaphore(&out->lock);
	ULONG room = out->ring.capacity - out->ring.count;
	ReleaseSemaphore(&out->lock);
	return room;
}

/**
 * audio_out_queued() - How much the ring holds.
 * @out: the output
 *
 * Return: sample frames not yet handed to AHI.
 */
ULONG audio_out_queued(struct audio_out *out)
{
	return out->ring.capacity - audio_out_room(out);
}

/**
 * audio_out_write() - Put decoded samples into the ring.
 * @out:     the output
 * @samples: the sample frames, in the shape the setup named
 * @frames:  how many
 * @time:    the film's time of the first, VC_TIME_UNKNOWN to follow on from the last
 *
 * Return: how many were taken: all, or as many as the ring had room for.
 */
ULONG audio_out_write(struct audio_out *out, CONST_APTR samples, ULONG frames, VCTime time)
{
	ObtainSemaphore(&out->lock);
	/* no more than the ring has room for */
	ULONG room = out->ring.capacity - out->ring.count;
	if (frames > room)
		frames = room;

	/* the tail, where the first new frame goes, is count frames after the head, round the buffer's end */
	ULONG tail = (out->ring.head + out->ring.count) % out->ring.capacity;

	/*
	 * The frames may wrap: first those from the tail to the buffer's end,
	 * then the rest from its start, up to the head at most since frames is
	 * no more than room.  With the head after the tail (the waiting frames
	 * wrap) the room is all between the two, so nothing wraps.
	 */
	ULONG first = out->ring.capacity - tail;
	if (first > frames)
		first = frames;
	memcpy(out->ring.samples + tail * out->frame_bytes, samples, first * out->frame_bytes);
	/* 0 bytes when nothing wraps */
	memcpy(out->ring.samples, (const UBYTE *)samples + first * out->frame_bytes, (frames - first) * out->frame_bytes);

	/* the first new frame's position is the frames played plus those waiting; its time is marked unless it follows on */
	if (frames != 0 && time != VC_TIME_UNKNOWN)
		ring_mark(&out->ring, out->ring.head_position + out->ring.count, time, out->setup.rate);
	out->ring.count += frames;
	ReleaseSemaphore(&out->lock);
	return frames;
}

/**
 * audio_out_watch() - See that AHI still plays, and have the audio task start it anew when it does not.
 * @out: the output
 *
 * Called by the engine on its passes.  The audio task gives AHI silence
 * when there is nothing to play, so its chunks come back, or start, all the
 * time.
 */
void audio_out_watch(struct audio_out *out)
{
	VCTime now = clock_wall();
	if (out->watch.chunks_done != out->watch.seen_done || out->watch.seen_at == 0)
	{
		out->watch.seen_done = out->watch.chunks_done;
		out->watch.seen_at = now;
	}
	else if (now - out->watch.seen_at > STALL_TIME)
	{
		out->watch.seen_at = now;
		out->watch.step = RESTART_ASKED;
		Signal(&out->task->pr_Task, RESTART_SIGNAL);
	}
}

/**
 * audio_out_counts() - What the output has done, for the statistics.
 * @out:      the output
 * @chunks:   gets the chunks AHI played: blocks that came back, or buffers that started
 * @restarts: gets the times AHI was started anew because none did
 * @step:     gets how far the last of those got: an enum restart_step, RESTART_DONE when it was done
 */
void audio_out_counts(const struct audio_out *out, ULONG *chunks, ULONG *restarts, ULONG *step)
{
	*chunks = out->watch.chunks_done;
	*restarts = out->watch.restarts;
	*step = out->watch.step;
}

/**
 * audio_out_low_level() - Whether the sound goes through AHI's low-level calls, at the file's level.
 * @out: the output
 *
 * Return: TRUE for the low-level calls, FALSE for ahi.device's writes.
 */
BOOL audio_out_low_level(const struct audio_out *out)
{
	return out->low_level;
}

/**
 * audio_out_run() - Play what the ring holds, or silence.
 * @out: the output
 * @run: play
 *
 * What AHI already holds, 80 ms at most, is heard either way.
 */
void audio_out_run(struct audio_out *out, BOOL run)
{
	ObtainSemaphore(&out->lock);
	out->running = run;
	ReleaseSemaphore(&out->lock);
}

/**
 * audio_out_flush() - Drop what the ring holds, as at a seek.
 * @out: the output
 *
 * What AHI already holds, 80 ms at most, is still heard, but no longer says what time it is.
 */
void audio_out_flush(struct audio_out *out)
{
	ObtainSemaphore(&out->lock);
	out->ring.head = 0;
	out->ring.count = 0;
	out->ring.head_position = 0;
	out->ring.mark_count = 0;
	/* what AHI holds is sound from before: the clock is not to follow it */
	for (ULONG i = 0; i < BLOCKS; i++)
		out->device.blocks[i].chunk.time = VC_TIME_UNKNOWN;
	for (ULONG i = 0; i < LOW_BUFFERS; i++)
		out->low.buffers[i].time = VC_TIME_UNKNOWN;
	ReleaseSemaphore(&out->lock);
}

/**
 * audio_out_volume() - Set the level from the next chunk on.
 * @out:    the output
 * @volume: 0 to AUDIO_VOLUME_FULL
 */
void audio_out_volume(struct audio_out *out, ULONG volume)
{
	out->volume = volume > AUDIO_VOLUME_FULL ? AUDIO_VOLUME_FULL : volume;
}

/**
 * audio_out_delay() - Put the sound behind the picture, or ahead of it.
 * @out:   the output
 * @delay: microseconds the sound comes later; negative for earlier
 */
void audio_out_delay(struct audio_out *out, VCTime delay)
{
	ObtainSemaphore(&out->lock);
	out->delay = delay;
	ReleaseSemaphore(&out->lock);
}

/* audiocodec.library's channels in the order of AHI's 7.1 sample frame: FL FR RL RR SL SR C LFE */
static const ULONG surround_order[AUDIO_SURROUND_CHANNELS] = {
    ACCH_FrontLeft, ACCH_FrontRight, ACCH_BackLeft, ACCH_BackRight, ACCH_SideLeft, ACCH_SideRight, ACCH_FrontCenter, ACCH_LFE,
};

/**
 * audio_out_placement() - Work out where AHI's 7.1 channels are in a decoder's sample frame.
 * @layout:    ACCH_*: the decoder's channels, in the order of their bits
 * @placement: gets the placement for audio_out_place(); a channel the stream lacks is silent, one AHI has no place for is dropped
 */
void audio_out_placement(ULONG layout, struct audio_placement *placement)
{
	placement->layout = layout;
	placement->channels = 0;
	for (ULONG i = 0; i < AUDIO_SURROUND_CHANNELS; i++)
		placement->source[i] = -1;
	for (ULONG bit = 1; bit != 0; bit <<= 1)
	{
		if ((layout & bit) == 0)
			continue;
		for (ULONG i = 0; i < AUDIO_SURROUND_CHANNELS; i++)
			if (surround_order[i] == bit)
				placement->source[i] = (BYTE)placement->channels;
		placement->channels++;
	}
}

/**
 * audio_out_place() - Put decoded 32-bit sample frames into AHI's 7.1 sample frames.
 * @samples:   the decoder's sample frames (ACF_S32)
 * @frames:    how many
 * @placement: where AHI's channels are in them, worked out for their layout
 * @placed:    gets @frames sample frames of eight
 */
void audio_out_place(const LONG *samples, ULONG frames, const struct audio_placement *placement, LONG *placed)
{
	for (ULONG frame = 0; frame < frames; frame++, samples += placement->channels, placed += AUDIO_SURROUND_CHANNELS)
		for (ULONG i = 0; i < AUDIO_SURROUND_CHANNELS; i++)
			placed[i] = placement->source[i] >= 0 ? samples[placement->source[i]] : 0;
}

