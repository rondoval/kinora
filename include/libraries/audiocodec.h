/* SPDX-License-Identifier: MPL-2.0 */
/*
 * libraries/audiocodec.h - audiocodec.library: audio decoders behind one API.
 *
 * The library turns a compressed audio stream's packets into PCM.  The
 * decoders themselves are backends, LIBS:audiocodec/<name>.codec
 * (libraries/audiocodec_backend.h); the library finds them and picks one for
 * each session.
 *
 *   session = AC_OpenSessionTags(ACA_Codec, ACC_AC3, ACA_SampleRate, 48000, ACA_Channels, 6,
 *                                ACA_OutputChannels, 2, ACA_ErrorPtr, &error, TAG_DONE);
 *   packet.Size = sizeof(packet);               // as in every structure here
 *   ...
 *   if (AC_SendPacket(session, &packet) == 0)   // one packet of the stream
 *       while (AC_ReceiveFrame(session, &frame) == 0)
 *           ...                                 // frame.Samples sample frames at frame.Data
 *   AC_CloseSession(session);
 *
 * The rules
 *
 * - Decoding runs in the caller's task, inside the calls: nothing is signalled
 *   and nothing arrives later.  One session belongs to one task at a time.
 * - Every structure starts with Size, which the caller sets to its sizeof().
 *   The library fills no more than that and reads nothing past it.
 * - Times are VCTime, microseconds, as videocodec.library's.
 * - AC_SendPacket() copies what it needs: the packet's bytes are the caller's
 *   again when it returns.
 * - After each AC_SendPacket(), call AC_ReceiveFrame() until it answers
 *   ACERR_AGAIN: a packet holds no, one or several frames.  A frame's Data
 *   is the session's and good until the next call on the session.
 * - At the stream's end send a packet of Length 0; AC_ReceiveFrame() then
 *   gives what the decoder still holds and ends with ACERR_END.  AC_Flush()
 *   makes the session ready for packets again, as after a seek.
 * - Samples are signed, interleaved, in the machine's byte order.  The
 *   channels of a sample frame come in the order of their ACCH_* bits, lowest
 *   first; ACFrame.Layout says which they are.
 * - The rate is the stream's: nothing is resampled.
 * - Calls return 0 or an ACERR_*.
 */
#ifndef LIBRARIES_AUDIOCODEC_H
#define LIBRARIES_AUDIOCODEC_H

#include <exec/types.h>
#include <utility/tagitem.h>
#include <libraries/videocodec.h>

struct ACSession; /* an open decoder; the library's own */

#define AUDIOCODEC_NAME "audiocodec.library"
#define AUDIOCODEC_BACKEND_DRAWER "audiocodec" /* in LIBS: */
#define AUDIOCODEC_BACKEND_SUFFIX ".codec"

/* Audio codecs (ACA_Codec; DMXStream.Codec of an audio stream) */
#define ACC_None 0
#define ACC_AAC 1      /* AAC: LC, HE and HE v2; raw frames, the AudioSpecificConfig as the stream's configuration */
#define ACC_AAC_LATM 2 /* AAC in LATM, as transport streams carry it */
#define ACC_AC3 3      /* AC-3 (Dolby Digital) */
#define ACC_EAC3 4     /* E-AC-3 (Dolby Digital Plus) */
#define ACC_DTS 5      /* DTS and its extensions, as far as the decoder reads them; mixed down (ACA_OutputChannels), the core alone */
#define ACC_MP3 6      /* MPEG audio layer 3 */
#define ACC_MP2 7      /* MPEG audio layer 2 */
#define ACC_Opus 8     /* Opus; the OpusHead as the stream's configuration */
#define ACC_Vorbis 9   /* Vorbis; its three headers as the stream's configuration */
#define ACC_FLAC 10    /* FLAC; the STREAMINFO as the stream's configuration */
#define ACC_TrueHD 11  /* Dolby TrueHD */
#define ACC_PCM 12     /* uncompressed; ACA_Bits and ACA_PcmFlags say how the samples are laid out */
#define ACC_ADPCM_IMA 13 /* IMA ADPCM as WAVE and AVI hold it; ACA_BlockAlign and the stream's configuration needed */
#define ACC_ADPCM_MS 14  /* Microsoft ADPCM; ACA_BlockAlign and the stream's configuration (its coefficients) needed */
#define ACC_ALaw 15      /* G.711 A-law, 8 bits a sample */
#define ACC_MuLaw 16     /* G.711 mu-law, 8 bits a sample */
#define ACC_PCM_DVD 17   /* a DVD's LPCM, 16, 20 or 24 bits: each packet says its rate and channels */

/* Errors */
#define ACERR_NOMEM (-1)       /* out of memory */
#define ACERR_BADPARAM (-2)    /* a tag or argument was rejected */
#define ACERR_UNSUPPORTED (-3) /* no backend decodes this codec, or not with this configuration */
#define ACERR_AGAIN (-4)       /* AC_ReceiveFrame(): send a packet first; AC_SendPacket(): receive the frames first */
#define ACERR_DATA (-5)        /* the packet could not be decoded; the session goes on with the next */
#define ACERR_END (-6)         /* AC_ReceiveFrame(): everything up to the stream's end was given */
#define ACERR_INTERNAL (-7)    /* the decoder gave up inside; the session is good for AC_CloseSession() only */

/* AC_OpenSessionTagList() */
#define ACA_Base (TAG_USER | 0x41430000UL)
#define ACA_Codec (ACA_Base + 1)          /* ULONG: ACC_*, required */
#define ACA_Extradata (ACA_Base + 2)      /* APTR: the codec's configuration from the container; copied */
#define ACA_ExtradataSize (ACA_Base + 3)  /* ULONG: its bytes */
#define ACA_SampleRate (ACA_Base + 4)     /* ULONG: as the container says; needed where the stream does not carry it */
#define ACA_Channels (ACA_Base + 5)       /* ULONG: as the container says; needed where the stream does not carry it */
#define ACA_Bits (ACA_Base + 6)           /* ULONG: ACC_PCM: bits a sample (8, 16, 24, 32) */
#define ACA_PcmFlags (ACA_Base + 7)       /* ULONG: ACC_PCM: ACPCM_* */
#define ACA_OutputFormat (ACA_Base + 8)   /* ULONG: ACF_*; ACF_S16 without the tag */
#define ACA_OutputChannels (ACA_Base + 9) /* ULONG: mix down to 1, 2 or 6 channels when the stream has more; 0 or no tag: as the stream */
#define ACA_ErrorPtr (ACA_Base + 10)      /* LONG *: gets the ACERR_* when the open fails */
#define ACA_Backend (ACA_Base + 11)       /* STRPTR: this backend and no other ("ffmpegaudio" or "ffmpegaudio.codec") */
#define ACA_BackendName (ACA_Base + 12)   /* char *, room for AC_BACKEND_NAME_SIZE: gets the file name of the backend the session went to, empty when refused */
#define ACA_BlockAlign (ACA_Base + 13)    /* ULONG: the bytes of one coded block, as the container says (DMXStream.BlockAlign); ADPCM needs it */

/* How uncompressed samples are laid out (ACA_PcmFlags, DMXStream.PcmFlags) */
#define ACPCM_BigEndian (1UL << 0) /* the samples' byte order */
#define ACPCM_Float (1UL << 1)     /* floating-point samples */
#define ACPCM_Unsigned (1UL << 2)  /* unsigned samples */

/* Sample formats a session gives (ACA_OutputFormat, ACFrame.Format) */
#define ACF_S16 1 /* 16 bits */
#define ACF_S32 2 /* 32 bits */

/*
 * Channels (ACFrame.Layout): a bit a channel.  A sample frame holds them in
 * this order, lowest bit first, which is the WAVE order.
 */
#define ACCH_FrontLeft (1UL << 0)
#define ACCH_FrontRight (1UL << 1)
#define ACCH_FrontCenter (1UL << 2)
#define ACCH_LFE (1UL << 3)
#define ACCH_BackLeft (1UL << 4)
#define ACCH_BackRight (1UL << 5)
#define ACCH_FrontLeftOfCenter (1UL << 6)
#define ACCH_FrontRightOfCenter (1UL << 7)
#define ACCH_BackCenter (1UL << 8)
#define ACCH_SideLeft (1UL << 9)
#define ACCH_SideRight (1UL << 10)

#define ACL_Mono ACCH_FrontCenter
#define ACL_Stereo (ACCH_FrontLeft | ACCH_FrontRight)
#define ACL_5_1 (ACL_Stereo | ACCH_FrontCenter | ACCH_LFE | ACCH_BackLeft | ACCH_BackRight)
#define ACL_7_1 (ACL_5_1 | ACCH_SideLeft | ACCH_SideRight)

/* One packet of the stream (AC_SendPacket()) */
struct ACPacket
{
	ULONG Size;   /* of this structure */
	APTR Data;    /* the bytes, as the container holds them */
	ULONG Length; /* how many; 0 is the stream's end */
	ULONG Flags;  /* none yet: 0 */
	VCTime Pts;   /* when its first sample is heard, VC_TIME_UNKNOWN when the file does not say */
};

/* Decoded samples (AC_ReceiveFrame()) */
struct ACFrame
{
	ULONG Size;       /* of this structure */
	APTR Data;        /* the samples, the session's until its next call */
	ULONG Samples;    /* sample frames: one sample of every channel each */
	ULONG SampleRate; /* sample frames a second */
	ULONG Channels;   /* samples in a sample frame */
	ULONG Layout;     /* ACCH_*: which channels they are */
	ULONG Format;     /* ACF_* */
	VCTime Pts;       /* when the first is heard; counted on from the last known time where a packet had none */
};

#define AC_BACKEND_NAME_SIZE 32 /* a backend's file name, with its NUL */
#define AC_CODEC_NAME_SIZE 32   /* a codec's name, with its NUL */

/* One row of the codec table (AC_QueryCodec()): a codec one backend decodes */
struct ACCodecInfo
{
	ULONG Size;                         /* of this structure */
	ULONG Codec;                        /* ACC_* */
	LONG Priority;                      /* of the backend's rows for one codec the highest gets the session */
	char Name[AC_CODEC_NAME_SIZE];      /* the codec, for people: "AC-3" */
	char Backend[AC_BACKEND_NAME_SIZE]; /* the backend's file name: "ffmpegaudio.codec" */
};

/* Whether a structure of the given Size reaches a field */
#define AC_HAS_FIELD(size, type, field) ((ULONG)(size) >= (ULONG) & ((type *)0)->field + (ULONG)sizeof(((type *)0)->field))

#endif /* LIBRARIES_AUDIOCODEC_H */
