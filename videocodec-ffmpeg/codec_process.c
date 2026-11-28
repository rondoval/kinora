/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_process.c - a session's process (docs/architecture.md, section 14.1): it takes the
 * client's packets in order, puts their pictures together (a framed session's pieces, or an
 * unframed one through FFmpeg's parser), decodes them, and hands the pictures out.
 *
 * It is named after the client's task with " decoder" behind and runs one priority below
 * it.  A packet is read in place and goes back to the client once its bytes are used, so the
 * client is held to the decoder's pace; a picture is copied once, into FFmpeg's packet, whole
 * or put together from its pieces there.  It sleeps on WORK_SIGNAL, which every call that
 * gives it something to do sends.  Each step that calls FFmpeg holds FFmpeg's lock for that
 * step; FFmpeg calling exit() fails the session.
 */
#include <string.h>

#include <dos/dostags.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include <amiga_library.h>
#include <libavutil/mem.h>

#include "codec_private.h"

#define PROCESS_STACK (128u << 10) /* FFmpeg's decoders and the parser */
#define PROCESS_SUFFIX " decoder"
#define PRIORITY_LEAST (-128)

/* What the process does next */
enum work
{
	WORK_NONE,   /* nothing: wait */
	WORK_QUIT,   /* the session closes */
	WORK_FLUSH,  /* flush FFmpeg before the next packet */
	WORK_PACKET, /* take a packet */
};

/**
 * skip_discard() - FFmpeg's skip_frame for a VCSK_*.
 * @skip: VCSK_*
 *
 * Return: the AVDISCARD_*.
 */
static enum AVDiscard skip_discard(ULONG skip)
{
	switch (skip)
	{
	case VCSK_NonReference:
		return AVDISCARD_NONREF;
	case VCSK_NonKey:
		return AVDISCARD_NONKEY;
	default:
		return AVDISCARD_DEFAULT;
	}
}

/**
 * work_next() - What to do next, and the packet when it is one.
 * @session: the session
 * @packet:  gets the packet taken from the queue with WORK_PACKET
 *
 * A failed session does nothing but wait for its close.  A flush comes before any packet:
 * the ones queued before it went back to the client already.
 *
 * Return: WORK_*.
 */
static enum work work_next(struct session *session, struct packet **packet)
{
	enum work work = WORK_NONE;
	ObtainSemaphore(&session->lock);
	if (session->quit)
		work = WORK_QUIT;
	else if (session->failed)
		work = WORK_NONE;
	else if (session->flush_pending)
	{
		session->flush_pending = FALSE;
		session->decode_generation = session->generation;
		work = WORK_FLUSH;
	}
	else if (session->queue.mlh_Head->mln_Succ != NULL)
	{
		*packet = packet_of_node(RemHead((struct List *)&session->queue));
		work = WORK_PACKET;
	}
	ReleaseSemaphore(&session->lock);
	return work;
}

/**
 * pictures_take() - Hand out every picture the decoder has ready.
 * @session: the session; FFmpeg's lock held
 */
static void pictures_take(struct session *session)
{
	while (avcodec_receive_frame(session->decoder, session->decoded) == 0)
	{
		frames_hand_out(session, session->decoded);
		av_frame_unref(session->decoded);
	}
}

/**
 * picture_send() - Send the picture built in session->picture to the decoder, or the end to drain it, and hand out what comes.
 * @session: the session; FFmpeg's lock held
 * @drain:   send the end instead: the decoder gives the pictures it still holds
 *
 * What the session may skip is set in the decoder first, read without the session's lock:
 * a ULONG is read whole, and a change taken a picture early or late makes no difference.  A
 * picture FFmpeg cannot decode is counted (it conceals and goes on); the process is the
 * count's only writer.  session->picture is empty afterwards.
 */
static void picture_send(struct session *session, BOOL drain)
{
	session->decoder->skip_frame = skip_discard(session->skip);
	int code = avcodec_send_packet(session->decoder, drain ? NULL : session->picture);
	av_packet_unref(session->picture);
	if (code != 0)
		session->counts.damaged++;
	pictures_take(session);
}

/**
 * picture_finish() - Send the picture built so far; one without bytes is dropped.
 * @session: the session; FFmpeg's lock held
 *
 * A packet of no bytes is no picture, and FFmpeg would take it for the end.
 */
static void picture_finish(struct session *session)
{
	if (session->picture->size > 0)
		picture_send(session, FALSE);
	av_packet_unref(session->picture);
}

/**
 * stream_end() - The end mark: the last pictures out, VCEV_EndOfStream, and the decoder ready for a stream again.
 * @session: the session; FFmpeg's lock held
 */
static void stream_end(struct session *session)
{
	picture_send(session, TRUE);
	avcodec_flush_buffers(session->decoder);
	ObtainSemaphore(&session->lock);
	if (!session->flush_pending)
		items_event(session, VCEV_EndOfStream, 0, NULL);
	ReleaseSemaphore(&session->lock);
}

/**
 * time_of() - A packet's time as FFmpeg's.
 * @time: VCTime
 *
 * Return: microseconds, AV_NOPTS_VALUE for VC_TIME_UNKNOWN.
 */
static int64_t time_of(VCTime time)
{
	return time == VC_TIME_UNKNOWN ? AV_NOPTS_VALUE : (int64_t)time;
}

/**
 * picture_add() - Put bytes behind the picture being built in session->picture, starting it when none is.
 * @session: the session; FFmpeg's lock held
 * @data:    the bytes
 * @length:  how many
 * @pts:     the picture's time, FFmpeg's, taken when this starts it
 *
 * The packet keeps FFmpeg's padding zeroed behind the bytes, which its decoders read into.
 *
 * Return: TRUE, FALSE without memory.
 */
static BOOL picture_add(struct session *session, const UBYTE *data, ULONG length, int64_t pts)
{
	AVPacket *picture = session->picture;
	int at = picture->size;
	if (picture->buf == NULL)
	{
		if (av_new_packet(picture, (int)length) != 0)
			return FALSE;
		picture->pts = pts;
	}
	else if (av_grow_packet(picture, (int)length) != 0)
		return FALSE;
	memcpy(picture->data + at, data, length);
	return TRUE;
}

/**
 * framed_take() - Take a framed session's packet: a picture, or a piece of one.
 * @session: the session; FFmpeg's lock held
 * @packet:  the packet
 *
 * VCPF_FrameStart marks a picture's first bytes and VCPF_FrameEnd its last: a start ends a
 * picture left unfinished, a piece marked neither continues the one being built, and with
 * none being built it is a whole picture, as one marked both is.  The picture's time is its
 * first piece's.
 *
 * Return: TRUE, FALSE without memory.
 */
static BOOL framed_take(struct session *session, const struct VCPacket *packet)
{
	BOOL starts = (packet->Flags & VCPF_FrameStart) != 0, ends = (packet->Flags & VCPF_FrameEnd) != 0;
	if (starts)
		picture_finish(session);
	BOOL more = !ends && (starts || session->picture->buf != NULL); /* more pieces of this picture come */
	if (!picture_add(session, packet->Data, packet->Length, time_of(packet->Pts)))
		return FALSE;
	if (!more)
		picture_finish(session);
	return TRUE;
}

/**
 * parsed_take() - Take an unframed session's packet, any cut of the stream, through FFmpeg's parser.
 * @session: the session; FFmpeg's lock held
 * @packet:  the packet, read in place: FFmpeg's padding behind its bytes is zeroed first (the
 *           session's packets have room for it); with VCPF_EndOfStream the parser gives what
 *           it still holds as well
 *
 * Return: TRUE, FALSE without memory.
 */
static BOOL parsed_take(struct session *session, struct VCPacket *packet)
{
	const UBYTE *data = packet->Data;
	int left = (int)packet->Length;
	BOOL end = (packet->Flags & VCPF_EndOfStream) != 0;
	memset((UBYTE *)packet->Data + packet->Length, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	while (left > 0 || end)
	{
		uint8_t *picture = NULL;
		int size = 0;
		int used = av_parser_parse2(session->parser, session->decoder, &picture, &size, data, left, time_of(packet->Pts),
		                            AV_NOPTS_VALUE, 0);
		if (size > 0)
		{
			if (!picture_add(session, picture, (ULONG)size, session->parser->pts))
				return FALSE;
			picture_finish(session);
		}
		if (used <= 0 && size <= 0)
			break; /* nothing more comes of what is left */
		data += used;
		left -= used;
	}
	return TRUE;
}

/**
 * packet_decode() - Decode what a packet completes, and the stream's end with it.
 * @session: the session; FFmpeg's lock held
 * @packet:  the packet
 *
 * Return: TRUE, FALSE without memory.
 */
static BOOL packet_decode(struct session *session, struct VCPacket *packet)
{
	BOOL ok = session->parser != NULL ? parsed_take(session, packet) : framed_take(session, packet);
	if (ok && (packet->Flags & VCPF_EndOfStream) != 0)
	{
		picture_finish(session); /* a picture left unfinished */
		stream_end(session);
	}
	return ok;
}

/**
 * packet_take() - Take a packet: its pictures decoded, then the packet given back.
 * @session: the session
 * @packet:  the packet, out of the queue
 *
 * The packet is read in place and goes back to the client once its bytes are used: sent to
 * the decoder, or kept in the picture being built.  FFmpeg calling exit() fails the session
 * (VCERR_STREAM), and so does a lack of memory.
 */
static void packet_take(struct session *session, struct packet *packet)
{
	volatile LONG failure = VCERR_STREAM; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
		failure = packet_decode(session, &packet->pub) ? 0 : VCERR_NOMEM;
	ffmpeg_leave();
	ObtainSemaphore(&session->lock);
	packet_return(session, packet);
	if (failure != 0)
		session_fail(session, failure);
	ReleaseSemaphore(&session->lock);
}

/**
 * decoder_flush() - Flush FFmpeg's decoder and parser and drop the picture put together.
 * @session: the session
 */
static void decoder_flush(struct session *session)
{
	volatile BOOL done = FALSE; /* set after setjmp(), read after a longjmp() */
	if (FFMPEG_ENTER())
	{
		avcodec_flush_buffers(session->decoder);
		BOOL parsed = session->parser != NULL;
		if (parsed)
		{
			av_parser_close(session->parser);
			session->parser = av_parser_init((int)session->decoder->codec_id);
		}
		av_packet_unref(session->picture); /* a picture being built goes */
		done = !parsed || session->parser != NULL;
	}
	ffmpeg_leave();
	if (!done)
	{
		ObtainSemaphore(&session->lock);
		session_fail(session, VCERR_NOMEM);
		ReleaseSemaphore(&session->lock);
	}
}

/**
 * process_run() - The process's work, until the session closes.
 * @session: the session
 */
static void process_run(struct session *session)
{
	for (;;)
	{
		struct packet *packet = NULL;
		enum work work = work_next(session, &packet);
		if (work == WORK_QUIT)
			return;
		if (work == WORK_FLUSH)
			decoder_flush(session);
		else if (work == WORK_PACKET)
			packet_take(session, packet);
		else
			Wait(WORK_SIGNAL);
	}
}

/**
 * process_main() - The process's entry: its session from its start message, its work, its end.
 *
 * It ends with Forbid() on, the closer signalled: the closer runs, and may let the library
 * go, only once the process is gone.
 */
static void process_main(void)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	WaitPort(&me->pr_MsgPort);
	struct session *session = ((struct start_message *)GetMsg(&me->pr_MsgPort))->session;
	process_run(session);
	Forbid();
	session->process = NULL;
	Signal(session->closer, 1UL << session->closer_signal);
}

/**
 * name_make() - The process's name: the client's task's, PROCESS_SUFFIX behind.
 * @session: the session
 * @client:  the client's task
 */
static void name_make(struct session *session, const struct Task *client)
{
	const char *from = client->tc_Node.ln_Name != NULL ? client->tc_Node.ln_Name : BACKEND_NAME;
	ULONG room = sizeof(session->process_name) - sizeof(PROCESS_SUFFIX);
	ULONG length = 0;
	while (from[length] != 0 && length < room)
	{
		session->process_name[length] = from[length];
		length++;
	}
	memcpy(session->process_name + length, PROCESS_SUFFIX, sizeof(PROCESS_SUFFIX));
}

/**
 * process_start() - Start the session's process, one priority below the client's task.
 * @session: the session, its decoder open
 *
 * Return: 0, or VCERR_NOMEM.
 */
LONG process_start(struct session *session)
{
	struct Task *client = session->sig_task != NULL ? session->sig_task : FindTask(NULL);
	LONG priority = client->tc_Node.ln_Pri > PRIORITY_LEAST ? client->tc_Node.ln_Pri - 1 : PRIORITY_LEAST;
	name_make(session, client);
	session->start.message.mn_Node.ln_Type = NT_MESSAGE;
	session->start.message.mn_Length = sizeof(session->start);
	session->start.session = session;
	struct Process *process = CreateNewProcTags(NP_Entry, (ULONG)process_main, NP_Name, (ULONG)session->process_name,
	                                            NP_Priority, (ULONG)priority, NP_StackSize, PROCESS_STACK, TAG_DONE);
	if (process == NULL)
		return VCERR_NOMEM;
	session->process = process;
	PutMsg(&process->pr_MsgPort, &session->start.message);
	return 0;
}

/**
 * process_stop() - End the session's process and wait until it is gone.
 * @session: the session
 *
 * The process ends its step first: a decode under way, or a wait for a free frame, which the
 * quit ends.
 */
void process_stop(struct session *session)
{
	if (session->process == NULL)
		return;
	BYTE signal = AllocSignal(-1);
	session->closer = FindTask(NULL);
	session->closer_signal = signal >= 0 ? signal : SIGBREAKB_CTRL_E;
	ObtainSemaphore(&session->lock);
	session->quit = TRUE;
	ReleaseSemaphore(&session->lock);
	Signal(&session->process->pr_Task, WORK_SIGNAL);
	while (session->process != NULL)
		Wait(1UL << session->closer_signal);
	if (signal >= 0)
		FreeSignal(signal);
}
