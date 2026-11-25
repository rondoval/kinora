/* SPDX-License-Identifier: MPL-2.0 */
/*
 * queue.h - compressed packets waiting for a decoder.
 *
 * The engine reads the file ahead of the decoders: about a second of each
 * stream waits here, so a slow read starves neither.  Decoded pictures are
 * not queued anywhere.  A packet in a queue is still demuxer.library's
 * memory; whoever takes it releases it.
 */
#ifndef KINORA_ENGINE_QUEUE_H
#define KINORA_ENGINE_QUEUE_H

#include <exec/types.h>
#include <libraries/demuxer.h>

#define QUEUE_PACKETS 512 /* the most packets of one stream that wait: a second of audio is some fifty, of video some sixty */

/* One stream's waiting packets, oldest first */
struct queue
{
	struct DMXPacket packets[QUEUE_PACKETS]; /* a ring */
	ULONG head;                              /* the oldest */
	ULONG count;                             /* how many wait */
	ULONG bytes;                             /* their bytes together */
};

BOOL queue_push(struct queue *queue, const struct DMXPacket *packet);
const struct DMXPacket *queue_peek(const struct queue *queue);
void queue_pop(struct queue *queue);
VCTime queue_duration(const struct queue *queue);
void queue_flush(struct queue *queue, struct DMXSource *source);

#endif /* KINORA_ENGINE_QUEUE_H */
