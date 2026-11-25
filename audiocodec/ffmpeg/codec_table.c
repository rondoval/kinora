/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_table.c - the codecs ffmpeg.codec decodes: its rows of the codec
 * table, and FFmpeg's decoder for an ACC_*.
 */
#include <libavutil/avstring.h>

#include "codec_private.h"

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

/* A codec this backend has a row for */
struct codec_row
{
	ULONG codec;       /* ACC_* */
	enum AVCodecID id; /* FFmpeg's; AV_CODEC_ID_NONE for ACC_PCM, which pcm_decoders[] resolves */
	const char *name;  /* for people */
};

static const struct codec_row codec_rows[] = {
    {ACC_AAC, AV_CODEC_ID_AAC, "AAC"},
    {ACC_AAC_LATM, AV_CODEC_ID_AAC_LATM, "AAC (LATM)"},
    {ACC_AC3, AV_CODEC_ID_AC3, "AC-3"},
    {ACC_EAC3, AV_CODEC_ID_EAC3, "E-AC-3"},
    {ACC_DTS, AV_CODEC_ID_DTS, "DTS"},
    {ACC_MP3, AV_CODEC_ID_MP3, "MP3"},
    {ACC_MP2, AV_CODEC_ID_MP2, "MP2"},
    {ACC_Opus, AV_CODEC_ID_OPUS, "Opus"},
    {ACC_Vorbis, AV_CODEC_ID_VORBIS, "Vorbis"},
    {ACC_FLAC, AV_CODEC_ID_FLAC, "FLAC"},
    {ACC_TrueHD, AV_CODEC_ID_TRUEHD, "TrueHD"},
    {ACC_PCM, AV_CODEC_ID_NONE, "PCM"},
};

/* An uncompressed layout and FFmpeg's decoder for it */
struct pcm_decoder
{
	ULONG bits;        /* bits a sample */
	ULONG flags;       /* ACPCM_* */
	enum AVCodecID id; /* FFmpeg's */
};

static const struct pcm_decoder pcm_decoders[] = {
    {8, ACPCM_Unsigned, AV_CODEC_ID_PCM_U8},
    {16, 0, AV_CODEC_ID_PCM_S16LE},
    {16, ACPCM_BigEndian, AV_CODEC_ID_PCM_S16BE},
    {24, 0, AV_CODEC_ID_PCM_S24LE},
    {24, ACPCM_BigEndian, AV_CODEC_ID_PCM_S24BE},
    {32, 0, AV_CODEC_ID_PCM_S32LE},
    {32, ACPCM_BigEndian, AV_CODEC_ID_PCM_S32BE},
    {32, ACPCM_Float, AV_CODEC_ID_PCM_F32LE},
    {32, ACPCM_Float | ACPCM_BigEndian, AV_CODEC_ID_PCM_F32BE},
};

/**
 * table_rows() - How many rows the backend has.
 *
 * Return: the count.
 */
ULONG table_rows(void)
{
	return ARRAY_COUNT(codec_rows);
}

/**
 * table_row() - One row of the codec table.
 * @index: the row
 * @info:  the caller's structure, its Size set
 *
 * Return: 0, ACERR_BADPARAM past the last row.
 */
LONG table_row(ULONG index, struct ACCodecInfo *info)
{
	if (index >= ARRAY_COUNT(codec_rows))
		return ACERR_BADPARAM;
	struct ACCodecInfo answer = {
	    .Codec = codec_rows[index].codec,
	    .Priority = ACB_PRIORITY_SOFTWARE,
	};
	av_strlcpy(answer.Name, codec_rows[index].name, sizeof(answer.Name));
	ULONG size = info->Size < sizeof(answer) ? info->Size : (ULONG)sizeof(answer);
	answer.Size = size;
	memcpy(info, &answer, size);
	return 0;
}

/**
 * pcm_decoder_id() - FFmpeg's decoder for uncompressed samples.
 * @bits:  bits a sample
 * @flags: ACPCM_*
 *
 * Return: its codec, AV_CODEC_ID_NONE for a layout there is none for.
 */
static enum AVCodecID pcm_decoder_id(ULONG bits, ULONG flags)
{
	for (ULONG i = 0; i < ARRAY_COUNT(pcm_decoders); i++)
		if (pcm_decoders[i].bits == bits && pcm_decoders[i].flags == flags)
			return pcm_decoders[i].id;
	return AV_CODEC_ID_NONE;
}

/**
 * table_decoder() - FFmpeg's decoder for a session.
 * @params: the session's parameters
 *
 * Return: the decoder, NULL for a codec or sample layout the backend has none for.
 */
const AVCodec *table_decoder(const struct ACSessionParams *params)
{
	enum AVCodecID id = AV_CODEC_ID_NONE;
	if (params->Codec == ACC_PCM)
		id = pcm_decoder_id(params->Bits, params->PcmFlags);
	else
		for (ULONG i = 0; i < ARRAY_COUNT(codec_rows); i++)
			if (codec_rows[i].codec == params->Codec)
				id = codec_rows[i].id;
	return id != AV_CODEC_ID_NONE ? avcodec_find_decoder(id) : NULL;
}
