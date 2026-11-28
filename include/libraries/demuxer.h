/* SPDX-License-Identifier: MPL-2.0 */
/*
 * libraries/demuxer.h - demuxer.library: a container file's streams and packets.
 *
 * The library opens a file (Matroska, MP4/MOV, MPEG-TS, MPEG-PS, AVI), says
 * what streams it holds and hands out their packets in file order, each
 * stream's in the order it is decoded.  It decodes nothing: video packets go
 * to videocodec.library, audio packets to audiocodec.library, subtitle packets
 * to whoever draws them.
 *
 *   source = DMX_OpenTags(DMXA_FileName, name, DMXA_ErrorPtr, &error, TAG_DONE);
 *   DMX_GetInfo(source, &info);                 // info.Size set first, as in every structure here
 *   DMX_GetStream(source, n, &stream);          // n below info.Streams
 *   DMX_SelectStreams(source, mask);            // the streams whose packets are wanted
 *   while (DMX_ReadPacket(source, &packet) == 0)
 *   {
 *       ...                                     // packet.Stream says whose it is
 *       DMX_ReleasePacket(source, &packet);
 *   }
 *   DMX_Close(source);
 *
 * The rules
 *
 * - A caller is a Process: the library reads through dos.library in its
 *   context.  One source belongs to one task at a time.
 * - Every structure starts with Size, which the caller sets to its sizeof().
 *   The library fills no more than that and reads nothing past it.
 * - Times are VCTime, microseconds, as videocodec.library's: a packet's time
 *   goes to a decoder as it is.  VC_TIME_UNKNOWN where the file does not say.
 * - A packet's Data belongs to the library until DMX_ReleasePacket(); several
 *   may be held at once.  A stream's Extradata lives as long as the source.
 * - H.264 and H.265 packets are Annex-B byte streams whatever the file holds,
 *   and the stream's Extradata is its parameter sets the same way: what
 *   videocodec.library takes.
 * - Every video packet holds whole pictures: MPEG-4 Part 2's packed
 *   B-pictures come unpacked, a picture a packet, and MPEG-1 and MPEG-2 in
 *   transport and programme streams are cut into pictures.
 * - Every video packet's Pts is when its picture is shown, also where the
 *   file says it only for some pictures (MPEG streams) or gives the decode
 *   order's (AVI's frame numbers): MPEG-1, MPEG-2 and MPEG-4 Part 2 packets
 *   then come a few at a time, a reference picture held until the next is
 *   read, later than the file has them beside the other streams'.
 * - Calls return 0 or a DMXERR_*.
 */
#ifndef LIBRARIES_DEMUXER_H
#define LIBRARIES_DEMUXER_H

#include <exec/types.h>
#include <utility/tagitem.h>
#include <libraries/videocodec.h>
#include <libraries/audiocodec.h>

struct DMXSource; /* an open file; the library's own */

/* Errors */
#define DMXERR_NOMEM (-1)    /* out of memory */
#define DMXERR_FILE (-2)     /* the file did not open or could not be read; IoErr() says why */
#define DMXERR_FORMAT (-3)   /* not a container this library reads */
#define DMXERR_BADPARAM (-4) /* a tag or argument was rejected */
#define DMXERR_END (-5)      /* DMX_ReadPacket(): the file's end */
#define DMXERR_SEEK (-6)     /* DMX_Seek(): the file or its handler cannot go there */
#define DMXERR_INTERNAL (-7) /* the reader gave up inside; the source is good for DMX_Close() only */

/* DMX_OpenTagList() */
#define DMXA_Base (TAG_USER | 0x444D0000UL)
#define DMXA_FileName (DMXA_Base + 1) /* STRPTR: the file, required */
#define DMXA_ErrorPtr (DMXA_Base + 2) /* LONG *: gets the DMXERR_* when the open fails */

/* What a stream is (DMXStream.Kind) */
#define DMXK_Video 1
#define DMXK_Audio 2
#define DMXK_Subtitle 3
#define DMXK_Other 4 /* attachments, data: listed, never selected by default */

/* Subtitle codecs (DMXStream.Codec of a subtitle stream) */
#define DMXS_None 0
#define DMXS_Text 1    /* plain UTF-8 text a packet (SubRip in Matroska) */
#define DMXS_ASS 2     /* ASS/SSA: the script's head as Extradata, an event line a packet */
#define DMXS_MovText 3 /* MP4 timed text (tx3g) */
#define DMXS_PGS 4     /* Blu-ray bitmap subtitles */
#define DMXS_VobSub 5  /* DVD bitmap subtitles; the palette in Extradata */
#define DMXS_DVB 6     /* DVB bitmap subtitles */

/* DMXStream.Flags */
#define DMXSF_Default (1UL << 0) /* the file marks it as the one to play */
#define DMXSF_Forced (1UL << 1)  /* subtitles for foreign speech only */

#define DMX_NAME_SIZE 32      /* a format's name, with its NUL */
#define DMX_LANGUAGE_SIZE 4   /* an ISO 639-2 code, with its NUL */
#define DMX_TITLE_SIZE 64     /* a stream's or chapter's title, UTF-8, cut to fit, with its NUL */

/* The file as a whole (DMX_GetInfo()) */
struct DMXInfo
{
	ULONG Size;                 /* of this structure */
	char Format[DMX_NAME_SIZE]; /* the container's name, FFmpeg's: "matroska,webm", "mpegts", "mpeg", "avi" */
	ULONG Streams;              /* how many streams DMX_GetStream() describes */
	ULONG Chapters;             /* how many chapters DMX_GetChapter() describes */
	VCTime Duration;            /* the whole length, VC_TIME_UNKNOWN when the file does not say */
	ULONG FileSizeHigh;         /* the file's bytes, upper 32 bits */
	ULONG FileSizeLow;          /* and lower */
	BOOL Seekable;              /* DMX_Seek() can work: the handler can move in this file */
};

/* One stream (DMX_GetStream()) */
struct DMXStream
{
	ULONG Size;                       /* of this structure */
	ULONG Kind;                       /* DMXK_* */
	ULONG Codec;                      /* VCC_* (video), ACC_* (audio) or DMXS_* (subtitles); 0 for one no decoder here is named for */
	ULONG Tag;                        /* the container's FourCC for the codec (VC_FOURCC()), 0 for none */
	ULONG Flags;                      /* DMXSF_* */
	char Language[DMX_LANGUAGE_SIZE]; /* empty when the file does not say */
	char Title[DMX_TITLE_SIZE];       /* empty when the file does not say */
	CONST_APTR Extradata;             /* the codec's configuration, NULL for none */
	ULONG ExtradataSize;              /* its bytes */
	VCTime Duration;                  /* the stream's length, VC_TIME_UNKNOWN when the file does not say */
	/* video */
	ULONG Width;                 /* the picture's pixels */
	ULONG Height;                /* and rows */
	struct VCRational Par;       /* pixel aspect ratio, 0/0 when the file does not say */
	struct VCRational FrameRate; /* pictures a second, Num 0 when the file does not say */
	ULONG ColorSpace;            /* VCCS_* the container names, VCCS_Unknown when it names none (the stream may) */
	/* audio */
	ULONG SampleRate; /* samples a second */
	ULONG Channels;   /* how many */
	ULONG Bits;       /* ACC_PCM: bits a sample; else what the file says, 0 for nothing */
	ULONG PcmFlags;   /* ACC_PCM: ACPCM_*, how the samples are laid out */
	ULONG BlockAlign; /* the bytes of one coded block (ACA_BlockAlign), 0 when the file does not say */
};

/* DMXPacket.Flags */
#define DMXPF_KeyFrame (1UL << 0) /* decoding can start here */
#define DMXPF_Corrupt (1UL << 1)  /* the file's reader found damage in these bytes */

/* One packet (DMX_ReadPacket()) */
struct DMXPacket
{
	ULONG Size;      /* of this structure */
	APTR Data;       /* the bytes, the library's until DMX_ReleasePacket() */
	ULONG Length;    /* how many */
	ULONG Stream;    /* the stream's number */
	ULONG Flags;     /* DMXPF_* */
	VCTime Pts;      /* when it is shown or heard */
	VCTime Dts;      /* when it is decoded: no later than Pts, the order packets come in */
	VCTime Duration; /* how long it lasts, VC_TIME_UNKNOWN when the file does not say */
	APTR Private;    /* the library's */
};

/* DMX_Seek() */
#define DMXSEEK_Before 0 /* to the key frame at or before the time: playing from there reaches it; in a programme stream,
                            which has no index, its video from the next I-picture after where its times led, up to a
                            group of pictures late */
#define DMXSEEK_After 1  /* to the first key frame at or after it: stepping forward */

/* One chapter (DMX_GetChapter()) */
struct DMXChapter
{
	ULONG Size;                 /* of this structure */
	VCTime Start;               /* where it begins */
	VCTime End;                 /* where it ends */
	char Title[DMX_TITLE_SIZE]; /* empty when the file does not say */
};

#endif /* LIBRARIES_DEMUXER_H */
