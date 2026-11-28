/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_session.c - a session (docs/architecture.md, section 14.1): its objects, the client's
 * calls, and the items the client polls.
 *
 * The client's calls never wait for a picture: a packet sent joins the queue the session's
 * process takes from (codec_process.c), and what the process finishes waits here as items,
 * in order, until polled.  Every function here takes the session's lock or is called with
 * it held; only the open and the close take FFmpeg's.
 */
#include <stddef.h>
#include <string.h>

#include <exec/memory.h>
#include <proto/exec.h>

#include <amiga_library.h>
#include <libavutil/mem.h>

#include "codec_private.h"

#define MICROSECONDS_PER_SECOND 1000000
#define EVENTS_ROOM 16u /* items beside packets and frames: stream infos, ends, an error */

static struct SignalSemaphore sessions_lock; /* the list of sessions and the totals of closed ones */
static struct MinList sessions;              /* every open session, by its listing node */
static struct counts closed_counts;          /* what closed sessions counted */
static ULONG session_total;                  /* open sessions */

/**
 * sessions_init() - Make the backend's list of sessions; the library's init calls it.
 */
void sessions_init(void)
{
	InitSemaphore(&sessions_lock);
	NewMinList(&sessions);
}

/* ---- items ------------------------------------------------------------------------ */

/**
 * items_add() - Hand the client an item: queue it, and signal the client.
 * @session: the session, its lock held
 * @item:    the item
 *
 * The ring has room for every packet and frame the session has and a few events, so it
 * never fills; an item past its room would be lost, and is dropped.
 */
void items_add(struct session *session, const struct VCItem *item)
{
	struct items *items = &session->items;
	if (items->count == items->room)
		return;
	items->item[(items->head + items->count) % items->room] = *item;
	items->count++;
	if (session->sig_task != NULL && session->sig_mask != 0)
		Signal(session->sig_task, session->sig_mask);
}

/**
 * items_event() - Hand the client an event.
 * @session: the session, its lock held
 * @event:   VCEV_*
 * @status:  VCERR_* with VCEV_Error, else 0
 * @stream:  the stream's shape with VCEV_StreamInfo, else NULL
 */
void items_event(struct session *session, ULONG event, LONG status, const struct VCStreamInfo *stream)
{
	struct VCItem item;
	memset(&item, 0, sizeof(item));
	item.Type = VCIT_Event;
	item.u.Event.Event = event;
	item.u.Event.Status = status;
	if (stream != NULL)
		item.u.Event.Stream = *stream;
	items_add(session, &item);
}

/**
 * session_fail() - Fail the session: VCEV_Error, once; only the close is left.
 * @session: the session, its lock held
 * @status:  VCERR_*
 */
void session_fail(struct session *session, LONG status)
{
	if (session->failed)
		return;
	session->failed = TRUE;
	items_event(session, VCEV_Error, status, NULL);
}

/**
 * packet_return() - Give an input packet back to the client, empty.
 * @session: the session, its lock held
 * @packet:  the packet, out of the queue
 */
void packet_return(struct session *session, struct packet *packet)
{
	struct VCItem item;
	memset(&item, 0, sizeof(item));
	packet->sent = FALSE;
	packet->pub.Length = 0;
	item.Type = VCIT_PacketReturned;
	item.u.Packet = &packet->pub;
	items_add(session, &item);
}

/* ---- the open and the close ------------------------------------------------------------- */

/**
 * clamped() - A count asked for, or the default, held to a range.
 * @asked:   the client's, 0 for none
 * @fallback: the default
 * @least:   the smallest allowed
 * @most:    the largest allowed
 *
 * Return: the count.
 */
static ULONG clamped(ULONG asked, ULONG fallback, ULONG least, ULONG most)
{
	ULONG count = asked != 0 ? asked : fallback;
	return count < least ? least : count > most ? most : count;
}

/**
 * params_refused() - Whether a session asks for what this decoder cannot do.
 * @params: the session's parameters
 *
 * It decodes to I420 at the stream's own size into frames of its own: no other format, no
 * scaling, no place in a frame (videocodec.library puts a converter behind it for those).
 *
 * Return: TRUE when refused.
 */
static BOOL params_refused(const struct VCSessionParams *params)
{
	if (params->Kind != VCK_Decoder)
		return TRUE;
	if (params->OutputFormat != 0 && params->OutputFormat != VCF_I420)
		return TRUE;
	return params->OutputWidth != 0 || params->OutputHeight != 0 || params->HaveDestRect || params->KeepAspect;
}

/**
 * packets_make() - The session's input packets, each handed to the client at once.
 * @session: the session
 * @params:  its parameters
 *
 * Return: 0, or VCERR_NOMEM.
 */
static LONG packets_make(struct session *session, const struct VCSessionParams *params)
{
	ULONG count = clamped(params->InputCount, PACKETS_DEFAULT, 1, PACKETS_MOST);
	ULONG size = params->InputSize != 0 ? params->InputSize : PACKET_BYTES_DEFAULT;
	session->packet = AllocVec(count * sizeof(*session->packet), MEMF_PUBLIC | MEMF_CLEAR);
	if (session->packet == NULL)
		return VCERR_NOMEM;
	session->packet_count = count;
	for (ULONG i = 0; i < count; i++)
	{
		struct packet *packet = &session->packet[i];
		packet->pub.Size = sizeof(packet->pub);
		packet->pub.Data = AllocVec(size + AV_INPUT_BUFFER_PADDING_SIZE, MEMF_PUBLIC); /* FFmpeg's padding: the parser reads it in place */
		packet->pub.Capacity = size;
		packet->pub.Pts = VC_TIME_UNKNOWN;
		packet->owner = session;
		if (packet->pub.Data == NULL)
			return VCERR_NOMEM;
	}
	for (ULONG i = 0; i < count; i++)
		packet_return(session, &session->packet[i]);
	return 0;
}

/**
 * extradata_copy() - Give the decoder its own copy of the codec's configuration.
 * @decoder: the decoder, not yet open
 * @params:  the session's parameters
 *
 * Return: TRUE, FALSE without memory.
 */
static BOOL extradata_copy(AVCodecContext *decoder, const struct VCSessionParams *params)
{
	if (params->ExtradataSize == 0 || params->Extradata == NULL)
		return TRUE;
	decoder->extradata = av_mallocz(params->ExtradataSize + AV_INPUT_BUFFER_PADDING_SIZE);
	if (decoder->extradata == NULL)
		return FALSE;
	memcpy(decoder->extradata, params->Extradata, params->ExtradataSize);
	decoder->extradata_size = (int)params->ExtradataSize;
	return TRUE;
}

/**
 * ffmpeg_tag() - A FourCC as FFmpeg keeps one: the first character in the low byte.
 * @tag: VC_FOURCC(), the first character in the top byte
 *
 * Return: FFmpeg's codec_tag.
 */
static unsigned int ffmpeg_tag(ULONG tag)
{
	return (unsigned int)(((tag >> 24) & 0xffu) | ((tag >> 8) & 0xff00u) | ((tag << 8) & 0xff0000u) | ((tag << 24) & 0xff000000u));
}

/**
 * decoder_make() - FFmpeg's decoder for a session, open, and its parser where one is needed.
 * @session: the session
 * @codec:   FFmpeg's decoder
 * @params:  the session's parameters
 *
 * The container's FourCC goes to FFmpeg as the codec tag, which picks bug workarounds and the
 * IDCT (Xvid's for XVID); its size goes as the picture's, which MS-MPEG4 streams do not carry.
 * Times are microseconds.  FFmpeg's lock is held.
 *
 * Return: 0, VCERR_NOMEM or VCERR_UNSUPPORTED.
 */
static LONG decoder_make(struct session *session, const AVCodec *codec, const struct VCSessionParams *params)
{
	AVCodecContext *decoder = avcodec_alloc_context3(codec);
	session->decoder = decoder;
	session->picture = av_packet_alloc();
	session->decoded = av_frame_alloc();
	if (decoder == NULL || session->picture == NULL || session->decoded == NULL || !extradata_copy(decoder, params))
		return VCERR_NOMEM;
	decoder->opaque = session;
	decoder->get_buffer2 = frames_get_buffer;
	decoder->thread_count = 1;
	decoder->pkt_timebase = (AVRational){1, MICROSECONDS_PER_SECOND};
	decoder->codec_tag = ffmpeg_tag(params->CodecTag);
	decoder->width = (int)params->Width;
	decoder->height = (int)params->Height;
	if (avcodec_open2(decoder, codec, NULL) != 0)
		return VCERR_UNSUPPORTED;
	if (!params->Framed && table_parsed(params->Codec))
	{
		session->parser = av_parser_init((int)codec->id);
		if (session->parser == NULL)
			return VCERR_NOMEM;
	}
	return 0;
}

/**
 * session_free() - Free a session, whole or half made, FFmpeg's decoder gone and its process too.
 * @session: the session
 */
static void session_free(struct session *session)
{
	frames_dispose_all(session);
	for (ULONG i = 0; i < session->packet_count; i++)
		FreeVec(session->packet[i].pub.Data);
	FreeVec(session->packet);
	FreeVec(session->items.item);
	FreeVec(session);
}

/**
 * decoder_free() - Close FFmpeg's decoder and its parser; FFmpeg's lock is held.
 * @session: the session
 *
 * Every reference FFmpeg holds to a frame goes, so the frames are FFmpeg's no more.
 */
static void decoder_free(struct session *session)
{
	if (session->parser != NULL)
		av_parser_close(session->parser);
	session->parser = NULL;
	av_frame_free(&session->decoded);
	av_packet_free(&session->picture);
	avcodec_free_context(&session->decoder);
}

/**
 * session_make() - A session's memory and objects, FFmpeg's decoder not yet.
 * @params: the session's parameters
 * @error:  gets the VCERR_* of a refusal
 *
 * Return: the session, NULL without memory.
 */
static struct session *session_make(const struct VCSessionParams *params, LONG *error)
{
	struct session *session = AllocVec(sizeof(*session), MEMF_PUBLIC | MEMF_CLEAR);
	*error = VCERR_NOMEM;
	if (session == NULL)
		return NULL;
	InitSemaphore(&session->lock);
	NewMinList(&session->queue);
	NewMinList(&session->sets);
	session->sig_task = params->SigTask;
	session->sig_mask = params->SigMask;
	session->skip = VC_HAS_FIELD(params->Size, struct VCSessionParams, Skip) ? params->Skip : VCSK_None;
	session->max_area = table_max_area(params->Codec);
	session->frame_count = clamped(params->OutputCount, FRAMES_DEFAULT, FRAMES_LEAST, FRAMES_MOST);
	session->frame_rate = params->FrameRate;
	session->color_space = params->ColorSpace;
	ULONG packets = clamped(params->InputCount, PACKETS_DEFAULT, 1, PACKETS_MOST);
	session->items.room = packets + 3 * FRAMES_MOST + EVENTS_ROOM;
	session->items.item = AllocVec(session->items.room * sizeof(struct VCItem), MEMF_PUBLIC | MEMF_CLEAR);
	if (session->items.item == NULL || packets_make(session, params) != 0)
	{
		session_free(session);
		return NULL;
	}
	*error = 0;
	return session;
}

/**
 * session_list() - Put a session in the backend's list, or take it out.
 * @session: the session
 * @in:      TRUE to put it in; FALSE to take it out and keep its counts in the totals
 */
static void session_list(struct session *session, BOOL in)
{
	ObtainSemaphore(&sessions_lock);
	if (in)
	{
		AddTail((struct List *)&sessions, (struct Node *)&session->listing);
		session_total++;
	}
	else
	{
		Remove((struct Node *)&session->listing);
		session_total--;
		closed_counts.pictures += session->counts.pictures;
		closed_counts.damaged += session->counts.damaged;
		closed_counts.waits += session->counts.waits;
	}
	ReleaseSemaphore(&sessions_lock);
}

/**
 * session_open() - Open a decoder: its objects, FFmpeg's decoder, its process.
 * @params: the session's parameters, checked by the frontend
 * @error:  gets the VCERR_* of a refusal
 *
 * Return: the session, NULL when refused.
 */
struct session *session_open(const struct VCSessionParams *params, LONG *error)
{
	if (table_max_area(params->Codec) == 0 || params_refused(params))
	{
		*error = VCERR_UNSUPPORTED;
		return NULL;
	}
	struct session *session = session_make(params, error);
	if (session == NULL)
		return NULL;

	volatile LONG why = VCERR_UNSUPPORTED; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
	{
		const AVCodec *codec = table_decoder(params->Codec, params->CodecTag);
		why = codec != NULL ? decoder_make(session, codec, params) : VCERR_UNSUPPORTED;
	}
	ffmpeg_leave();
	if (why == 0)
		why = process_start(session);
	if (why != 0)
	{
		if (FFMPEG_ENTER())
			decoder_free(session);
		ffmpeg_leave();
		session_free(session);
		*error = why;
		return NULL;
	}
	session_list(session, TRUE);
	*error = 0;
	return session;
}

/**
 * session_close() - Close a decoder: its process ends first, then FFmpeg's decoder goes, then the rest.
 * @session: the session
 *
 * The process may be inside FFmpeg: it ends its call first, so FFmpeg's lock is taken only
 * after it is gone.
 */
void session_close(struct session *session)
{
	process_stop(session);
	if (FFMPEG_ENTER())
		decoder_free(session);
	ffmpeg_leave();
	session_list(session, FALSE);
	session_free(session);
}

/* ---- the client's calls ------------------------------------------------------------------ */

/**
 * packet_of() - One of a session's input packets from the client's pointer.
 * @session: the session
 * @pub:     the client's pointer
 *
 * Return: the packet, NULL for a pointer that is not one of the session's.
 */
static struct packet *packet_of(struct session *session, struct VCPacket *pub)
{
	for (ULONG i = 0; i < session->packet_count; i++)
		if (&session->packet[i].pub == pub)
			return &session->packet[i];
	return NULL;
}

/**
 * packet_of_node() - The packet a queue's node is part of.
 * @node: its node
 *
 * Return: the packet.
 */
struct packet *packet_of_node(struct Node *node)
{
	return (struct packet *)((UBYTE *)node - offsetof(struct packet, node));
}

/**
 * session_send() - Queue a packet for the session's process.
 * @session: the session
 * @pub:     one of its input packets, filled
 *
 * Return: 0, VCERR_BADPARAM for another's packet or more bytes than it holds, VCERR_BADSTATE
 * for one already sent or a failed session.
 */
LONG session_send(struct session *session, struct VCPacket *pub)
{
	LONG error = 0;
	ObtainSemaphore(&session->lock);
	struct packet *packet = packet_of(session, pub);
	if (packet == NULL || pub->Length > pub->Capacity)
		error = VCERR_BADPARAM;
	else if (packet->sent || session->failed)
		error = VCERR_BADSTATE;
	else
	{
		packet->sent = TRUE;
		AddTail((struct List *)&session->queue, (struct Node *)&packet->node);
	}
	struct Process *process = session->process;
	ReleaseSemaphore(&session->lock);
	if (error == 0 && process != NULL)
		Signal(&process->pr_Task, WORK_SIGNAL);
	return error;
}

/**
 * session_poll() - Take the next item for the client.
 * @session: the session
 * @item:    filled in
 *
 * Items a flush emptied (VCIT_None) are passed over.
 *
 * Return: 1 with @item filled, 0 when nothing waits.
 */
LONG session_poll(struct session *session, struct VCItem *item)
{
	LONG found = 0;
	ObtainSemaphore(&session->lock);
	struct items *items = &session->items;
	while (items->count != 0 && !found)
	{
		*item = items->item[items->head];
		items->head = (items->head + 1) % items->room;
		items->count--;
		found = item->Type != VCIT_None;
	}
	ReleaseSemaphore(&session->lock);
	return found;
}

/**
 * session_release_frame() - The client lets go of a frame; the process may be waiting for one.
 * @session: the session
 * @pub:     the frame
 */
void session_release_frame(struct session *session, struct VCFrame *pub)
{
	ObtainSemaphore(&session->lock);
	frames_release(session, (struct frame *)pub);
	struct Process *process = session->process;
	ReleaseSemaphore(&session->lock);
	if (process != NULL)
		Signal(&process->pr_Task, WORK_SIGNAL);
}

/**
 * session_flush() - Drop what the session holds: the queued packets back unused, the pictures not handed out.
 * @session: the session
 *
 * The process flushes FFmpeg before its next packet; a picture it is decoding meanwhile is
 * dropped when it comes out (the generation moved on).  An end mark not yet polled goes too.
 *
 * Return: 0.
 */
LONG session_flush(struct session *session)
{
	ObtainSemaphore(&session->lock);
	struct Node *node;
	while ((node = RemHead((struct List *)&session->queue)) != NULL)
		packet_return(session, packet_of_node(node));
	frames_flush(session);
	session->generation++;
	session->flush_pending = TRUE;
	struct Process *process = session->process;
	ReleaseSemaphore(&session->lock);
	if (process != NULL)
		Signal(&process->pr_Task, WORK_SIGNAL);
	return 0;
}

/**
 * session_set_attrs() - Change a running session: its signal, and what it may skip.
 * @session: the session
 * @params:  the change
 *
 * Return: 0, or VCERR_UNSUPPORTED for anything else (a chain's converter takes the output's).
 */
LONG session_set_attrs(struct session *session, const struct VCSessionParams *params)
{
	ObtainSemaphore(&session->lock);
	if (params->SigTask != NULL)
		session->sig_task = params->SigTask;
	if (params->SigMask != 0)
		session->sig_mask = params->SigMask;
	BOOL skip = (params->Changed & VCSPF_Skip) != 0 && VC_HAS_FIELD(params->Size, struct VCSessionParams, Skip);
	if (skip)
		session->skip = params->Skip;
	ReleaseSemaphore(&session->lock);
	return (params->Changed & ~VCSPF_Skip) == 0 ? 0 : VCERR_UNSUPPORTED;
}

/**
 * session_output_frame() - One of the current set's frames, by index (VCB_OutputFrame()).
 * @session: the session
 * @index:   0 upward
 *
 * Return: the frame, NULL past the last or before the stream's size is known.
 */
struct VCFrame *session_output_frame(struct session *session, ULONG index)
{
	ObtainSemaphore(&session->lock);
	struct frame_set *set = (struct frame_set *)session->sets.mlh_Head;
	struct VCFrame *frame = NULL;
	if (set->node.mln_Succ != NULL && !set->retired && index < set->count)
		frame = &set->frames[index].pub;
	ReleaseSemaphore(&session->lock);
	return frame;
}

/**
 * session_count() - How many sessions are open.
 *
 * Return: the count.
 */
ULONG session_count(void)
{
	ObtainSemaphore(&sessions_lock);
	ULONG count = session_total;
	ReleaseSemaphore(&sessions_lock);
	return count;
}

/**
 * session_counts() - What every session counted, open and closed.
 * @sum: filled in
 */
void session_counts(struct counts *sum)
{
	ObtainSemaphore(&sessions_lock);
	*sum = closed_counts;
	for (struct MinNode *node = sessions.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		const struct session *session = (const struct session *)node; /* the listing node comes first */
		sum->pictures += session->counts.pictures; /* each written by its process alone, read whole */
		sum->damaged += session->counts.damaged;
		sum->waits += session->counts.waits;
	}
	ReleaseSemaphore(&sessions_lock);
}
