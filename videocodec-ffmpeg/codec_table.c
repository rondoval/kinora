/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_table.c - the codecs ffmpegvideo.codec decodes: its rows of the codec table, and
 * FFmpeg's decoder for a session's codec and FourCC.
 *
 * The rows' sizes are what the formats allow, not what keeps up (docs/architecture.md,
 * section 14): a machine at 2.5 GHz plays what one at 1.8 cannot, and a film that chokes
 * skips.  A picture of larger area than its row's is refused.
 */
#include <string.h>

#include <libavutil/avstring.h>

#include "codec_private.h"

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define LARGE_WIDTH 1920   /* MPEG-1/2, MPEG-4 Part 2, MS-MPEG4 and Theora: up to 1920x1088 */
#define LARGE_HEIGHT 1088
#define H263_WIDTH 1408    /* H.263's largest standard picture, 16CIF */
#define H263_HEIGHT 1152

/* A codec this backend has a row for */
struct codec_row
{
	ULONG codec;           /* VCC_* */
	enum AVCodecID id;     /* FFmpeg's, for a FourCC no form below names */
	ULONG width, height;   /* the largest picture: its area is the limit */
	BOOL parsed;           /* an unframed session's packets go through FFmpeg's parser */
};

static const struct codec_row codec_rows[] = {
    {VCC_MPEG1, AV_CODEC_ID_MPEG1VIDEO, LARGE_WIDTH, LARGE_HEIGHT, TRUE},
    {VCC_MPEG2, AV_CODEC_ID_MPEG2VIDEO, LARGE_WIDTH, LARGE_HEIGHT, TRUE},
    {VCC_MPEG4, AV_CODEC_ID_MPEG4, LARGE_WIDTH, LARGE_HEIGHT, TRUE},
    {VCC_MSMPEG4, AV_CODEC_ID_MSMPEG4V3, LARGE_WIDTH, LARGE_HEIGHT, FALSE},
    {VCC_H263, AV_CODEC_ID_H263, H263_WIDTH, H263_HEIGHT, FALSE},
    {VCC_Theora, AV_CODEC_ID_THEORA, LARGE_WIDTH, LARGE_HEIGHT, FALSE},
};

/* A form of a codec that has a decoder of its own, by the container's FourCC */
struct codec_form
{
	ULONG codec;       /* VCC_* */
	ULONG tag;         /* VC_FOURCC(), upper case */
	enum AVCodecID id; /* FFmpeg's decoder for it */
};

static const struct codec_form codec_forms[] = {
    {VCC_MSMPEG4, VC_FOURCC('M', 'P', 'G', '4'), AV_CODEC_ID_MSMPEG4V1},
    {VCC_MSMPEG4, VC_FOURCC('M', 'P', '4', '1'), AV_CODEC_ID_MSMPEG4V1},
    {VCC_MSMPEG4, VC_FOURCC('D', 'I', 'V', '1'), AV_CODEC_ID_MSMPEG4V1},
    {VCC_MSMPEG4, VC_FOURCC('M', 'P', '4', '2'), AV_CODEC_ID_MSMPEG4V2},
    {VCC_MSMPEG4, VC_FOURCC('D', 'I', 'V', '2'), AV_CODEC_ID_MSMPEG4V2},
    {VCC_H263, VC_FOURCC('F', 'L', 'V', '1'), AV_CODEC_ID_FLV1},
    {VCC_H263, VC_FOURCC('I', '2', '6', '3'), AV_CODEC_ID_H263I},
};

/**
 * row_of() - The row of a codec.
 * @codec: VCC_*
 *
 * Return: the row, NULL for a codec the backend does not decode.
 */
static const struct codec_row *row_of(ULONG codec)
{
	for (ULONG i = 0; i < ARRAY_COUNT(codec_rows); i++)
		if (codec_rows[i].codec == codec)
			return &codec_rows[i];
	return NULL;
}

/**
 * tag_fold() - A FourCC with its letters in upper case, as the forms are listed.
 * @tag: VC_FOURCC()
 *
 * Return: the FourCC, each byte from 'a' to 'z' made upper case.
 */
static ULONG tag_fold(ULONG tag)
{
	ULONG folded = 0;
	for (int shift = 24; shift >= 0; shift -= 8)
	{
		UBYTE c = (UBYTE)(tag >> shift);
		if (c >= 'a' && c <= 'z')
			c = (UBYTE)(c - 'a' + 'A');
		folded |= (ULONG)c << shift;
	}
	return folded;
}

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
 * Every row decodes for sessions to I420, at software's priority.
 *
 * Return: 0, VCERR_BADPARAM past the last row.
 */
LONG table_row(ULONG index, struct VCCodecInfo *info)
{
	if (index >= ARRAY_COUNT(codec_rows))
		return VCERR_BADPARAM;
	struct VCCodecInfo answer = {
	    .Codec = codec_rows[index].codec,
	    .Kind = VCK_Decoder,
	    .MaxWidth = codec_rows[index].width,
	    .MaxHeight = codec_rows[index].height,
	    .OutputFormats = VCF_MASK(VCF_I420),
	    .Priority = VCB_PRIORITY_SOFTWARE,
	    .Flags = VCCF_Session,
	};
	av_strlcpy((char *)answer.Backend, BACKEND_NAME, sizeof(answer.Backend));
	ULONG size = info->Size < sizeof(answer) ? info->Size : (ULONG)sizeof(answer);
	answer.Size = size;
	memcpy(info, &answer, size);
	return 0;
}

/**
 * table_decoder() - FFmpeg's decoder for a session.
 * @codec: VCC_*
 * @tag:   the container's FourCC (VCA_CodecTag), 0 for none
 *
 * A form of the codec with a decoder of its own (MS-MPEG4's versions, Sorenson's and Intel's
 * H.263) goes by its FourCC; any other FourCC, and none, gets the codec's own decoder
 * (MS-MPEG4 version 3, DivX 3, for MS-MPEG4).
 *
 * Return: the decoder, NULL for a codec the backend does not decode.
 */
const AVCodec *table_decoder(ULONG codec, ULONG tag)
{
	const struct codec_row *row = row_of(codec);
	if (row == NULL)
		return NULL;
	ULONG folded = tag_fold(tag);
	for (ULONG i = 0; i < ARRAY_COUNT(codec_forms); i++)
		if (codec_forms[i].codec == codec && codec_forms[i].tag == folded)
			return avcodec_find_decoder(codec_forms[i].id);
	return avcodec_find_decoder(row->id);
}

/**
 * table_max_area() - The largest picture a codec's row takes, in pixels.
 * @codec: VCC_*, one of the rows'
 *
 * Return: the area.
 */
ULONG table_max_area(ULONG codec)
{
	const struct codec_row *row = row_of(codec);
	return row != NULL ? row->width * row->height : 0;
}

/**
 * table_parsed() - Whether an unframed session of a codec goes through FFmpeg's parser.
 * @codec: VCC_*, one of the rows'
 *
 * MPEG-1, MPEG-2 and MPEG-4 Part 2 come as any cut of their start codes; the others a
 * picture a packet only.
 *
 * Return: TRUE if it does.
 */
BOOL table_parsed(ULONG codec)
{
	const struct codec_row *row = row_of(codec);
	return row != NULL && row->parsed;
}
