/* SPDX-License-Identifier: MPL-2.0 */
/*
 * engine_audio.c - the engine's audio decoder: the queue's packets go in,
 * samples come out and go into the sound output's ring.
 *
 * The output is opened at the first decoded samples, since only they say the
 * true rate (an AAC stream may play at twice the rate its container names).
 * For a unit whose mode is stereo the decoder mixes down to two channels;
 * for one whose mode is 7.1 it gives the stream's channels and they are
 * placed in AHI's order here.  Samples are 32-bit where ahi.device takes
 * them (version 6 on), else 16-bit, and 7.1, which only has a 32-bit sample
 * type, comes with them.  A failure of the sound drops the audio and leaves
 * the film playing.
 */
#include <proto/exec.h>
#include <proto/demuxer.h>
#include <proto/audiocodec.h>

#include "engine_private.h"

#define MICROSECONDS_PER_SECOND 1000000
#define PIECE_FRAMES 256u /* sample frames converted on the stack at a time */

/**
 * audio_open() - Open the decoder for the file's audio stream.
 * @engine: the engine, the file open
 *
 * A stream no decoder takes is left out: the film plays silent.
 */
void audio_open(struct engine *engine)
{
	const struct DMXStream *stream = &engine->audio_stream;
	if (engine->audio_index == NO_STREAM)
		return;
	struct audio_abilities abilities;
	audio_out_abilities(engine->setup.audio_unit, &abilities);
	engine->audio_32bit = abilities.wide;
	engine->audio_surround = abilities.surround;
	audio_out_placement(0, &engine->placement);
	engine->audio_decoder = AC_OpenSessionTags(ACA_Codec, stream->Codec,
	                                           ACA_Extradata, (ULONG)stream->Extradata,
	                                           ACA_ExtradataSize, stream->ExtradataSize,
	                                           ACA_SampleRate, stream->SampleRate,
	                                           ACA_Channels, stream->Channels,
	                                           ACA_Bits, stream->Bits,
	                                           ACA_PcmFlags, stream->PcmFlags,
	                                           ACA_OutputFormat, engine->audio_32bit ? ACF_S32 : ACF_S16,
	                                           ACA_OutputChannels, engine->audio_surround ? 0 : AUDIO_STEREO_CHANNELS,
	                                           TAG_DONE);
}

/**
 * audio_close() - Close the decoder and the sound output.
 * @engine: the engine; without them nothing happens
 */
void audio_close(struct engine *engine)
{
	audio_out_close(engine->audio_out);
	engine->audio_out = NULL;
	if (engine->audio_decoder != NULL)
		AC_CloseSession(engine->audio_decoder);
	engine->audio_decoder = NULL;
	engine->pending_held = FALSE;
	engine->audio_end_sent = FALSE;
	engine->audio_done = FALSE;
}

/**
 * audio_drop() - Give the sound up and let the film play without it.
 * @engine: the engine
 *
 * The packets read ahead for it are released, and a clock that waited for
 * the sound starts by itself.
 */
static void audio_drop(struct engine *engine)
{
	audio_close(engine);
	reader_hold(engine);
	reader_flush(engine, &engine->audio_queue);
	reader_let_go(engine);
	engine_clock_start(engine);
}

/**
 * output_open() - Open the sound output for the first decoded samples.
 * @engine: the engine
 * @frame:  the samples
 *
 * Return: TRUE with the output open, FALSE when ahi.device refused.
 */
static BOOL output_open(struct engine *engine, const struct ACFrame *frame)
{
	struct audio_setup setup = {
	    .unit = engine->setup.audio_unit,
	    .shared = engine->setup.audio_shared,
	    .rate = frame->SampleRate,
	    .channels = engine->audio_surround ? AUDIO_SURROUND_CHANNELS : AUDIO_STEREO_CHANNELS,
	    .wide = engine->audio_32bit,
	    .delay = engine->audio_delay,
	    .clock = &engine->clock,
	    .listener = FindTask(NULL),
	    .signal = 1UL << engine->audio_signal,
	};
	LONG error = 0;
	engine->audio_out = audio_out_open(&setup, &error);
	if (engine->audio_out == NULL)
		return FALSE;
	audio_out_volume(engine->audio_out, engine->volume);
	audio_out_run(engine->audio_out, engine->state == ENGINE_PLAYING);
	return TRUE;
}

/**
 * mono_spread() - Put one channel's samples on both sides of stereo sample frames.
 * @samples: the channel's samples, LONG when @wide, else WORD
 * @frames:  how many
 * @wide:    32-bit samples
 * @stereo:  gets @frames stereo sample frames of the same width
 */
static void mono_spread(CONST_APTR samples, ULONG frames, BOOL wide, APTR stereo)
{
	if (wide)
	{
		const LONG *from = samples;
		LONG *to = stereo;
		for (ULONG frame = 0; frame < frames; frame++)
			to[frame * AUDIO_STEREO_CHANNELS] = to[frame * AUDIO_STEREO_CHANNELS + 1] = from[frame];
		return;
	}
	const WORD *from = samples;
	WORD *to = stereo;
	for (ULONG frame = 0; frame < frames; frame++)
		to[frame * AUDIO_STEREO_CHANNELS] = to[frame * AUDIO_STEREO_CHANNELS + 1] = from[frame];
}

/**
 * piece_convert() - Make the next sample frames of the pending samples what the output takes.
 * @engine: the engine
 * @frames: how many, PIECE_FRAMES at most
 * @piece:  room for them in the output's shape
 *
 * Return: the sample frames to write: @piece, or the decoder's own memory where they are right as they are.
 */
static CONST_APTR piece_convert(struct engine *engine, ULONG frames, LONG *piece)
{
	const struct ACFrame *from = &engine->pending_samples;
	if (engine->audio_surround)
	{
		if (engine->placement.layout != from->Layout)
			audio_out_placement(from->Layout, &engine->placement);
		audio_out_place((const LONG *)from->Data + engine->pending_written * from->Channels, frames, &engine->placement, piece);
		return piece;
	}
	ULONG sample_bytes = engine->audio_32bit ? sizeof(LONG) : sizeof(WORD);
	const UBYTE *samples = (const UBYTE *)from->Data + engine->pending_written * from->Channels * sample_bytes;
	if (from->Channels == AUDIO_STEREO_CHANNELS)
		return samples;
	mono_spread(samples, frames, engine->audio_32bit, piece); /* one channel: the same on both sides */
	return piece;
}

/**
 * pending_write() - Move the pending samples into the output's ring, as far as it has room.
 * @engine: the engine, samples pending
 *
 * Return: TRUE when all are in, FALSE when the ring is full.
 */
static BOOL pending_write(struct engine *engine)
{
	const struct ACFrame *from = &engine->pending_samples;
	while (engine->pending_written < from->Samples)
	{
		LONG piece[PIECE_FRAMES * AUDIO_SURROUND_CHANNELS];
		ULONG frames = from->Samples - engine->pending_written;
		if (frames > PIECE_FRAMES)
			frames = PIECE_FRAMES;
		ULONG room = audio_out_room(engine->audio_out);
		if (frames > room)
			frames = room;
		if (frames == 0)
			return FALSE;
		VCTime time = from->Pts;
		if (time != VC_TIME_UNKNOWN)
			time += (VCTime)engine->pending_written * MICROSECONDS_PER_SECOND / from->SampleRate;
		audio_out_write(engine->audio_out, piece_convert(engine, frames, piece), frames, time);
		engine->pending_written += frames;
	}
	engine->pending_held = FALSE;
	return TRUE;
}

/**
 * audio_feed() - Give the decoder the queue's next packet, or the stream's end.
 * @engine: the engine
 *
 * Return: TRUE when the decoder got something, FALSE when there is nothing to give it now.
 */
static BOOL audio_feed(struct engine *engine)
{
	BOOL ended;
	const struct DMXPacket *from = reader_next(engine, &engine->audio_queue, &ended);
	struct ACPacket packet = {.Size = sizeof(packet), .Pts = VC_TIME_UNKNOWN}; /* Length 0: the end */
	if (from == NULL && (!ended || engine->audio_end_sent))
		return FALSE;
	if (from != NULL)
	{
		packet.Data = from->Data;
		packet.Length = from->Length;
		packet.Pts = from->Pts;
	}
	if (AC_SendPacket(engine->audio_decoder, &packet) == ACERR_AGAIN)
		return TRUE; /* the decoder has frames to give first; the packet stays */
	if (from == NULL)
	{
		engine->audio_end_sent = TRUE;
		return TRUE;
	}
	reader_take(engine, &engine->audio_queue);
	return TRUE;
}

/**
 * frame_take() - Take the decoder's next samples as the pending ones.
 * @engine: the engine, nothing pending
 *
 * Return: TRUE to go on: samples were taken, or the decoder was fed, or a bad
 * packet was passed over; FALSE to stop: nothing to feed it, its end, or the
 * sound given up.
 */
static BOOL frame_take(struct engine *engine)
{
	struct ACFrame frame = {.Size = sizeof(frame)};
	LONG error = AC_ReceiveFrame(engine->audio_decoder, &frame);
	if (error == ACERR_AGAIN)
		return audio_feed(engine);
	if (error == ACERR_DATA)
		return TRUE;
	if (error == ACERR_END)
	{
		engine->audio_done = TRUE;
		return FALSE;
	}
	if (error != 0 || frame.SampleRate == 0 || (engine->audio_out == NULL && !output_open(engine, &frame)))
	{
		audio_drop(engine);
		return FALSE;
	}
	engine->pending_samples = frame;
	engine->pending_written = 0;
	engine->pending_held = TRUE;
	return TRUE;
}

/**
 * audio_pump() - One pass of the audio decoder: decode until the output's ring is full or the queue is empty.
 * @engine: the engine
 */
void audio_pump(struct engine *engine)
{
	if (engine->audio_out != NULL)
		audio_out_watch(engine->audio_out);
	while (engine->audio_decoder != NULL && !engine->audio_done)
	{
		if (engine->pending_held && !pending_write(engine))
			break;
		if (!frame_take(engine))
			break;
	}
}

/**
 * audio_flush() - Drop what the decoder and the output hold, for a seek.
 * @engine: the engine
 */
void audio_flush(struct engine *engine)
{
	if (engine->audio_decoder == NULL)
		return;
	AC_Flush(engine->audio_decoder);
	if (engine->audio_out != NULL)
		audio_out_flush(engine->audio_out);
	engine->pending_held = FALSE;
	engine->audio_end_sent = FALSE;
	engine->audio_done = FALSE;
}
