/* SPDX-License-Identifier: MPL-2.0 */
/*
 * engine_video.c - the engine's video decoder: the queue's packets go in,
 * pictures come out, wait for their time and are shown.
 *
 * The session is framed: a packet of the file is one picture, cut over as
 * many of the decoder's input packets as it needs and marked at its start
 * and end.  Decoded pictures are held, not copied; the decoder stops when
 * all its frames are out, which is what keeps it a few pictures ahead and no
 * more.
 */
#include <string.h>
#include <proto/exec.h>
#include <proto/demuxer.h>
#include <proto/videocodec.h>

#include "engine_private.h"
#include "trace.h"

#define LATE_LIMIT 120000 /* microseconds behind the clock from which a picture is not shown: some three pictures */
#define INPUT_PACKETS 8u  /* the decoder's input packets asked for: fewer than FREE_PACKETS */
#define ITEMS_A_PASS 64u  /* VC_Poll() items taken in one pass, so a busy decoder cannot keep the engine from its commands */

/* Skipping while the decoder falls behind (architecture 6.3), in microseconds of clock_wall() */
#define SKIP_STEP_GAP 2000000      /* a step up at most this often */
#define SKIP_HOLD_FIRST 10000000   /* this long without a dropped picture takes a step back */
#define SKIP_HOLD_MOST 160000000   /* the longest that wait grows to */
#define SKIP_RELAPSE 5000000       /* drops this soon after a step back double the wait */

/**
 * ready_release() - Give every waiting picture back to the decoder unshown.
 * @engine: the engine
 */
static void ready_release(struct engine *engine)
{
	while (engine->ready_count != 0)
	{
		VC_ReleaseFrame(engine->video_decoder, engine->ready_frames[engine->ready_head]);
		engine->ready_head = (engine->ready_head + 1) % READY_FRAMES;
		engine->ready_count--;
	}
}

/**
 * skip_set() - Tell the decoder what it may leave out (VCA_Skip); the hardware decoders ignore it.
 * @engine: the engine, its decoder open
 */
static void skip_set(struct engine *engine)
{
	VC_SetAttrsTags(engine->video_decoder, VCA_Skip, engine->skip.level, TAG_DONE);
}

/**
 * skip_reset() - Start a film decoding every picture.
 * @engine: the engine
 */
static void skip_reset(struct engine *engine)
{
	engine->skip = (struct video_skip){.level = VCSK_None, .changed = clock_wall(), .hold = SKIP_HOLD_FIRST};
}

/**
 * skip_more() - A picture was dropped: ask the decoder to leave more out, a step at most every SKIP_STEP_GAP.
 * @engine: the engine
 *
 * A step up soon after a step back doubles the wait before the next one, so that a film the
 * machine cannot play whole settles at the level it can.
 */
static void skip_more(struct engine *engine)
{
	struct video_skip *skip = &engine->skip;
	VCTime now = clock_wall();
	skip->last_drop = now;
	if (skip->level >= VCSK_NonKey || now - skip->changed < SKIP_STEP_GAP)
		return;
	if (skip->lowered != 0 && now - skip->lowered < SKIP_RELAPSE)
		skip->hold = skip->hold * 2 < SKIP_HOLD_MOST ? skip->hold * 2 : SKIP_HOLD_MOST;
	skip->level++;
	skip->changed = now;
	skip->lowered = 0;
	skip_set(engine);
}

/**
 * skip_less() - After its wait without a dropped picture, ask the decoder to leave less out.
 * @engine: the engine, its decoder open
 */
static void skip_less(struct engine *engine)
{
	struct video_skip *skip = &engine->skip;
	VCTime now = clock_wall();
	VCTime since = skip->changed > skip->last_drop ? skip->changed : skip->last_drop;
	if (skip->level == VCSK_None || now - since < skip->hold)
		return;
	skip->level--;
	skip->changed = now;
	skip->lowered = now;
	skip_set(engine);
}

/**
 * tag_if() - A tag that counts only when its value was given.
 * @given: the value was given
 * @tag:   the tag
 *
 * Return: the tag, or TAG_IGNORE.
 */
static Tag tag_if(BOOL given, Tag tag)
{
	return given ? tag : TAG_IGNORE;
}

/**
 * video_open() - Open the decoder for the file's video stream.
 * @engine: the engine, the file open
 *
 * Without a video stream or a window to show it in there is no decoder, and that is no error.
 *
 * Return: 0 or ENGINE_ERR_VIDEO.
 */
LONG video_open(struct engine *engine)
{
	const struct DMXStream *stream = &engine->video_stream;
	if (engine->video_index == NO_STREAM || engine->display == NULL)
		return 0;
	ULONG format = display_choose_format(engine->display, stream->Codec);
	if (format == VCF_Unknown)
		return ENGINE_ERR_VIDEO;

	BOOL has_rate = stream->FrameRate.Num > 0 && stream->FrameRate.Den > 0;
	BOOL has_extradata = stream->ExtradataSize != 0;
	struct TagItem tags[] = {
	    {VCA_Codec, stream->Codec},
	    {VCA_Kind, VCK_Decoder},
	    {VCA_CodecTag, stream->Tag},
	    {VCA_OutputFormat, format},
	    {VCA_SigTask, (ULONG)FindTask(NULL)},
	    {VCA_SigMask, 1UL << engine->video_signal},
	    {VCA_Framed, TRUE},
	    {VCA_InputCount, INPUT_PACKETS},
	    {tag_if(stream->Width != 0, VCA_Width), stream->Width}, /* the container's word on the size: MS-MPEG4 carries none */
	    {tag_if(stream->Height != 0, VCA_Height), stream->Height},
	    {tag_if(has_extradata, VCA_Extradata), (ULONG)stream->Extradata},
	    {tag_if(has_extradata, VCA_ExtradataSize), stream->ExtradataSize},
	    {tag_if(has_rate, VCA_FrameRateNum), (ULONG)stream->FrameRate.Num},
	    {tag_if(has_rate, VCA_FrameRateDen), (ULONG)stream->FrameRate.Den},
	    {tag_if(stream->ColorSpace != VCCS_Unknown, VCA_ColorSpace), stream->ColorSpace},
	    {VCA_BackendName, (ULONG)engine->video_backend},
	    {VCA_ConverterName, (ULONG)engine->video_converter},
	    {TAG_DONE, 0},
	};
	engine->video_decoder = VC_OpenSessionTagList(tags);
	skip_reset(engine);
	return engine->video_decoder != NULL ? 0 : ENGINE_ERR_VIDEO;
}

/**
 * video_close() - Close the decoder.
 * @engine: the engine; without a decoder nothing happens
 *
 * The session's packets and frames go with it.
 */
void video_close(struct engine *engine)
{
	display_detach(engine->display);
	if (engine->video_decoder != NULL)
		VC_CloseSession(engine->video_decoder);
	engine->video_decoder = NULL;
	engine->free_packet_count = 0;
	engine->ready_head = 0;
	engine->ready_count = 0;
	engine->video_end_sent = FALSE;
	engine->video_done = FALSE;
	engine->decoder_sized = FALSE;
	engine->first_shown = FALSE;
}

/**
 * video_attach() - Have the decoder make its pictures for the display.
 * @engine: the engine, the stream's shape known
 *
 * The pictures waiting are released first: the decoder gets new frames,
 * and what the subtitles wrote into the old ones is forgotten.  Without a
 * display the decoder goes on at the stream's own size and its pictures are
 * passed over.
 */
void video_attach(struct engine *engine)
{
	if (engine->display == NULL)
		return;
	ready_release(engine);
	engine->decoder_sized =
	    display_attach(engine->display, engine->video_decoder, engine->video_stream.Codec, &engine->video_shape, &engine->aspect);
	subtitles_frames_new(engine->subtitles);
	if (!engine->decoder_sized)
		engine->error = ENGINE_ERR_VIDEO;
}

/**
 * video_place() - Show the pictures in another place.
 * @engine: the engine
 * @place:  the place, NULL for nowhere
 */
void video_place(struct engine *engine, const struct display_place *place)
{
	if (engine->video_decoder != NULL)
		ready_release(engine);
	display_detach(engine->display);
	display_close(engine->display);
	engine->display = display_open(place, FindTask(NULL), 1UL << engine->video_signal);
	engine->decoder_sized = FALSE;
	if (engine->video_decoder != NULL && engine->video_shape.Crop.Width != 0)
		video_attach(engine);
}

/**
 * event_take() - Act on something the decoder says.
 * @engine: the engine
 * @event:  the event
 */
static void event_take(struct engine *engine, const struct VCEvent *event)
{
	if (event->Event == VCEV_StreamInfo)
	{
		engine->video_shape = event->Stream;
		if (engine->video_stream.Par.Num > 0 && engine->video_stream.Par.Den > 0)
			engine->video_shape.Par = engine->video_stream.Par; /* the file's word on the shape counts before the stream's */
		video_attach(engine);
	}
	else if (event->Event == VCEV_EndOfStream)
	{
		engine->video_done = TRUE;
	}
	else if (event->Event == VCEV_Error)
	{
		engine->error = ENGINE_ERR_VIDEO;
		engine->video_done = TRUE;
	}
}

/**
 * item_take() - Take one thing the decoder hands out.
 * @engine: the engine
 * @item:   what VC_Poll() filled
 *
 * An empty input packet is kept to be filled, a picture joins those waiting
 * for their time; one more than can wait is given back unshown.
 */
static void item_take(struct engine *engine, const struct VCItem *item)
{
	if (item->Type == VCIT_PacketReturned && engine->free_packet_count < FREE_PACKETS)
	{
		engine->free_packets[engine->free_packet_count++] = item->u.Packet;
	}
	else if (item->Type == VCIT_Frame && engine->ready_count < READY_FRAMES)
	{
		VCTime early = item->u.Frame->Pts - clock_now(&engine->clock);
		if (early >= 0)
			TRACE_VALUE(TRACE_ARRIVED, early);
		else
			TRACE_VALUE(TRACE_ARRIVED_LATE, -early);
		engine->ready_frames[(engine->ready_head + engine->ready_count) % READY_FRAMES] = item->u.Frame;
		engine->ready_count++;
	}
	else if (item->Type == VCIT_Frame)
	{
		VC_ReleaseFrame(engine->video_decoder, item->u.Frame);
	}
	else if (item->Type == VCIT_Event)
	{
		event_take(engine, &item->u.Event);
	}
}

/**
 * end_send() - Tell the decoder the stream is over.
 * @engine: the engine, an empty input packet in hand
 */
static void end_send(struct engine *engine)
{
	struct VCPacket *packet = engine->free_packets[--engine->free_packet_count];
	packet->Length = 0;
	packet->Pts = VC_TIME_UNKNOWN;
	packet->Flags = VCPF_EndOfStream;
	if (VC_SendPacket(engine->video_decoder, packet) != 0)
		engine->video_done = TRUE; /* no end mark will come back */
	engine->video_end_sent = TRUE;
}

/**
 * piece_send() - Send the next piece of the queue's first packet to the decoder.
 * @engine: the engine, an empty input packet in hand
 * @from:   the queue's first packet
 *
 * When its last byte is sent the packet leaves the queue, handed back to the reader.
 */
static void piece_send(struct engine *engine, const struct DMXPacket *from)
{
	struct VCPacket *packet = engine->free_packets[--engine->free_packet_count];
	ULONG left = from->Length - engine->head_sent_bytes;
	ULONG piece = left < packet->Capacity ? left : packet->Capacity;

	memcpy(packet->Data, (const UBYTE *)from->Data + engine->head_sent_bytes, piece);
	packet->Length = piece;
	packet->Pts = from->Pts;
	packet->Flags = (engine->head_sent_bytes == 0 ? VCPF_FrameStart : 0) | (piece == left ? VCPF_FrameEnd : 0) |
	                ((from->Flags & DMXPF_KeyFrame) != 0 ? VCPF_KeyFrame : 0);
	if (VC_SendPacket(engine->video_decoder, packet) != 0)
		engine->error = ENGINE_ERR_VIDEO;
	engine->head_sent_bytes += piece;
	if (piece == left)
	{
		reader_take(engine, &engine->video_queue);
		engine->head_sent_bytes = 0;
	}
}

/**
 * video_feed() - Fill the decoder's empty input packets from the queue.
 * @engine: the engine
 *
 * With the queue empty and the file read to its end, the decoder is told so, once.
 */
static void video_feed(struct engine *engine)
{
	while (engine->free_packet_count != 0 && !engine->video_end_sent)
	{
		BOOL ended;
		const struct DMXPacket *from = reader_next(engine, &engine->video_queue, &ended);
		if (from != NULL)
			piece_send(engine, from);
		else if (ended)
			end_send(engine);
		else
			break;
	}
}

/**
 * frame_overlay() - Write what is drawn over the picture into its frame: the subtitle of its time, and the statistics panel while it is on.
 * @engine: the engine
 * @frame:  the picture, about to be shown
 *
 * Without a subtitle, what an earlier one left beside the picture is taken away.
 */
static void frame_overlay(struct engine *engine, struct VCFrame *frame)
{
	BOOL shown = engine->subtitle_index != ENGINE_TRACK_OFF && frame->Pts != VC_TIME_UNKNOWN;
	subtitles_draw(engine->subtitles, shown ? frame->Pts - engine->subtitle_delay : VC_TIME_UNKNOWN, frame, frame->Format);
	if (engine->statistics.panel_on)
		subtitles_draw_panel(engine->subtitles, engine->statistics.text, engine->statistics.text_version, frame, frame->Format);
}

/**
 * frame_count() - Count a picture as shown or dropped.
 * @engine: the engine
 * @shown:  it was shown
 *
 * After a move the sound may start a little later than the first picture's
 * time, and the pictures between are late: they are not counted as dropped,
 * until a picture other than the first comes in time.  A picture dropped
 * otherwise asks the decoder to skip (skip_more()).
 */
static void frame_count(struct engine *engine, BOOL shown)
{
	if (shown)
		engine->frames_shown++;
	else if (!engine->catching_up)
	{
		engine->frames_dropped++;
		skip_more(engine);
	}
	if (shown && engine->first_shown)
		engine->catching_up = FALSE;
}

/**
 * ready_take() - Take the first waiting picture: show it, or give it back to the decoder unshown.
 * @engine: the engine, a picture waiting
 * @show:   show it; else it is dropped as too late
 *
 * The first picture after the file was opened or moved in says where the
 * film goes on from, and starts the clock where no sound does.
 */
static void ready_take(struct engine *engine, BOOL show)
{
	struct VCFrame *frame = engine->ready_frames[engine->ready_head];
	engine->ready_head = (engine->ready_head + 1) % READY_FRAMES;
	engine->ready_count--;
	VCTime time = frame->Pts;
	BOOL shown = show && engine->decoder_sized;
	if (shown)
	{
		stats_count_frame(engine, frame);
		display_bars_black(engine->display, frame); /* before anything is written into them */
		frame_overlay(engine, frame);
		display_show(engine->display, frame); /* the display's from here */
	}
	else
	{
		VC_ReleaseFrame(engine->video_decoder, frame);
	}
	frame_count(engine, shown);
	if (!engine->first_shown)
	{
		engine->first_shown = TRUE;
		if (time != VC_TIME_UNKNOWN)
			clock_place(&engine->clock, time); /* a move lands on a key frame before the time asked for: the film goes on from there */
		engine_clock_start(engine);
	}
}

/**
 * frame_late() - Whether the first waiting picture comes too late to be shown.
 * @engine: the engine
 * @frame:  the picture, its time come
 * @now:    the film's time
 *
 * Return: TRUE when the picture behind it is due as well, or it is further behind the clock than LATE_LIMIT.
 */
static BOOL frame_late(const struct engine *engine, const struct VCFrame *frame, VCTime now)
{
	const struct VCFrame *next = engine->ready_frames[(engine->ready_head + 1) % READY_FRAMES];
	if (engine->ready_count > 1 && next->Pts != VC_TIME_UNKNOWN && next->Pts <= now)
		return TRUE;
	return frame->Pts != VC_TIME_UNKNOWN && now - frame->Pts > LATE_LIMIT;
}

/**
 * frames_show() - Show the waiting pictures whose time has come.
 * @engine: the engine
 *
 * The first picture after an open or a seek is shown at once, playing or
 * not.  A picture whose successor is due as well is dropped, and so is one
 * further behind the clock than LATE_LIMIT: showing costs time, and only
 * pictures not shown let the video catch the clock up again.  For a picture
 * not yet due the engine's timer is set.  While the display is busy with the
 * last picture nothing is shown; it signals the engine when it is done.
 */
static void frames_show(struct engine *engine)
{
	while (engine->ready_count != 0 && !display_busy(engine->display))
	{
		const struct VCFrame *frame = engine->ready_frames[engine->ready_head];
		if (!engine->first_shown)
		{
			ready_take(engine, TRUE);
			continue;
		}
		if (engine->state != ENGINE_PLAYING)
			break;
		VCTime now = clock_now(&engine->clock);
		if (frame->Pts != VC_TIME_UNKNOWN && frame->Pts > now)
		{
			engine_wake_in(engine, frame->Pts - now);
			break;
		}
		BOOL late = frame_late(engine, frame, now);
		TRACE_VALUE(late ? TRACE_DROPPED : TRACE_SHOWN, now > frame->Pts ? now - frame->Pts : 0);
		ready_take(engine, !late);
	}
}

/**
 * video_pump() - One pass of the video decoder: take what it hands out, feed it, show what is due.
 * @engine: the engine
 */
void video_pump(struct engine *engine)
{
	if (engine->video_decoder == NULL)
		return;
	struct VCItem item;
	ULONG taken = 0;
	while (taken < ITEMS_A_PASS && VC_Poll(engine->video_decoder, &item))
	{
		item_take(engine, &item);
		taken++;
	}
	if (taken == ITEMS_A_PASS)
		Signal(FindTask(NULL), 1UL << engine->video_signal); /* more may wait: another pass, after the commands */
	video_feed(engine);
	frames_show(engine);
	skip_less(engine);
	VCTime again = display_poll(engine->display);
	if (again > 0)
		engine_wake_in(engine, again);
}

/**
 * video_flush() - Drop what the decoder holds, for a seek.
 * @engine: the engine
 */
void video_flush(struct engine *engine)
{
	if (engine->video_decoder == NULL)
		return;
	ready_release(engine);
	VC_Flush(engine->video_decoder);
	engine->video_end_sent = FALSE;
	engine->video_done = FALSE;
	engine->first_shown = FALSE;
	engine->catching_up = TRUE;
}
