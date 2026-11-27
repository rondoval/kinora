/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * demuxer_codecs.c - FFmpeg's codec numbers and colour description as the
 * public headers' own: VCC_* (libraries/videocodec.h), ACC_*
 * (libraries/audiocodec.h), DMXS_* and VCCS_*.
 */
#include "demuxer_private.h"

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define RECORD_VERSION 1 /* the first byte of an avcC or hvcC record: the file holds length-prefixed NAL units */

/* An FFmpeg codec and the number this library gives it */
struct codec_name
{
	enum AVCodecID id; /* FFmpeg's */
	ULONG number;      /* VCC_*, ACC_* or DMXS_* */
};

static const struct codec_name codec_names[] = {
    {AV_CODEC_ID_H264, VCC_H264},
    {AV_CODEC_ID_HEVC, VCC_H265},
    {AV_CODEC_ID_MJPEG, VCC_MJPEG},
    {AV_CODEC_ID_MPEG1VIDEO, VCC_MPEG1},
    {AV_CODEC_ID_MPEG2VIDEO, VCC_MPEG2},
    {AV_CODEC_ID_MPEG4, VCC_MPEG4},
    {AV_CODEC_ID_MSMPEG4V1, VCC_MSMPEG4}, /* the version goes by the FourCC (DMXStream.Tag) */
    {AV_CODEC_ID_MSMPEG4V2, VCC_MSMPEG4},
    {AV_CODEC_ID_MSMPEG4V3, VCC_MSMPEG4},
    {AV_CODEC_ID_H263, VCC_H263},
    {AV_CODEC_ID_H263P, VCC_H263},
    {AV_CODEC_ID_H263I, VCC_H263},
    {AV_CODEC_ID_FLV1, VCC_H263},
    {AV_CODEC_ID_VP8, VCC_VP8},
    {AV_CODEC_ID_VP9, VCC_VP9},
    {AV_CODEC_ID_AV1, VCC_AV1},
    {AV_CODEC_ID_VC1, VCC_VC1},
    {AV_CODEC_ID_THEORA, VCC_Theora},
    {AV_CODEC_ID_AAC, ACC_AAC},
    {AV_CODEC_ID_AAC_LATM, ACC_AAC_LATM},
    {AV_CODEC_ID_AC3, ACC_AC3},
    {AV_CODEC_ID_EAC3, ACC_EAC3},
    {AV_CODEC_ID_DTS, ACC_DTS},
    {AV_CODEC_ID_MP3, ACC_MP3},
    {AV_CODEC_ID_MP2, ACC_MP2},
    {AV_CODEC_ID_OPUS, ACC_Opus},
    {AV_CODEC_ID_VORBIS, ACC_Vorbis},
    {AV_CODEC_ID_FLAC, ACC_FLAC},
    {AV_CODEC_ID_TRUEHD, ACC_TrueHD},
    {AV_CODEC_ID_ADPCM_IMA_WAV, ACC_ADPCM_IMA},
    {AV_CODEC_ID_ADPCM_MS, ACC_ADPCM_MS},
    {AV_CODEC_ID_PCM_ALAW, ACC_ALaw},
    {AV_CODEC_ID_PCM_MULAW, ACC_MuLaw},
    {AV_CODEC_ID_PCM_DVD, ACC_PCM_DVD},
    {AV_CODEC_ID_SUBRIP, DMXS_Text},
    {AV_CODEC_ID_TEXT, DMXS_Text},
    {AV_CODEC_ID_ASS, DMXS_ASS},
    {AV_CODEC_ID_SSA, DMXS_ASS},
    {AV_CODEC_ID_MOV_TEXT, DMXS_MovText},
    {AV_CODEC_ID_HDMV_PGS_SUBTITLE, DMXS_PGS},
    {AV_CODEC_ID_DVD_SUBTITLE, DMXS_VobSub},
    {AV_CODEC_ID_DVB_SUBTITLE, DMXS_DVB},
};

/* An uncompressed audio codec and how its samples are laid out */
struct pcm_layout
{
	enum AVCodecID id; /* FFmpeg's */
	ULONG flags;       /* ACPCM_* */
};

static const struct pcm_layout pcm_layouts[] = {
    {AV_CODEC_ID_PCM_S16LE, 0},
    {AV_CODEC_ID_PCM_S16BE, ACPCM_BigEndian},
    {AV_CODEC_ID_PCM_S24LE, 0},
    {AV_CODEC_ID_PCM_S24BE, ACPCM_BigEndian},
    {AV_CODEC_ID_PCM_S32LE, 0},
    {AV_CODEC_ID_PCM_S32BE, ACPCM_BigEndian},
    {AV_CODEC_ID_PCM_U8, ACPCM_Unsigned},
    {AV_CODEC_ID_PCM_F32LE, ACPCM_Float},
    {AV_CODEC_ID_PCM_F32BE, ACPCM_Float | ACPCM_BigEndian},
};

/**
 * codec_kind() - What a stream is.
 * @type: FFmpeg's media type
 *
 * Return: DMXK_*.
 */
ULONG codec_kind(enum AVMediaType type)
{
	switch (type)
	{
	case AVMEDIA_TYPE_VIDEO:
		return DMXK_Video;
	case AVMEDIA_TYPE_AUDIO:
		return DMXK_Audio;
	case AVMEDIA_TYPE_SUBTITLE:
		return DMXK_Subtitle;
	default:
		return DMXK_Other;
	}
}

/**
 * pcm_layout_of() - How an uncompressed audio codec lays its samples out.
 * @id: FFmpeg's codec
 *
 * Return: its entry, NULL for a codec that is not one of them.
 */
static const struct pcm_layout *pcm_layout_of(enum AVCodecID id)
{
	for (ULONG i = 0; i < ARRAY_COUNT(pcm_layouts); i++)
		if (pcm_layouts[i].id == id)
			return &pcm_layouts[i];
	return NULL;
}

/**
 * codec_number() - The number a stream's codec has in the public headers.
 * @parameters: the stream's
 *
 * Return: VCC_*, ACC_* or DMXS_* by the stream's kind; 0 for a codec that has none.
 */
ULONG codec_number(const AVCodecParameters *parameters)
{
	for (ULONG i = 0; i < ARRAY_COUNT(codec_names); i++)
		if (codec_names[i].id == parameters->codec_id)
			return codec_names[i].number;
	return pcm_layout_of(parameters->codec_id) != NULL ? ACC_PCM : 0;
}

/**
 * codec_pcm_flags() - The ACPCM_* of an uncompressed audio codec's samples.
 * @id: FFmpeg's codec
 *
 * Return: the flags, 0 for any other codec.
 */
ULONG codec_pcm_flags(enum AVCodecID id)
{
	const struct pcm_layout *layout = pcm_layout_of(id);
	return layout != NULL ? layout->flags : 0;
}

/**
 * codec_tag() - A container's FourCC as VC_FOURCC() writes one.
 * @tag: FFmpeg's, the first character in the low byte
 *
 * Return: the FourCC, 0 for none.
 */
ULONG codec_tag(uint32_t tag)
{
	return tag != 0 ? VC_FOURCC(tag & 0xff, (tag >> 8) & 0xff, (tag >> 16) & 0xff, (tag >> 24) & 0xff) : 0;
}

/**
 * codec_color_space() - The colour space a container names for a video stream.
 * @parameters: the stream's
 *
 * Return: VCCS_*, VCCS_Unknown for a matrix the container does not name or this API has no name for.
 */
ULONG codec_color_space(const AVCodecParameters *parameters)
{
	BOOL full = parameters->color_range == AVCOL_RANGE_JPEG;
	switch (parameters->color_space)
	{
	case AVCOL_SPC_BT709:
		return full ? VCCS_BT709_Full : VCCS_BT709;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		return full ? VCCS_JPEG : VCCS_BT601;
	default:
		return VCCS_Unknown;
	}
}

/**
 * codec_filter() - The bitstream filter that makes a stream's packets what the decoders take.
 * @parameters: the stream's
 *
 * H.264 and H.265 held as length-prefixed NAL units (an avcC or hvcC record) become an
 * Annex-B byte stream.  MPEG-4 Part 2 has its packed B-pictures (DivX's and Xvid's way in
 * AVI and in Matroska's VfW mode: a P- and a B-picture in one packet, a placeholder in the
 * next) put each in a packet of its own, so that every packet is one picture, the B-picture
 * with the placeholder's time (demuxer_order.c puts the pair's times in order); a stream
 * without them passes as it is.
 *
 * Return: its name; NULL for a stream that needs none.
 */
const char *codec_filter(const AVCodecParameters *parameters)
{
	if (parameters->codec_id == AV_CODEC_ID_MPEG4)
		return "mpeg4_unpack_bframes";
	if (parameters->extradata_size == 0 || parameters->extradata[0] != RECORD_VERSION)
		return NULL;
	if (parameters->codec_id == AV_CODEC_ID_H264)
		return "h264_mp4toannexb";
	if (parameters->codec_id == AV_CODEC_ID_HEVC)
		return "hevc_mp4toannexb";
	return NULL;
}
