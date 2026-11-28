/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_frames.c - the frames FFmpeg decodes into (docs/architecture.md, section 14.1).
 *
 * FFmpeg asks for each picture's memory (get_buffer2), and gets one of the session's
 * frames: I420, each row of whole 64 bytes, the luma rows a multiple of 16 and at least as
 * many as FFmpeg writes, the two chroma planes after them, every frame on a 64-byte line in
 * Fast RAM the VPU reaches (codec_memory.c), which is what the VideoCore's converter imports.
 *
 * The words used here:
 *
 *   in use    a frame FFmpeg holds (its buffer has a reference left) or the client holds
 *             (handed out, not yet given back with VCB_ReleaseFrame()); an unused frame
 *             can take the next picture, and FFmpeg's request waits for one when none is
 *   set       the frames of one shape, made when FFmpeg first asks for a picture of it
 *   current   the newest set, the one VCB_OutputFrame() lists and new pictures go into
 *   retired   a set a newer one replaced; its frames in use stay valid
 *   spent     a retired set none of whose frames is in use any more
 *   dispose   give a set's memory back: done to a set once it is spent, and to all of them
 *             when the decoder goes
 *
 * Everything here runs with the session's lock held, or takes it.
 */
#include <string.h>

#include <exec/memory.h>
#include <proto/exec.h>

#include <libavutil/buffer.h>
#include <libavutil/frame.h>

#include "codec_private.h"

#define PITCH_ALIGN 64 /* videocodec's I420 rows: whole 64 bytes */
#define ROW_ALIGN 16   /* its luma rows: a multiple of 16 */
#define TAIL_BYTES 256 /* past the last plane: FFmpeg's readers may look a little beyond */
#define ALIGN_UP(x, a) (((x) + (a) - 1) / (a) * (a))
#define PLANES 3

/**
 * frame_in_use() - Whether FFmpeg or the client holds a frame.
 * @frame: the frame
 *
 * Return: TRUE while either does; an unused frame can take the next picture.
 */
static BOOL frame_in_use(const struct frame *frame)
{
	return frame->with_decoder || frame->with_client;
}

/**
 * set_in_use() - Whether any frame of a set is in use.
 * @set: the set
 *
 * Return: TRUE while FFmpeg or the client holds one of its frames.
 */
static BOOL set_in_use(const struct frame_set *set)
{
	for (ULONG i = 0; i < set->count; i++)
		if (frame_in_use(&set->frames[i]))
			return TRUE;
	return FALSE;
}

/**
 * set_dispose() - Give a set's memory back: its pictures, its frames and itself.
 * @set: the set, out of the session's list or never in it; none of its frames may be in use
 */
static void set_dispose(struct frame_set *set)
{
	memory_free(set->memory);
	FreeVec(set->frames);
	FreeVec(set);
}

/**
 * set_dispose_if_spent() - Dispose of a set once it is retired and none of its frames is in use.
 * @set: the set; a current one, or one with a frame in use, is left as it is
 *
 * Called whenever a set is retired or one of its frames comes out of use.
 */
static void set_dispose_if_spent(struct frame_set *set)
{
	if (!set->retired || set_in_use(set))
		return;
	Remove((struct Node *)&set->node);
	set_dispose(set);
}

/**
 * set_layout() - A new set's layout, for the pictures FFmpeg asks for.
 * @set:     the set, its width and height set
 * @decoder: the decoder; FFmpeg's lock held
 *
 * As many rows as FFmpeg writes (an MPEG-4 picture of 400 rows writes 416), and the
 * converter is told the frame's rows and takes the picture by its crop.
 */
static void set_layout(struct frame_set *set, AVCodecContext *decoder)
{
	int aligned_width = (int)set->width, aligned_height = (int)set->height;
	int linesize_align[AV_NUM_DATA_POINTERS];
	avcodec_align_dimensions2(decoder, &aligned_width, &aligned_height, linesize_align);
	set->pitch = ALIGN_UP((ULONG)aligned_width, PITCH_ALIGN);
	set->rows = ALIGN_UP(set->height, ROW_ALIGN);
	if ((ULONG)aligned_height > set->rows)
		set->rows = ALIGN_UP((ULONG)aligned_height, ROW_ALIGN);
	set->picture_bytes = set->pitch * set->rows + 2 * (set->pitch / 2) * (set->rows / 2);
}

/**
 * frame_fill() - Fill in a frame of a new set: what the client sees of it (its planes in the set's layout) and its set.
 * @set:   the set, its layout known
 * @frame: the frame
 * @data:  where its picture starts in the set's memory
 */
static void frame_fill(struct frame_set *set, struct frame *frame, UBYTE *data)
{
	struct VCFrame *pub = &frame->pub;
	ULONG luma = set->pitch * set->rows;
	pub->Size = sizeof(*pub);
	pub->Data = data;
	pub->Format = VCF_I420;
	pub->Width = set->pitch;
	pub->Height = set->rows;
	pub->Crop = (struct VCRect){0, 0, (LONG)set->width, (LONG)set->height};
	pub->Planes = PLANES;
	pub->Offset[1] = luma;
	pub->Offset[2] = luma + (set->pitch / 2) * (set->rows / 2);
	pub->Pitch[0] = set->pitch;
	pub->Pitch[1] = set->pitch / 2;
	pub->Pitch[2] = set->pitch / 2;
	pub->Pts = VC_TIME_UNKNOWN;
	frame->set = set;
}

/**
 * set_make() - Make a set of frames for pictures of a size, none of them in use.
 * @session: the session
 * @decoder: the decoder; FFmpeg's lock held
 * @width:   the frame FFmpeg asks for: its coded width
 * @height:  and rows
 *
 * Return: the set, not yet in the session's list; NULL without memory.
 */
static struct frame_set *set_make(struct session *session, AVCodecContext *decoder, ULONG width, ULONG height)
{
	struct frame_set *set = AllocVec(sizeof(*set), MEMF_PUBLIC | MEMF_CLEAR);
	if (set == NULL)
		return NULL;

	set->owner = session;
	set->width = width;
	set->height = height;
	set->count = session->frame_count;
	set_layout(set, decoder);
	ULONG slot = ALIGN_UP(set->picture_bytes, MEMORY_ALIGN);
	set->frames = AllocVec(set->count * sizeof(*set->frames), MEMF_PUBLIC | MEMF_CLEAR);
	set->memory = memory_alloc(set->count * slot + TAIL_BYTES);
	if (set->frames == NULL || set->memory == NULL)
	{
		set_dispose(set); /* FreeVec() and memory_free() pass over what is NULL */
		return NULL;
	}

	for (ULONG i = 0; i < set->count; i++)
		frame_fill(set, &set->frames[i], (UBYTE *)set->memory + i * slot);
	return set;
}

/**
 * stream_info_fill() - Fill in the shape a new set gives the client, with VCEV_StreamInfo.
 * @session: the session
 * @set:     the set
 * @decoder: the decoder; FFmpeg's lock held
 * @info:    filled in
 *
 * The frame (its row in pixels, its rows) and the picture as its crop, which a chain passes
 * to its converter as the part to take; the rate the client gave or the stream's; the colour
 * space the client gave or the stream's, as videocodec.library guesses for a stream that
 * names none.
 */
static void stream_info_fill(const struct session *session, const struct frame_set *set, const AVCodecContext *decoder,
                             struct VCStreamInfo *info)
{
	memset(info, 0, sizeof(*info));
	info->Width = set->pitch;
	info->Height = set->rows;
	info->Crop = (struct VCRect){0, 0, decoder->width, decoder->height};
	info->FrameRate = session->frame_rate.Num > 0 ? session->frame_rate
	                                               : (struct VCRational){decoder->framerate.num, decoder->framerate.den};
	BOOL aspect = decoder->sample_aspect_ratio.num > 0 && decoder->sample_aspect_ratio.den > 0;
	info->Par = aspect ? (struct VCRational){decoder->sample_aspect_ratio.num, decoder->sample_aspect_ratio.den}
	                   : (struct VCRational){1, 1};
	info->ColorSpace = session->color_space != VCCS_Unknown
	                       ? session->color_space
	                       : VC_ColorSpaceOf((ULONG)decoder->colorspace, decoder->color_range == AVCOL_RANGE_JPEG, (ULONG)decoder->height);
	info->Format = VCF_I420;
}

/**
 * set_for_picture() - The set for a picture FFmpeg asks for: the current one, or a new one for a new size.
 * @session: the session, its lock held
 * @decoder: the decoder; FFmpeg's lock held
 * @frame:   FFmpeg's request, its coded size set
 *
 * A new set becomes the current one; the one it replaces is retired, and
 * disposed of as soon as it is spent, which may be at once.  The client
 * hears of the new shape (VCEV_StreamInfo) before its first picture.
 *
 * Return: the set, NULL without memory.
 */
static struct frame_set *set_for_picture(struct session *session, AVCodecContext *decoder, const AVFrame *frame)
{
	struct frame_set *current = (struct frame_set *)session->sets.mlh_Head;
	BOOL any = current->node.mln_Succ != NULL;
	if (any && (ULONG)frame->width == current->width && (ULONG)frame->height == current->height)
		return current;

	struct frame_set *set = set_make(session, decoder, (ULONG)frame->width, (ULONG)frame->height);
	if (set == NULL)
		return NULL;
	if (any)
	{
		current->retired = TRUE;
		set_dispose_if_spent(current);
	}

	AddHead((struct List *)&session->sets, (struct Node *)&set->node);
	struct VCStreamInfo info;
	stream_info_fill(session, set, decoder, &info);
	items_event(session, VCEV_StreamInfo, 0, &info);
	return set;
}

/**
 * frame_wait_unused() - An unused frame of a set, waiting for one when all are in use.
 * @session: the session, its lock held (let go while it waits)
 * @set:     the set
 *
 * A frame comes out of use when FFmpeg lets go of it (buffer_released())
 * or the client gives it back, and either signals the session's process.
 * A close or a flush ends the wait: the picture would be dropped anyway.
 *
 * Return: the frame, NULL when the wait was ended.
 */
static struct frame *frame_wait_unused(struct session *session, struct frame_set *set)
{
	BOOL counted = FALSE;
	for (;;)
	{
		for (ULONG i = 0; i < set->count; i++)
			if (!frame_in_use(&set->frames[i]))
				return &set->frames[i];
		if (session->quit || session->flush_pending)
			return NULL;
		if (!counted)
			session->counts.waits++;
		counted = TRUE;
		ReleaseSemaphore(&session->lock);
		Wait(WORK_SIGNAL);
		ObtainSemaphore(&session->lock);
	}
}

/**
 * buffer_released() - FFmpeg let go of a frame: its buffer's last reference went (the buffer's free callback).
 * @opaque: the frame
 * @data:   its picture
 *
 * Nothing is freed here: the picture's memory is the set's.  The frame is
 * FFmpeg's no longer, its set may be spent now, and a request waiting for
 * an unused frame (frame_wait_unused()) is woken.  This mostly runs in the
 * session's process itself, inside an FFmpeg call: the signal it then sends
 * itself costs one pass that finds nothing to do, less than telling the
 * two cases apart on every release.
 */
static void buffer_released(void *opaque, uint8_t *data)
{
	(void)data;
	struct frame *frame = opaque;
	struct session *session = frame->set->owner;
	ObtainSemaphore(&session->lock);
	frame->with_decoder = FALSE;
	set_dispose_if_spent(frame->set);
	struct Process *process = session->process;
	ReleaseSemaphore(&session->lock);
	if (process != NULL)
		Signal(&process->pr_Task, WORK_SIGNAL);
}

/**
 * request_refused() - Whether FFmpeg asks for a picture the session cannot take.
 * @session: the session
 * @frame:   FFmpeg's request
 *
 * Return: 0, VCERR_UNSUPPORTED for a sampling other than 8-bit 4:2:0, VCERR_STREAM for a
 * picture larger than the codec's row allows.
 */
static LONG request_refused(const struct session *session, const AVFrame *frame)
{
	if (frame->format != AV_PIX_FMT_YUV420P)
		return VCERR_UNSUPPORTED;
	if (frame->width <= 0 || frame->height <= 0 || (ULONG)frame->width * (ULONG)frame->height > session->max_area)
		return VCERR_STREAM;
	return 0;
}

/**
 * frame_attach() - Give FFmpeg's request a frame: its planes, and a buffer whose last reference gives it back (buffer_released()).
 * @request: FFmpeg's request
 * @frame:   the frame, taken for FFmpeg
 *
 * Return: 0, or AVERROR(ENOMEM).
 */
static int frame_attach(AVFrame *request, struct frame *frame)
{
	const struct VCFrame *pub = &frame->pub;
	request->buf[0] = av_buffer_create(pub->Data, frame->set->picture_bytes, buffer_released, frame, 0);
	if (request->buf[0] == NULL)
		return AVERROR(ENOMEM);
	for (int plane = 0; plane < PLANES; plane++)
	{
		request->data[plane] = (uint8_t *)pub->Data + pub->Offset[plane];
		request->linesize[plane] = (int)pub->Pitch[plane];
	}
	request->extended_data = request->data;
	return 0;
}

/**
 * frames_get_buffer() - FFmpeg's get_buffer2: a frame of the session's for the next picture.
 * @decoder: the decoder; FFmpeg's lock held
 * @request: what FFmpeg asks for, its coded size and format set
 * @flags:   AV_GET_BUFFER_FLAG_*
 *
 * Return: 0, or an AVERROR: the picture is refused, there is no memory, or the wait for an
 * unused frame was ended.
 */
int frames_get_buffer(AVCodecContext *decoder, AVFrame *request, int flags)
{
	(void)flags;
	struct session *session = decoder->opaque;
	LONG refused = request_refused(session, request);
	ObtainSemaphore(&session->lock);
	struct frame_set *set = refused == 0 ? set_for_picture(session, decoder, request) : NULL;
	struct frame *frame = set != NULL ? frame_wait_unused(session, set) : NULL;
	if (frame != NULL)
		frame->with_decoder = TRUE;
	if (refused != 0 || set == NULL)
		session_fail(session, refused != 0 ? refused : VCERR_NOMEM);
	ReleaseSemaphore(&session->lock);
	if (frame == NULL)
		return set == NULL ? AVERROR(ENOMEM) : AVERROR(EAGAIN);
	int code = frame_attach(request, frame);
	if (code != 0)
	{
		ObtainSemaphore(&session->lock);
		frame->with_decoder = FALSE;
		session_fail(session, VCERR_NOMEM);
		ReleaseSemaphore(&session->lock);
	}
	return code;
}

/**
 * frame_find() - One of the session's frames from a pointer, in any of its sets.
 * @session: the session, its lock held
 * @maybe:   the pointer: the client's, or a buffer's opaque from FFmpeg
 *
 * A set's frames are one array, so a pointer is one of them when it lies
 * in the array on a frame's start: one test a set, of which there is one
 * but while a new shape replaces an old.  Nothing is read through the
 * pointer before it passed.
 *
 * Return: the frame, NULL for a pointer that is none of them.
 */
static struct frame *frame_find(struct session *session, const void *maybe)
{
	for (struct MinNode *node = session->sets.mlh_Head; node->mln_Succ != NULL; node = node->mln_Succ)
	{
		struct frame_set *set = (struct frame_set *)node;
		ULONG offset = (ULONG)maybe - (ULONG)set->frames; /* below the array it wraps past any length */
		if (offset < set->count * sizeof(*set->frames) && offset % sizeof(*set->frames) == 0)
			return &set->frames[offset / sizeof(*set->frames)];
	}
	return NULL;
}

/**
 * frame_flags() - A decoded picture's VCFF_* flags.
 * @decoded: the picture
 *
 * Return: the flags.
 */
static ULONG frame_flags(const AVFrame *decoded)
{
	ULONG flags = 0;
	if (decoded->flags & AV_FRAME_FLAG_KEY)
		flags |= VCFF_KeyFrame;
	if (decoded->flags & AV_FRAME_FLAG_INTERLACED)
		flags |= VCFF_Interlaced;
	if (decoded->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST)
		flags |= VCFF_TopFieldFirst;
	return flags;
}

/**
 * frames_hand_out() - Hand a decoded picture to the client.
 * @session: the session
 * @decoded: the picture; FFmpeg's lock held
 *
 * A picture decoded before a flush, or into memory not the session's, is dropped.  The
 * frame stays the client's until it releases it, whatever FFmpeg does with it.
 */
void frames_hand_out(struct session *session, const AVFrame *decoded)
{
	ObtainSemaphore(&session->lock);
	struct frame *frame = decoded->buf[0] != NULL ? frame_find(session, av_buffer_get_opaque(decoded->buf[0])) : NULL;
	BOOL current = !session->flush_pending && session->decode_generation == session->generation && !session->failed;
	if (frame != NULL && current && !frame->with_client)
	{
		struct VCFrame *pub = &frame->pub;
		pub->Pts = decoded->best_effort_timestamp != AV_NOPTS_VALUE ? (VCTime)decoded->best_effort_timestamp : VC_TIME_UNKNOWN;
		pub->Flags = frame_flags(decoded);
		pub->Crop = (struct VCRect){0, 0, decoded->width, decoded->height};
		frame->with_client = TRUE;
		struct VCItem item = {.Type = VCIT_Frame};
		item.u.Frame = pub;
		items_add(session, &item);
		session->counts.pictures++;
	}
	ReleaseSemaphore(&session->lock);
}

/**
 * frames_release() - The client gives a frame back (VCB_ReleaseFrame()).
 * @session: the session, its lock held
 * @maybe:   the client's pointer; one that is no frame of the session's is passed over
 *
 * Its set may be spent now.
 */
void frames_release(struct session *session, struct frame *maybe)
{
	struct frame *frame = frame_find(session, maybe);
	if (frame == NULL)
		return;
	frame->with_client = FALSE;
	set_dispose_if_spent(frame->set);
}

/**
 * frames_flush() - Take back the pictures waiting among the items, not yet polled, and an end mark not yet polled.
 * @session: the session, its lock held
 *
 * Their frames are the client's no longer; a retired set may be spent now.
 */
void frames_flush(struct session *session)
{
	struct items *items = &session->items;
	for (ULONG i = 0; i < items->count; i++)
	{
		struct VCItem *item = &items->item[(items->head + i) % items->room];
		if (item->Type == VCIT_Frame)
		{
			struct frame *frame = (struct frame *)item->u.Frame;
			item->Type = VCIT_None;
			frame->with_client = FALSE;
			set_dispose_if_spent(frame->set);
		}
		else if (item->Type == VCIT_Event && item->u.Event.Event == VCEV_EndOfStream)
			item->Type = VCIT_None;
	}
}

/**
 * frames_dispose_all() - Dispose of every set, current and retired, the session closing.
 * @session: the session; FFmpeg's decoder is gone, so it holds no frame, and the client's are its no longer
 */
void frames_dispose_all(struct session *session)
{
	struct Node *node;
	while ((node = RemHead((struct List *)&session->sets)) != NULL)
		set_dispose((struct frame_set *)node);
}
