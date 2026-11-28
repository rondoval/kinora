/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_session.c - a session: FFmpeg's decoder, and behind it FFmpeg's
 * converter, which turns whatever the decoder gives (planar or not, float or
 * integer, any channel order) into interleaved float samples in the WAVE
 * order and mixes down when the client asked for fewer channels; the samples
 * are then scaled to the signed integers asked for.
 *
 * AC-3, E-AC-3, DTS and TrueHD decoders are asked to mix down themselves,
 * with the levels their streams carry; the converter mixes only what they
 * leave.  A DTS stream mixed down is decoded from its core alone: the
 * extensions (lossless, extra channels, 96 kHz) are what costs, a lossless
 * one about a quarter of a PiStorm's 68k, and a mix down keeps next to
 * nothing of them.  Nothing is resampled, so every frame converts to as many
 * sample frames as it has.
 *
 * A mix down keeps its sources' levels.  FFmpeg scales its mixes so that
 * the loudest moment cannot pass full scale, which puts dialogue in a 5.1
 * film 10.7 dB down; the AC-3 decoders are told not to (Kinora's FFmpeg
 * patch 0002) and the converter is never let to.  What then passes full
 * scale is bent back below it (sample_limit()).
 */
#include <string.h>

#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

#include "codec_private.h"

#define MICROSECONDS_PER_SECOND 1000000
#define NAMED_CHANNELS 0x7ffu      /* the channels libraries/audiocodec.h has ACCH_* bits for */
#define REMATRIX_UNBOUNDED 1000.0  /* the converter's rematrix_maxval at its largest: a mix it makes is never scaled down */
#define LIMIT_KNEE 0.75f           /* of full scale: a mixed sample up to this far from silence passes as it is */
#define S16_SCALE 32768.0f         /* full scale in 16-bit samples */
#define S16_MOST 32767
#define S16_LEAST (-32768)
#define S32_SCALE 2147483648.0f    /* and in 32-bit ones */
#define S32_MOST 0x7fffffffL
#define S32_LEAST (-0x7fffffffL - 1)
#define ROUNDING 0.5f

/**
 * error_of() - An FFmpeg error as an ACERR_*.
 * @code:  FFmpeg's, negative
 * @other: what anything but "again", the end and no memory becomes
 *
 * Return: the ACERR_*.
 */
static LONG error_of(int code, LONG other)
{
	if (code == AVERROR(EAGAIN))
		return ACERR_AGAIN;
	if (code == AVERROR_EOF)
		return ACERR_END;
	if (code == AVERROR(ENOMEM))
		return ACERR_NOMEM;
	return other;
}

/**
 * extradata_copy() - Give the decoder its own copy of the codec's configuration.
 * @decoder: the decoder, not yet open
 * @params:  the session's parameters
 *
 * Return: TRUE, FALSE without memory.
 */
static BOOL extradata_copy(AVCodecContext *decoder, const struct ACSessionParams *params)
{
	if (params->ExtradataSize == 0)
		return TRUE;
	decoder->extradata = av_mallocz(params->ExtradataSize + AV_INPUT_BUFFER_PADDING_SIZE);
	if (decoder->extradata == NULL)
		return FALSE;
	memcpy(decoder->extradata, params->Extradata, params->ExtradataSize);
	decoder->extradata_size = (int)params->ExtradataSize;
	return TRUE;
}

/**
 * decoder_open() - Make and open FFmpeg's decoder for a session.
 * @session: the session, its output_channels set
 * @codec:   FFmpeg's decoder
 * @params:  the session's parameters
 *
 * Return: 0, ACERR_NOMEM or ACERR_UNSUPPORTED.
 */
static LONG decoder_open(struct session *session, const AVCodec *codec, const struct ACSessionParams *params)
{
	AVCodecContext *decoder = avcodec_alloc_context3(codec);
	session->decoder = decoder;
	if (decoder == NULL || !extradata_copy(decoder, params))
		return ACERR_NOMEM;

	decoder->sample_rate = (int)params->SampleRate;
	decoder->bits_per_coded_sample = (int)params->Bits;
	if (AC_HAS_FIELD(params->Size, struct ACSessionParams, BlockAlign))
		decoder->block_align = (int)params->BlockAlign; /* ADPCM's blocks: a packet may hold several */
	decoder->pkt_timebase = (AVRational){1, MICROSECONDS_PER_SECOND};
	if (params->Channels != 0)
		av_channel_layout_default(&decoder->ch_layout, (int)params->Channels);
	if (session->output_channels != 0 && decoder->priv_data != NULL)
	{
		/* a decoder without an option says so, and goes on without it: the converter mixes instead */
		AVChannelLayout wanted;
		av_channel_layout_default(&wanted, (int)session->output_channels);
		av_opt_set_chlayout(decoder->priv_data, "downmix", &wanted, 0);
		av_opt_set_int(decoder->priv_data, "core_only", 1, 0);         /* DTS's */
		av_opt_set_int(decoder->priv_data, "normalize_downmix", 0, 0); /* AC-3's and E-AC-3's, from patch 0002 */
	}
	return avcodec_open2(decoder, codec, NULL) == 0 ? 0 : ACERR_UNSUPPORTED;
}

/**
 * session_open() - Open a decoder.
 * @params: the session's parameters
 * @error:  gets the ACERR_* of a refusal
 *
 * Return: the session, NULL when refused.
 */
struct session *session_open(const struct ACSessionParams *params, LONG *error)
{
	const AVCodec *codec = table_decoder(params);
	if (codec == NULL)
	{
		*error = ACERR_UNSUPPORTED;
		return NULL;
	}
	struct session *session = av_mallocz(sizeof(*session));
	if (session == NULL)
	{
		*error = ACERR_NOMEM;
		return NULL;
	}
	session->output_format = params->OutputFormat;
	session->output_channels = params->OutputChannels;
	session->stream_channels = params->Channels;
	session->next_time = AV_NOPTS_VALUE;
	session->input.format = AV_SAMPLE_FMT_NONE;
	session->packet = av_packet_alloc();
	session->frame = av_frame_alloc();

	*error = session->packet != NULL && session->frame != NULL ? decoder_open(session, codec, params) : ACERR_NOMEM;
	if (*error != 0)
	{
		session_close(session);
		return NULL;
	}
	return session;
}

/**
 * session_close() - Close a decoder and free everything of it.
 * @session: the session, whole or half made
 */
void session_close(struct session *session)
{
	swr_free(&session->converter);
	av_channel_layout_uninit(&session->input.layout);
	av_channel_layout_uninit(&session->output_layout);
	av_frame_free(&session->frame);
	av_packet_free(&session->packet);
	avcodec_free_context(&session->decoder);
	av_free(session->mixed);
	av_free(session->samples);
	av_free(session);
}

/**
 * session_send() - Give the decoder a packet.
 * @session: the session
 * @packet:  the packet; a Length of 0 is the stream's end
 *
 * The bytes are copied: FFmpeg reads past a packet's end, into padding the
 * caller's memory need not have.
 *
 * Return: 0, ACERR_AGAIN, ACERR_END after the end was sent, ACERR_NOMEM or ACERR_DATA.
 */
LONG session_send(struct session *session, const struct ACPacket *packet)
{
	if (packet->Length == 0)
	{
		int code = avcodec_send_packet(session->decoder, NULL);
		return code == 0 ? 0 : error_of(code, ACERR_DATA);
	}
	if (av_new_packet(session->packet, (int)packet->Length) != 0)
		return ACERR_NOMEM;
	memcpy(session->packet->data, packet->Data, packet->Length);
	session->packet->pts = packet->Pts == VC_TIME_UNKNOWN ? AV_NOPTS_VALUE : packet->Pts;
	int code = avcodec_send_packet(session->decoder, session->packet);
	av_packet_unref(session->packet);
	return code == 0 ? 0 : error_of(code, ACERR_DATA);
}

/**
 * layout_named() - Whether a channel layout is one this API can name as it is.
 * @layout: the layout
 *
 * Return: TRUE for channels in FFmpeg's native order that all have an ACCH_* bit.
 */
static BOOL layout_named(const AVChannelLayout *layout)
{
	return layout->order == AV_CHANNEL_ORDER_NATIVE && (layout->u.mask & ~(uint64_t)NAMED_CHANNELS) == 0;
}

/**
 * output_layout_set() - Choose the channels the converter gives for the decoder's.
 * @session: the session, its input layout set
 *
 * The decoder's own when the client takes that many and they can be named;
 * else the usual layout of the number wanted.
 */
static void output_layout_set(struct session *session)
{
	ULONG decoded = (ULONG)session->input.layout.nb_channels;
	ULONG wanted = session->output_channels != 0 && session->output_channels < decoded ? session->output_channels : decoded;

	av_channel_layout_uninit(&session->output_layout);
	if (wanted == decoded && layout_named(&session->input.layout))
		av_channel_layout_copy(&session->output_layout, &session->input.layout);
	else
		av_channel_layout_default(&session->output_layout, (int)wanted);
}

/**
 * converter_current() - Whether the converter is set up for a frame's samples.
 * @session: the session
 * @frame:   the decoded frame
 *
 * Return: TRUE if it is.
 */
static BOOL converter_current(const struct session *session, const AVFrame *frame)
{
	return session->converter != NULL && session->input.format == frame->format && session->input.rate == frame->sample_rate &&
	       av_channel_layout_compare(&session->input.layout, &frame->ch_layout) == 0;
}

/**
 * converter_make() - Set the converter up for a frame's samples.
 * @session: the session
 * @frame:   the decoded frame
 *
 * A decoder that names no order for its channels gets the usual layout of
 * their number.
 *
 * Return: 0, ACERR_NOMEM or ACERR_UNSUPPORTED.
 */
static LONG converter_make(struct session *session, const AVFrame *frame)
{
	swr_free(&session->converter);
	av_channel_layout_uninit(&session->input.layout);
	if (av_channel_layout_copy(&session->input.layout, &frame->ch_layout) != 0)
		return ACERR_NOMEM;
	session->input.format = frame->format;
	session->input.rate = frame->sample_rate;

	AVChannelLayout source = session->input.layout;
	if (source.order != AV_CHANNEL_ORDER_NATIVE)
		av_channel_layout_default(&source, source.nb_channels);
	output_layout_set(session);
	session->converter_mixes = session->output_layout.nb_channels < source.nb_channels;
	int code = swr_alloc_set_opts2(&session->converter, &session->output_layout, AV_SAMPLE_FMT_FLT, frame->sample_rate, &source,
	                               frame->format, frame->sample_rate, 0, NULL);
	if (code == 0 && session->converter_mixes)
		code = av_opt_set_double(session->converter, "rematrix_maxval", REMATRIX_UNBOUNDED, 0);
	if (code == 0)
		code = swr_init(session->converter);
	if (code != 0)
		swr_free(&session->converter);
	return code == 0 ? 0 : error_of(code, ACERR_UNSUPPORTED);
}

/**
 * sample_limit() - Bend a sample that passes the knee back towards full scale, never to it.
 * @sample: the sample, full scale at 1
 *
 * Beyond the knee a sample's excess over it is squeezed into what is left up
 * to full scale, by excess / (1 + excess) of that room: a sample just past
 * the knee is nearly unchanged, and one however far beyond stays short of
 * full scale.  The curve meets the straight line at the knee with the same
 * slope, so nothing clicks there.
 *
 * Return: the sample, within full scale.
 */
static float sample_limit(float sample)
{
	float size = sample < 0 ? -sample : sample;
	if (size <= LIMIT_KNEE)
		return sample;
	float room = 1.0f - LIMIT_KNEE;
	float excess = (size - LIMIT_KNEE) / room;
	float bent = LIMIT_KNEE + room * excess / (1.0f + excess);
	return sample < 0 ? -bent : bent;
}

/**
 * samples_put() - Scale float samples to the 16-bit or 32-bit ones asked for.
 * @from:   the samples, full scale at 1
 * @count:  how many
 * @format: ACF_S16 or ACF_S32
 * @limit:  they were mixed down and may pass full scale: bend them back (sample_limit()); else cut them at it
 * @to:     gets @count samples of @format
 */
static void samples_put(const float *from, ULONG count, ULONG format, BOOL limit, APTR to)
{
	for (ULONG i = 0; i < count; i++)
	{
		float sample = limit ? sample_limit(from[i]) : from[i];
		if (format == ACF_S32)
		{
			float scaled = sample * S32_SCALE;
			((LONG *)to)[i] = scaled >= S32_SCALE ? S32_MOST : scaled <= -S32_SCALE ? S32_LEAST : (LONG)scaled;
		}
		else
		{
			float scaled = sample * S16_SCALE + (sample < 0 ? -ROUNDING : ROUNDING);
			((WORD *)to)[i] = (WORD)(scaled >= S16_MOST ? S16_MOST : scaled <= S16_LEAST ? S16_LEAST : (LONG)scaled);
		}
	}
}

/**
 * frame_mixed() - Whether the decoded frame's channels were mixed down, by the decoder or by the converter.
 * @session: the session, its converter current
 *
 * Return: TRUE if they were.
 */
static BOOL frame_mixed(const struct session *session)
{
	ULONG decoded = (ULONG)session->frame->ch_layout.nb_channels;
	return session->converter_mixes || (session->stream_channels != 0 && decoded < session->stream_channels);
}

/**
 * frame_convert() - Convert the decoded frame into the session's sample buffer.
 * @session: the session, its converter current
 * @answer:  gets Data, Samples, Channels, Layout and Format
 *
 * Return: 0, ACERR_NOMEM or ACERR_INTERNAL.
 */
static LONG frame_convert(struct session *session, struct ACFrame *answer)
{
	const AVFrame *frame = session->frame;
	ULONG channels = (ULONG)session->output_layout.nb_channels;
	ULONG sample_bytes = session->output_format == ACF_S32 ? sizeof(LONG) : sizeof(WORD);
	size_t count = (size_t)frame->nb_samples * channels;

	av_fast_malloc(&session->mixed, &session->mixed_room, count * sizeof(float));
	av_fast_malloc(&session->samples, &session->samples_room, count * sample_bytes);
	if (session->mixed == NULL || session->samples == NULL)
		return ACERR_NOMEM;
	uint8_t *planes[1] = {(uint8_t *)session->mixed};
	int converted = swr_convert(session->converter, planes, frame->nb_samples, (const uint8_t **)frame->extended_data, frame->nb_samples);
	if (converted < 0)
		return ACERR_INTERNAL;
	samples_put(session->mixed, (ULONG)converted * channels, session->output_format, frame_mixed(session), session->samples);

	answer->Data = session->samples;
	answer->Samples = (ULONG)converted;
	answer->Channels = channels;
	answer->Layout = (ULONG)session->output_layout.u.mask;
	answer->Format = session->output_format;
	return 0;
}

/**
 * frame_time() - The time of the decoded frame's first sample, and where the next begins.
 * @session: the session
 *
 * A frame without a time continues from the one before.
 *
 * Return: the time, VC_TIME_UNKNOWN when none is known yet.
 */
static VCTime frame_time(struct session *session)
{
	const AVFrame *frame = session->frame;
	int64_t time = frame->pts != AV_NOPTS_VALUE ? frame->pts : session->next_time;
	if (time == AV_NOPTS_VALUE || frame->sample_rate <= 0)
		return VC_TIME_UNKNOWN;
	session->next_time = time + (int64_t)frame->nb_samples * MICROSECONDS_PER_SECOND / frame->sample_rate;
	return time;
}

/**
 * session_receive() - Take the next decoded samples.
 * @session: the session
 * @frame:   the caller's structure, whole
 *
 * Return: 0, ACERR_AGAIN, ACERR_END, ACERR_DATA, ACERR_NOMEM, ACERR_UNSUPPORTED or ACERR_INTERNAL.
 */
LONG session_receive(struct session *session, struct ACFrame *frame)
{
	int code = avcodec_receive_frame(session->decoder, session->frame);
	if (code != 0)
		return error_of(code, ACERR_DATA);

	LONG error = converter_current(session, session->frame) ? 0 : converter_make(session, session->frame);
	if (error == 0)
		error = frame_convert(session, frame);
	if (error == 0)
	{
		frame->SampleRate = (ULONG)session->frame->sample_rate;
		frame->Pts = frame_time(session);
	}
	av_frame_unref(session->frame);
	return error;
}

/**
 * session_flush() - Drop what the decoder holds.
 * @session: the session
 *
 * Return: 0.
 */
LONG session_flush(struct session *session)
{
	avcodec_flush_buffers(session->decoder);
	session->next_time = AV_NOPTS_VALUE;
	return 0;
}
