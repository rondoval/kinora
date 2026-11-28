/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * codec_private.h - ffmpegvideo.codec's own structures (docs/architecture.md, section 14).
 *
 *   codec_lib.c       the library: its name, its function table, the calls' entry points
 *   codec_table.c     the codec rows, and FFmpeg's decoder for a session
 *   codec_session.c   a session: its objects, the client's calls, the items handed out
 *   codec_process.c   the session's process: packets into FFmpeg's decoder, pictures out
 *   codec_frames.c    the frames FFmpeg decodes into (get_buffer2), in sets of one shape
 *   codec_memory.c    their pictures' memory, within the VPU's reach
 *
 * A session's state is under its lock; FFmpeg is under FFmpeg's (amiga_library.h), which
 * only the session's process and the open and the close take.  The process may take the
 * session's lock while it holds FFmpeg's, never the other way round, so a client's call
 * never waits for a picture.
 */
#ifndef CODEC_PRIVATE_H
#define CODEC_PRIVATE_H

#include <exec/lists.h>
#include <exec/ports.h>
#include <exec/semaphores.h>
#include <exec/types.h>
#include <dos/dosextens.h>
#include <libraries/videocodec_backend.h>

#include <libavcodec/avcodec.h>
#include "codec_memory.h"

#define BACKEND_NAME "ffmpegvideo.codec"
#define FRAMES_LEAST 4u          /* a session's frames: FFmpeg's references and one more */
#define FRAMES_MOST 32u          /* what a chain imports at most (videocodec.library) */
#define FRAMES_DEFAULT 8u        /* FFmpeg's two or three, the converter's one or two, the rest to run ahead */
#define PACKETS_DEFAULT 8u       /* the library's own input packets */
#define PACKETS_MOST 64u
#define PACKET_BYTES_DEFAULT (256u << 10)
#define WORK_SIGNAL SIGBREAKF_CTRL_F /* the session's process: something to do (a packet, a free frame, a flush, the end) */

/* One of the session's input packets: the public part first, as the backend interface wants */
struct packet
{
	struct VCPacket pub;  /* what the client sees */
	struct MinNode node;  /* in the session's queue while sent */
	struct session *owner; /* the session it belongs to */
	BOOL sent;            /* with the session, not the client's */
};

/* One of the frames FFmpeg decodes into; in use while FFmpeg or the client holds it (codec_frames.c) */
struct frame
{
	struct VCFrame pub;      /* what the client sees */
	struct frame_set *set;   /* the set it belongs to */
	BOOL with_decoder;       /* FFmpeg holds it: its buffer has a reference left */
	BOOL with_client;        /* handed out (or waiting among the items to be), not yet given back */
};

/* The frames of one shape: FFmpeg's I420 as the VideoCore's converter imports it */
struct frame_set
{
	struct MinNode node;     /* in the session's sets: the current one at the head, retired ones behind */
	struct session *owner;   /* the session */
	struct frame *frames;    /* the frames */
	ULONG count;             /* how many: the session's frame_count when the set was made */
	APTR memory;             /* their pictures, one block from memory_alloc(): on a cache line, within the VPU's reach */
	ULONG width;             /* the picture FFmpeg asked for: its coded width */
	ULONG height;            /* and rows */
	ULONG pitch;             /* luma: bytes a row; the chroma's are half */
	ULONG rows;              /* luma: rows, at least as many as FFmpeg writes; the chroma's are half */
	ULONG picture_bytes;     /* one frame's three planes */
	BOOL retired;            /* a newer set is current: this one is disposed of once none of its frames is in use */
};

/* The message that starts a session's process: which session it serves.  On 3.x a process's entry takes no
   argument (NP_UserData is OS4's), so the process takes this from its pr_MsgPort first (codec_process.c) */
struct start_message
{
	struct Message message;  /* Exec's part; never replied: it lives in the session */
	struct session *session; /* the session the process serves */
};

/* What the session hands out, in order: a ring of items */
struct items
{
	struct VCItem *item;  /* the ring */
	ULONG room;           /* its length */
	ULONG head;           /* the next to hand out */
	ULONG count;          /* waiting */
};

/* A session's counts, for VCInfo's counters */
struct counts
{
	ULONG pictures;       /* handed out */
	ULONG damaged;        /* packets FFmpeg could not decode */
	ULONG waits;          /* the process waited for an unused frame */
};

/* An open decoder */
struct session
{
	/* Its place in the backend */
	struct MinNode listing;          /* in the backend's list of sessions (codec_session.c) */
	struct SignalSemaphore lock;     /* the fields below, but those fixed at the open and FFmpeg's part */

	/* Fixed at the open, read without the lock: what the client asked for, and what the codec's row allows */
	ULONG max_area;                  /* the row's largest picture, in pixels */
	ULONG frame_count;               /* frames a set */
	struct VCRational frame_rate;    /* the client's; Num 0 when it gave none */
	ULONG color_space;               /* the client's VCCS_*; VCCS_Unknown: the stream's */

	/* The decoder's process */
	struct Process *process;         /* the process, NULL once gone */
	char process_name[64];           /* its name: the client's, " decoder" behind */
	struct start_message start;      /* its start message */
	BOOL quit;                       /* it is to end */
	struct Task *closer;             /* the task closing the session, which the process signals as it goes */
	BYTE closer_signal;              /* that task's signal */

	/* The client */
	struct Task *sig_task;           /* its task to signal, or NULL */
	ULONG sig_mask;                  /* its signals; 0: it polls */
	ULONG skip;                      /* VCSK_*: what FFmpeg is to leave out (VCA_Skip), set into it before each picture and
	                                    read there without the lock; ULONG as the ABI carries it: VCSK_* are videocodec.h's
	                                    defines, not an enum, and come as a tag's value and in VCSessionParams.Skip */
	BOOL failed;                     /* VCEV_Error was queued: only the close is left */
	struct items items;              /* what it polls */
	struct counts counts;            /* for VCInfo's counters: written by the process alone, read whole without the lock */

	/* Packets in, pictures out */
	struct packet *packet;           /* the input packets */
	ULONG packet_count;              /* how many */
	struct MinList queue;            /* struct packet sent, in order */
	struct MinList sets;             /* struct frame_set, the current one first */
	BOOL flush_pending;              /* VCB_Flush(): the process flushes FFmpeg before its next packet */
	ULONG generation;                /* counts the flushes: a picture decoded before one is dropped */

	/* FFmpeg's part: the process's own, touched only with FFmpeg's lock held */
	AVCodecContext *decoder;         /* FFmpeg's decoder */
	AVCodecParserContext *parser;    /* for a session that is not framed and a codec that has one; else NULL */
	AVPacket *picture;               /* the picture being built from the client's pieces, then sent; empty (buf NULL) between */
	AVFrame *decoded;                /* what the decoder gives */
	ULONG decode_generation;         /* the flush generation of what is being decoded */
};

/* codec_table.c */
ULONG table_rows(void);
LONG table_row(ULONG index, struct VCCodecInfo *info);
const AVCodec *table_decoder(ULONG codec, ULONG tag);
ULONG table_max_area(ULONG codec);
BOOL table_parsed(ULONG codec);

/* codec_session.c: the client's calls; session_* take the session's lock */
void sessions_init(void);
struct session *session_open(const struct VCSessionParams *params, LONG *error);
void session_close(struct session *session);
LONG session_send(struct session *session, struct VCPacket *packet);
LONG session_poll(struct session *session, struct VCItem *item);
void session_release_frame(struct session *session, struct VCFrame *frame);
LONG session_flush(struct session *session);
LONG session_set_attrs(struct session *session, const struct VCSessionParams *params);
struct VCFrame *session_output_frame(struct session *session, ULONG index);
ULONG session_count(void);
void session_counts(struct counts *sum);
/* with the session's lock held */
void items_add(struct session *session, const struct VCItem *item);
void items_event(struct session *session, ULONG event, LONG status, const struct VCStreamInfo *stream);
void session_fail(struct session *session, LONG status);
void packet_return(struct session *session, struct packet *packet);
struct packet *packet_of_node(struct Node *node);

/* codec_process.c */
LONG process_start(struct session *session);
void process_stop(struct session *session);

/* codec_frames.c */
int frames_get_buffer(AVCodecContext *decoder, AVFrame *frame, int flags);
void frames_hand_out(struct session *session, const AVFrame *decoded);
void frames_release(struct session *session, struct frame *frame);
void frames_flush(struct session *session);
void frames_dispose_all(struct session *session);

#endif /* CODEC_PRIVATE_H */
