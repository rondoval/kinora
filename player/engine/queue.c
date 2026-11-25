/* SPDX-License-Identifier: MPL-2.0 */
/*
 * queue.c - compressed packets waiting for a decoder (queue.h).
 */
#include <proto/demuxer.h>

#include "queue.h"

/**
 * queue_push() - Add a packet behind the others.
 * @queue:  the queue
 * @packet: the packet as DMX_ReadPacket() filled it
 *
 * Return: TRUE, FALSE when the queue is full.
 */
BOOL queue_push(struct queue *queue, const struct DMXPacket *packet)
{
	if (queue->count == QUEUE_PACKETS)
		return FALSE;
	queue->packets[(queue->head + queue->count) % QUEUE_PACKETS] = *packet;
	queue->count++;
	queue->bytes += packet->Length;
	return TRUE;
}

/**
 * queue_peek() - The oldest packet, left in the queue.
 * @queue: the queue
 *
 * Return: the packet, NULL when the queue is empty.
 */
const struct DMXPacket *queue_peek(const struct queue *queue)
{
	return queue->count != 0 ? &queue->packets[queue->head] : NULL;
}

/**
 * queue_pop() - Take the oldest packet out; releasing it is the caller's.
 * @queue: the queue, not empty
 */
void queue_pop(struct queue *queue)
{
	queue->bytes -= queue->packets[queue->head].Length;
	queue->head = (queue->head + 1) % QUEUE_PACKETS;
	queue->count--;
}

/**
 * queue_duration() - How much playing time waits.
 * @queue: the queue
 *
 * Return: from the oldest packet's decode time to the newest's; 0 with fewer than two or without times.
 */
VCTime queue_duration(const struct queue *queue)
{
	if (queue->count < 2)
		return 0;
	VCTime first = queue->packets[queue->head].Dts;
	VCTime last = queue->packets[(queue->head + queue->count - 1) % QUEUE_PACKETS].Dts;
	return first == VC_TIME_UNKNOWN || last == VC_TIME_UNKNOWN || last < first ? 0 : last - first;
}

/**
 * queue_flush() - Release every waiting packet.
 * @queue:  the queue
 * @source: the source the packets came from
 */
void queue_flush(struct queue *queue, struct DMXSource *source)
{
	while (queue->count != 0)
	{
		struct DMXPacket packet = queue->packets[queue->head];
		queue_pop(queue); /* first: releasing a packet clears it, and its length is what the queue's bytes go down by */
		DMX_ReleasePacket(source, &packet);
	}
}
