/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * demuxer_order.c - the times MPEG video's pictures are shown at.
 *
 * MPEG-1, MPEG-2 and MPEG-4 Part 2 send a B-picture after both reference
 * pictures (I, P, S) it lies between, and show it before the later one.  Most
 * containers give each picture the time it is shown.  AVI gives each its place
 * in the file, its frame number in decode order, and so do Matroska files
 * made from AVI and by libxvid: then a reference and the B-pictures after it
 * carry their times in decode order, where the B-pictures, shown first, should
 * have the earlier ones and the reference the last.
 *
 * So a stream's reference picture is held, with what follows it, until the
 * next reference comes: a run.  When a B-picture of the run has a later time
 * than its reference, the run's times are rotated by one: each B-picture takes
 * the time of the picture before it, the reference the last B-picture's.  A run
 * whose B-pictures come earlier than its reference is as the file says.  A
 * picture's time here is its presentation time where the file gives one, else
 * its decode time: AVI's B-pictures have one, its references do not, and a
 * packet that leaves without one has its decode time.
 *
 * The picture types serve twice more.  A programme stream marks no packet as
 * a key frame, so I-pictures are marked here.  And its moves land between key
 * frames, so after the open and after every move the packets before the first
 * I-picture are dropped: the decoder would draw them over nothing.  So are
 * the B-pictures between that I-picture and the next reference: in an open
 * group of pictures they are shown before it and decoded from the picture
 * before the move, which was not read.  They stay in the run until its times
 * are settled, since in decode order they hold times the I-picture takes.
 */
#include <string.h>

#include "demuxer_private.h"

#define START_CODE_BYTES 4         /* a start code: 00 00 01 and its code byte */
#define VOP_START_CODE 0xb6u       /* MPEG-4 Part 2: a picture (VOP) starts */
#define VOP_HEADER_BYTES 1         /* the VOP header's bytes read after its start code */
#define VOP_TYPE_SHIFT 6           /* its coding type: the top two bits of the first of them */
#define PICTURE_START_CODE 0x00u   /* MPEG-1 and MPEG-2: a picture starts */
#define PICTURE_HEADER_BYTES 2     /* the picture header's bytes read after its start code */
#define PICTURE_TYPE_SHIFT 3       /* its coding type: bits 5 to 3 of the second of them */
#define PICTURE_TYPE_MASK 7u
#define NVOP_BYTES_MOST 19         /* an MPEG-4 packet this small holds a picture not coded (mpeg4_unpack_bframes's rule) */

/* A VOP's coding types (0 I, 1 P, 2 B, 3 S) and a picture's (1 I, 2 P, 3 B, 4 D, DC-only intra) as kinds */
static const enum picture_kind vop_kinds[] = {
    PICTURE_INTRA, PICTURE_PREDICTED, PICTURE_BIDIRECTIONAL, PICTURE_PREDICTED,
};
static const enum picture_kind picture_kinds[] = {PICTURE_NONE, PICTURE_INTRA, PICTURE_PREDICTED, PICTURE_BIDIRECTIONAL,
                                                  PICTURE_INTRA, PICTURE_NONE, PICTURE_NONE, PICTURE_NONE};

/**
 * order_needed() - Whether a stream needs an order: its codec sends B-pictures after the pictures they are shown before.
 * @parameters: the stream's codec parameters
 *
 * Return: TRUE for MPEG-1, MPEG-2 and MPEG-4 Part 2 video, FALSE for every other stream.
 */
BOOL order_needed(const AVCodecParameters *parameters)
{
	return parameters->codec_id == AV_CODEC_ID_MPEG1VIDEO || parameters->codec_id == AV_CODEC_ID_MPEG2VIDEO ||
	       parameters->codec_id == AV_CODEC_ID_MPEG4;
}

/**
 * order_make() - Make an empty order for one stream.
 * @parameters: the stream's codec parameters, which order_needed() takes
 *
 * The order starts out waiting for an I-picture: the packets before the
 * stream's first one are dropped (start_passes()).
 *
 * Return: the order, which order_free() frees; NULL when out of memory.
 */
struct order *order_make(const AVCodecParameters *parameters)
{
	struct order *order = av_mallocz(sizeof(*order));
	if (order == NULL)
		return NULL;
	order->codec = parameters->codec_id;
	order->start = ORDER_BEFORE_INTRA;
	return order;
}

/**
 * order_flush() - Drop every packet an order holds, after a move in the file.
 * @order: the order
 *
 * The order is then as order_make() made it: empty, and waiting for an I-picture.
 */
void order_flush(struct order *order)
{
	for (unsigned int i = 0; i < order->count; i++)
		av_packet_free(&order->held[i].packet);
	order->count = 0;
	order->ready = 0;
	order->start = ORDER_BEFORE_INTRA;
}

/**
 * order_free() - Free an order and the packets it still holds.
 * @order: the variable that points to the order, set to NULL after; a NULL order is left alone
 */
void order_free(struct order **order)
{
	if (*order == NULL)
		return;
	order_flush(*order);
	av_freep(order);
}

/**
 * start_code_find() - Find a packet's first start code with a given code byte.
 * @packet: the packet to search
 * @code:   the byte after the start code's prefix 00 00 01: 0x00 for an MPEG-1 or MPEG-2 picture, 0xb6 for an MPEG-4 VOP
 * @after:  how many bytes of the header must follow it within the packet; a start code nearer the end is passed over
 *
 * Return: the offset of the start code's first byte, -1 when the packet has none.
 */
static int start_code_find(const AVPacket *packet, unsigned int code, int after)
{
	const uint8_t *data = packet->data;
	for (int i = 0; i + START_CODE_BYTES + after <= packet->size; i++)
		if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 && data[i + 3] == code)
			return i;
	return -1;
}

/**
 * kind_of() - Tell what kind of picture a packet holds, from the coding type in its first picture header.
 * @order:  the order, for the stream's codec
 * @packet: the packet
 *
 * MPEG-4 Part 2 has the coding type in the two bits after the VOP's start
 * code, MPEG-1 and MPEG-2 in three bits of the picture header.  An MPEG-4
 * packet of NVOP_BYTES_MOST bytes or fewer holds a picture that is not coded,
 * DivX's placeholder, which counts as none, as it does for mpeg4_unpack_bframes.
 *
 * Return: PICTURE_INTRA, PICTURE_PREDICTED or PICTURE_BIDIRECTIONAL; PICTURE_NONE
 * for a packet without a picture header or with a picture that is not coded.
 */
static enum picture_kind kind_of(const struct order *order, const AVPacket *packet)
{
	BOOL vop = order->codec == AV_CODEC_ID_MPEG4;
	if (vop && packet->size <= NVOP_BYTES_MOST)
		return PICTURE_NONE; /* DivX's placeholder: a VOP header that codes nothing */
	/* other headers may come first (MPEG-4's VOL and GOV, MPEG-2's sequence and GOP): the first picture header tells */
	int offset = start_code_find(packet, vop ? VOP_START_CODE : PICTURE_START_CODE,
	                             vop ? VOP_HEADER_BYTES : PICTURE_HEADER_BYTES);
	if (offset < 0)
		return PICTURE_NONE;
	const uint8_t *header = packet->data + offset + START_CODE_BYTES;
	if (vop)
		return vop_kinds[header[0] >> VOP_TYPE_SHIFT];
	return picture_kinds[header[1] >> PICTURE_TYPE_SHIFT & PICTURE_TYPE_MASK]; /* after the 10-bit temporal reference */
}

/**
 * is_reference() - Whether a kind of picture is a reference picture, one that other pictures are decoded from.
 * @kind: the kind
 *
 * A reference picture ends the run before it and starts one.
 *
 * Return: TRUE for an I-, P- or S-picture, FALSE for a B-picture or no picture.
 */
static BOOL is_reference(enum picture_kind kind)
{
	return kind == PICTURE_INTRA || kind == PICTURE_PREDICTED;
}

/**
 * file_time() - The time the file gave a packet, which a rotation may hand on to another picture of its run.
 * @packet: the packet
 *
 * It is the packet's presentation time where the file gives one, else its
 * decode time: AVI gives presentation times to its B-pictures only.
 *
 * Return: the time in the stream's time base, AV_NOPTS_VALUE when the packet has neither.
 */
static int64_t file_time(const AVPacket *packet)
{
	return packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
}

/**
 * run_rotate() - Give the pictures of a run whose times are in decode order the times they are shown at.
 * @order: the order, holding a run: its reference picture at held[ready], then the packets that followed it
 *
 * A run is shown as its B-pictures first, the reference picture last, and the
 * times the file gave it, in decode order, belong to its pictures in that
 * order.  So the first B-picture takes the reference picture's time, each
 * further B-picture the time of the B-picture before it, and the reference
 * picture the last B-picture's.  Packets of the run that hold no B-picture
 * keep theirs.  Every picture of the run has a time (run_in_decode_order()).
 */
static void run_rotate(struct order *order)
{
	AVPacket *reference = order->held[order->ready].packet;
	/* in decode order, each B-picture takes the time of the picture before it and hands its own on */
	int64_t previous_time = file_time(reference);
	for (unsigned int i = order->ready + 1; i < order->count; i++)
	{
		if (order->held[i].kind != PICTURE_BIDIRECTIONAL)
			continue; /* no picture: its time is none of the run's */
		AVPacket *packet = order->held[i].packet;
		int64_t own_time = file_time(packet);
		packet->pts = previous_time;
		previous_time = own_time;
	}
	reference->pts = previous_time; /* shown last: the last B-picture's time */
}

/**
 * run_in_decode_order() - Whether the times of the run an order holds are in decode order, so that it must be rotated.
 * @order: the order, holding a run
 *
 * Where the file gives the times pictures are shown at, a run's B-pictures
 * come before its reference picture; a B-picture with a later time than the
 * reference picture means the times are the decode order, as AVI's frame
 * numbers are.
 *
 * Return: TRUE when a B-picture of the run has a later time than its reference
 * picture and every picture of the run has a time; FALSE when the run keeps
 * the times the file gave it.
 */
static BOOL run_in_decode_order(const struct order *order)
{
	int64_t reference_time = file_time(order->held[order->ready].packet);
	BOOL later = FALSE; /* a B-picture of the run comes after its reference in time */
	for (unsigned int i = order->ready + 1; i < order->count; i++)
	{
		if (order->held[i].kind != PICTURE_BIDIRECTIONAL)
			continue;
		int64_t own_time = file_time(order->held[i].packet);
		if (own_time == AV_NOPTS_VALUE)
			return FALSE; /* a time missing: nothing to rotate with */
		later |= own_time > reference_time;
	}
	return later && reference_time != AV_NOPTS_VALUE;
}

/**
 * run_settle() - Settle the times of the run an order holds, if it holds one, and make all its packets ready.
 * @order: the order
 *
 * Called when the next reference picture comes and at the stream's end.  A
 * run in decode order is rotated (run_rotate()); either way order_take()
 * gives out every packet held from then on.
 */
static void run_settle(struct order *order)
{
	BOOL run_open = order->ready < order->count; /* the packets past the ready ones are the run */
	if (run_open && run_in_decode_order(order))
		run_rotate(order);
	order->ready = order->count;
}

/**
 * start_passes() - Move the stream's start on by one packet, and decide whether the packet passes to the caller.
 * @order: the order, whose start (enum order_start) this moves on
 * @kind:  what the packet holds (kind_of())
 *
 * After the open and after every move (order_flush()) the order waits for an
 * I-picture and drops everything before it: the decoder would draw it over
 * nothing.  From that I-picture to the next reference picture it drops the
 * B-pictures: in an open group of pictures they are decoded from a picture
 * before the move, which was not read.  After that every packet passes.
 *
 * Return: TRUE when the packet passes, FALSE when it is dropped.
 */
static BOOL start_passes(struct order *order, enum picture_kind kind)
{
	/* the first I-picture ends the wait, the next reference after it the dropping of B-pictures */
	if (order->start == ORDER_BEFORE_INTRA && kind == PICTURE_INTRA)
		order->start = ORDER_AFTER_INTRA;
	else if (order->start == ORDER_AFTER_INTRA && is_reference(kind))
		order->start = ORDER_FLOWING;
	/* before the I-picture nothing passes, after it all but the B-pictures, from the next reference on everything */
	return order->start == ORDER_FLOWING || (order->start == ORDER_AFTER_INTRA && kind != PICTURE_BIDIRECTIONAL);
}

/**
 * order_put() - Give an order the stream's next packet, in the order the file holds them.
 * @order:  the order, which has given out every packet that was ready (order_take())
 * @packet: the packet, the order's from now
 *
 * An I-picture is marked as a key frame, since a programme stream marks no
 * packet as one.  A packet that comes before the stream's first I-picture
 * since the open or the move is freed at once.  A B-picture dropped right
 * after that I-picture stays in its run until the run's times are settled,
 * since it carries one of them, and order_take() frees it then.  A reference
 * picture settles the run before it and starts one.  A packet that comes
 * while no run is open (a B-picture before any reference picture) is ready at
 * once with its own time, and a run that fills the order (ORDER_HELD_MOST)
 * goes with the times the file gave it.
 */
void order_put(struct order *order, AVPacket *packet)
{
	enum picture_kind kind = kind_of(order, packet);
	if (kind == PICTURE_INTRA)
		packet->flags |= AV_PKT_FLAG_KEY; /* a programme stream marks none */

	BOOL passes = start_passes(order, kind);
	if (order->start == ORDER_BEFORE_INTRA)
	{
		av_packet_free(&packet); /* before the first I-picture: dropped, and no run to time yet */
		return;
	}

	/* from here a packet that does not pass is a B-picture right after the I-picture: it carries a time of its run */
	if (is_reference(kind))
		run_settle(order); /* the run before it is whole */

	BOOL run_open = order->ready < order->count;
	order->held[order->count++] = (struct order_entry){.packet = packet, .kind = kind, .passes = passes};
	if (!run_open && !is_reference(kind))
		order->ready = order->count; /* no reference before it to wait for: it goes with its own time */
	else if (order->count == ORDER_HELD_MOST)
		order->ready = order->count; /* a run longer than the order holds goes as the file timed it */
}

/**
 * order_end() - Settle the stream's last run at the file's end, since no next reference picture will.
 * @order: the order
 */
void order_end(struct order *order)
{
	run_settle(order);
}

/**
 * order_take() - Take the next packet whose time is settled, in decode order.
 * @order: the order
 *
 * Packets that were dropped and stayed to time their run are freed on the
 * way.  A packet still without a presentation time gets its decode time: AVI
 * gives its reference pictures none once FFmpeg has seen B-pictures in the
 * stream, and a reference picture with no B-picture after it is shown at its
 * own place in the file.
 *
 * Return: the packet, the caller's from now; NULL when none is ready.
 */
AVPacket *order_take(struct order *order)
{
	while (order->ready != 0)
	{
		struct order_entry entry = order->held[0];
		order->count--;
		order->ready--;
		memmove(&order->held[0], &order->held[1], order->count * sizeof(order->held[0])); /* ORDER_HELD_MOST at most */
		if (!entry.passes)
		{
			av_packet_free(&entry.packet); /* a B-picture kept only to time its run */
			continue;
		}
		if (entry.packet->pts == AV_NOPTS_VALUE)
			entry.packet->pts = entry.packet->dts; /* AVI's reference pictures: shown at their place in the file */
		return entry.packet;
	}
	return NULL;
}
