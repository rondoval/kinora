/* SPDX-License-Identifier: MPL-2.0 */
/*
 * engine_reader.c - the reader: the process that reads the film's packets
 * ahead into the queues, so that a slow read holds up neither the pictures
 * nor the sound (docs/architecture.md 6.1).
 *
 * The reader makes the reads and the releases.  The engine's task makes the
 * other DMX_ calls, opening, choosing streams, moving, listing and closing,
 * with the reader held (reader_hold()): file_lock, which a read holds from
 * its start until its packet is in its queue.  demuxer.library has one caller
 * at a time and the reader holds it while a read waits, so the engine does
 * not release a packet it is done with: it hands it back (reader_take()), and
 * the reader releases it before its next read.  queue_lock, held only for
 * moments, guards the queues, the file's end, the packets handed back and
 * what was read; where both are held, file_lock is taken first.
 */
#include <string.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/demuxer.h>

#include "engine_private.h"
#include "trace.h"

#define READER_STACK (512 * 1024)       /* FFmpeg's demuxers run on the reader's stack */
#define AHEAD_TIME 1000000              /* microseconds of each stream kept read ahead */
#define AHEAD_PACKETS 16u               /* and never fewer packets than this */
#define AHEAD_BYTES (32u * 1024 * 1024) /* a queue this heavy stops the reading, whatever the other lacks */

static struct engine *reader_engine; /* the engine the reader being started works for */

/**
 * queue_wants_more() - Whether a decoder's queue wants more packets.
 * @queue:       the queue
 * @has_decoder: the stream has a decoder
 *
 * Return: TRUE below the time and packets kept read ahead.
 */
static BOOL queue_wants_more(const struct queue *queue, BOOL has_decoder)
{
	return has_decoder && (queue->count < AHEAD_PACKETS || queue_duration(queue) < AHEAD_TIME);
}

/**
 * queue_full() - Whether a queue can take no more.
 * @queue: the queue
 *
 * Return: TRUE when it is full or holds AHEAD_BYTES.
 */
static BOOL queue_full(const struct queue *queue)
{
	return queue->count == QUEUE_PACKETS || queue->bytes >= AHEAD_BYTES;
}

/**
 * reading_wanted() - Whether the reader is to read another packet: queue_lock held.
 * @engine: the engine
 *
 * A file that keeps one stream's packets far from the other's makes one
 * queue grow while the other is filled; reading stops when it can take no
 * more.
 *
 * Return: TRUE before the file's end while a decoder's queue wants more and neither is full.
 */
static BOOL reading_wanted(const struct engine *engine)
{
	if (engine->file_end || queue_full(&engine->video_queue) || queue_full(&engine->audio_queue))
		return FALSE;
	return queue_wants_more(&engine->video_queue, engine->video_decoder != NULL) ||
	       queue_wants_more(&engine->audio_queue, engine->audio_decoder != NULL);
}

/**
 * reads_count() - Count a packet read for a decoder: queue_lock held.
 * @reads:  the counts
 * @video:  it is the video stream's; else the audio stream's
 * @packet: the packet
 */
static void reads_count(struct reads *reads, BOOL video, const struct DMXPacket *packet)
{
	VCTime *first = video ? &reads->video_first : &reads->audio_first;
	if (*first == VC_TIME_UNKNOWN)
		*first = packet->Pts;
	if (video)
		reads->video_bytes += packet->Length;
	else
		reads->audio_bytes += packet->Length;
}

/**
 * packet_route() - Put a packet just read into its queue: queue_lock held.
 * @engine: the engine
 * @packet: the packet
 *
 * Return: TRUE when a queue took it; else it is to be released.
 */
static BOOL packet_route(struct engine *engine, const struct DMXPacket *packet)
{
	LONG stream = (LONG)packet->Stream;
	BOOL video = engine->video_decoder != NULL && stream == engine->video_index;
	if (video || (engine->audio_decoder != NULL && stream == engine->audio_index))
	{
		reads_count(&engine->reads, video, packet);
		return queue_push(video ? &engine->video_queue : &engine->audio_queue, packet);
	}
	return stream == engine->subtitle_index && queue_push(&engine->subtitle_queue, packet);
}

/**
 * packet_read() - Read the file's next packet into its queue, or find the file's end: file_lock held.
 * @engine: the engine
 *
 * A read that fails ends the file like its end.  The engine is told either way.
 */
static void packet_read(struct engine *engine)
{
	struct DMXPacket packet = {.Size = sizeof(packet)};
	TRACE_T0(asked);
	LONG failed = DMX_ReadPacket(engine->source, &packet);
	TRACE_ADD(TRACE_READ, asked);
	TRACE_READ_ADD(asked);
	ObtainSemaphore(&engine->queue_lock);
	BOOL kept = failed == 0 && packet_route(engine, &packet);
	if (failed != 0)
		engine->file_end = TRUE;
	ReleaseSemaphore(&engine->queue_lock);
	if (failed == 0 && !kept)
		DMX_ReleasePacket(engine->source, &packet);
	Signal(&engine->process->pr_Task, 1UL << engine->reader_signal);
}

/**
 * done_release() - Release the packets the engine handed back: file_lock held.
 * @engine: the engine
 */
static void done_release(struct engine *engine)
{
	struct DMXPacket packets[DONE_PACKETS];
	ObtainSemaphore(&engine->queue_lock);
	ULONG count = engine->done_count;
	memcpy(packets, engine->done_packets, count * sizeof(packets[0]));
	engine->done_count = 0;
	ReleaseSemaphore(&engine->queue_lock);
	for (ULONG i = 0; i < count; i++)
		DMX_ReleasePacket(engine->source, &packets[i]);
}

/**
 * reader_step() - Release what the engine handed back, and read one packet if one is wanted.
 * @engine: the engine
 *
 * Return: TRUE when a packet was read.
 */
static BOOL reader_step(struct engine *engine)
{
	ObtainSemaphore(&engine->file_lock);
	done_release(engine);
	ObtainSemaphore(&engine->queue_lock);
	BOOL wanted = reading_wanted(engine);
	ReleaseSemaphore(&engine->queue_lock);
	if (wanted)
		packet_read(engine);
	ReleaseSemaphore(&engine->file_lock);
	return wanted;
}

/**
 * reader_entry() - The reader process: read while packets are wanted, else sleep, until told to end.
 *
 * Ctrl-F says something changed: packets taken or handed back, the reader
 * let go after a move.  Ctrl-C ends it; what was handed back is released first.
 */
static void reader_entry(void)
{
	struct engine *engine = reader_engine;
	for (;;)
	{
		ULONG signals = reader_step(engine) ? SetSignal(0, SIGBREAKF_CTRL_C) : Wait(SIGBREAKF_CTRL_F | SIGBREAKF_CTRL_C);
		if ((signals & SIGBREAKF_CTRL_C) != 0)
			break;
	}
	ObtainSemaphore(&engine->file_lock);
	done_release(engine);
	ReleaseSemaphore(&engine->file_lock);
	Forbid(); /* until the process is gone: the engine may close the file and free the engine */
	engine->reader_gone = TRUE;
	Signal(&engine->process->pr_Task, 1UL << engine->reader_signal);
}

/**
 * reader_start() - Start the reader for the open file, one priority below the engine.
 * @engine: the engine, a file open and its decoders made
 *
 * Return: TRUE, FALSE when the process could not be made.
 */
BOOL reader_start(struct engine *engine)
{
	reader_engine = engine;
	engine->reader_gone = FALSE;
	engine->reader = CreateNewProcTags(NP_Entry, (ULONG)reader_entry,
	                                   NP_Name, (ULONG) "Kinora reader",
	                                   NP_Priority, (ULONG)(LONG)(engine->process->pr_Task.tc_Node.ln_Pri - 1),
	                                   NP_StackSize, READER_STACK,
	                                   TAG_DONE);
	return engine->reader != NULL;
}

/**
 * reader_end() - End the reader and wait until it is gone.
 * @engine: the engine; without a reader nothing happens
 *
 * A read under way is waited out.
 */
void reader_end(struct engine *engine)
{
	if (engine->reader == NULL)
		return;
	Signal(&engine->reader->pr_Task, SIGBREAKF_CTRL_C);
	while (!engine->reader_gone)
		Wait(1UL << engine->reader_signal);
	engine->reader = NULL;
}

/**
 * reader_hold() - Stop the reader between two reads, for the engine's own DMX_ calls.
 * @engine: the engine, a file open
 *
 * A read under way is waited out.  What was handed back is released; until
 * reader_let_go(), nothing is read.  Holds nest.
 */
void reader_hold(struct engine *engine)
{
	ObtainSemaphore(&engine->file_lock);
	done_release(engine);
}

/**
 * reader_let_go() - Let the reader read again, and wake it to look.
 * @engine: the engine, the reader held
 */
void reader_let_go(struct engine *engine)
{
	ReleaseSemaphore(&engine->file_lock);
	if (engine->reader != NULL)
		Signal(&engine->reader->pr_Task, SIGBREAKF_CTRL_F);
}

/**
 * queue_next() - A queue's first packet, and whether the file has been read to its end.
 * @engine: the engine
 * @queue:  the queue
 * @ended:  gets whether the file's end was read
 *
 * The packet stays the queue's first until reader_take(): the reader adds
 * only behind the last.
 *
 * Return: the packet, NULL for an empty queue.
 */
static const struct DMXPacket *queue_next(struct engine *engine, struct queue *queue, BOOL *ended)
{
	ObtainSemaphore(&engine->queue_lock);
	const struct DMXPacket *packet = queue_peek(queue);
	*ended = engine->file_end;
	ReleaseSemaphore(&engine->queue_lock);
	return packet;
}

/**
 * reader_next() - A decoder's queue's first packet, and whether the file has been read to its end.
 * @engine: the engine
 * @queue:  the video or the audio queue
 * @ended:  gets whether the file's end was read
 *
 * As queue_next(); an empty queue wakes the reader, which may have stopped
 * while no decoder wanted packets.
 *
 * Return: the packet, NULL for an empty queue.
 */
const struct DMXPacket *reader_next(struct engine *engine, struct queue *queue, BOOL *ended)
{
	const struct DMXPacket *packet = queue_next(engine, queue, ended);
	if (packet == NULL && !*ended && engine->reader != NULL)
		Signal(&engine->reader->pr_Task, SIGBREAKF_CTRL_F);
	return packet;
}

/**
 * reader_take() - Take a queue's first packet away, done with: the reader releases it.
 * @engine: the engine
 * @queue:  the queue, not empty
 *
 * Without a reader, or with no room for one more handed back, the packet is
 * released here, the reader held for it.
 */
void reader_take(struct engine *engine, struct queue *queue)
{
	ObtainSemaphore(&engine->queue_lock);
	struct DMXPacket packet = *queue_peek(queue);
	queue_pop(queue);
	BOOL handed = engine->reader != NULL && engine->done_count < DONE_PACKETS;
	if (handed)
		engine->done_packets[engine->done_count++] = packet;
	ReleaseSemaphore(&engine->queue_lock);
	if (handed)
	{
		Signal(&engine->reader->pr_Task, SIGBREAKF_CTRL_F);
		return;
	}
	reader_hold(engine);
	DMX_ReleasePacket(engine->source, &packet);
	reader_let_go(engine);
}

/**
 * reader_flush() - Release what a queue holds: the reader held.
 * @engine: the engine, the reader held (reader_hold())
 * @queue:  the queue
 */
void reader_flush(struct engine *engine, struct queue *queue)
{
	ObtainSemaphore(&engine->queue_lock);
	queue_flush(queue, engine->source);
	ReleaseSemaphore(&engine->queue_lock);
}

/**
 * reader_subtitles() - Make cues of the subtitle packets read, and hand them back.
 * @engine: the engine, a file open
 */
void reader_subtitles(struct engine *engine)
{
	BOOL ended;
	const struct DMXPacket *packet;
	while ((packet = queue_next(engine, &engine->subtitle_queue, &ended)) != NULL)
	{
		subtitles_add_packet(engine->subtitles, engine->subtitle_codec, packet);
		reader_take(engine, &engine->subtitle_queue);
	}
}

/**
 * reader_reads_take() - What the reader counted since the last call: its bytes start again from 0.
 * @engine: the engine
 * @reads:  gets the counts
 */
void reader_reads_take(struct engine *engine, struct reads *reads)
{
	ObtainSemaphore(&engine->queue_lock);
	*reads = engine->reads;
	engine->reads.video_bytes = 0;
	engine->reads.audio_bytes = 0;
	ReleaseSemaphore(&engine->queue_lock);
}

/**
 * reader_reads_reset() - Forget what was read before: counts and first times, for a new file or a move.
 * @engine: the engine, the reader held or none running
 */
void reader_reads_reset(struct engine *engine)
{
	ObtainSemaphore(&engine->queue_lock);
	engine->reads = (struct reads){.video_first = VC_TIME_UNKNOWN, .audio_first = VC_TIME_UNKNOWN};
	ReleaseSemaphore(&engine->queue_lock);
}
