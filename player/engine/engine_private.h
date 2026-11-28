/* SPDX-License-Identifier: MPL-2.0 */
/*
 * engine_private.h - the engine's own structures.
 *
 *   engine.c         the front's calls, the engine's task, its commands and its loop
 *   engine_file.c    the file: opened, its streams chosen, seeking
 *   engine_reader.c  the reader, the process that reads the file's packets ahead into the queues
 *   engine_video.c   the video decoder: packets in, pictures out and shown by the clock
 *   engine_audio.c   the audio decoder: packets in, samples to the audio output
 *   engine_stats.c   the statistics: counted, summed up a second, written as text
 *
 * Everything but the front's calls in engine.c and the reader's own part of
 * engine_reader.c runs in the engine's task.
 */
#ifndef KINORA_ENGINE_PRIVATE_H
#define KINORA_ENGINE_PRIVATE_H

#include <exec/ports.h>
#include <dos/dosextens.h>
#include <devices/timer.h>
#include <libraries/demuxer.h>
#include <libraries/audiocodec.h>

#include "engine.h"
#include "clock.h"
#include "queue.h"
#include "../audio/audio_out.h"
#include "../subtitles/subtitles.h"
#include "../display/display.h"

#define FREE_PACKETS 32        /* the decoder's input packets the engine can hold empty */
#define READY_FRAMES 32        /* the decoded pictures that can wait for their time */
#define NO_STREAM (-1L)        /* video_index, audio_index: the file has none to play */
#define SUBTITLE_NAME_SIZE 512 /* a subtitle file's path, with its NUL */
#define SOURCE_STREAMS 32u     /* the streams DMX_SelectStreams() can name */
#define DONE_PACKETS 256       /* packets the engine is done with that can wait for the reader to release them */

/* What the front asks for */
enum engine_command
{
	COMMAND_OPEN,
	COMMAND_CLOSE,
	COMMAND_PLAY,
	COMMAND_SEEK,
	COMMAND_VOLUME,
	COMMAND_PLACE,
	COMMAND_STATUS,
	COMMAND_TRACKS,
	COMMAND_SELECT,
	COMMAND_UNIT,
	COMMAND_SUBTITLE_FILE,
	COMMAND_SUBTITLE_FONT,
	COMMAND_SUBTITLE_LOOK,
	COMMAND_CHAPTERS,
	COMMAND_DELAYS,
	COMMAND_ASPECT,
	COMMAND_STATISTICS,
	COMMAND_END, /* the last: the commands before it index the engine's table */
};

/* What a command carries: the member named for it */
union engine_arguments
{
	struct engine *engine; /* the start message: the engine the task serves */
	struct
	{
		CONST_STRPTR name;                 /* the file */
		const struct display_place *place; /* the box for its pictures */
	} open; /* COMMAND_OPEN */
	BOOL play;                         /* COMMAND_PLAY: run, else stand still */
	VCTime seek_time;                  /* COMMAND_SEEK: where to */
	ULONG volume;                      /* COMMAND_VOLUME: 0 to AUDIO_VOLUME_FULL */
	const struct display_place *place; /* COMMAND_PLACE: the new box, NULL for none */
	struct engine_status *status;      /* COMMAND_STATUS: filled in */
	struct
	{
		struct engine_track *list; /* filled in */
		ULONG room;                /* how many it holds */
	} tracks; /* COMMAND_TRACKS */
	struct
	{
		enum engine_track_kind kind; /* audio or subtitle */
		LONG track;                  /* the track's stream, ENGINE_TRACK_OFF or ENGINE_TRACK_FILE */
	} select; /* COMMAND_SELECT */
	struct
	{
		ULONG number; /* the ahi.device unit */
		BOOL shared;  /* share it with other programs */
	} unit; /* COMMAND_UNIT */
	CONST_STRPTR subtitle_file; /* COMMAND_SUBTITLE_FILE: the SubRip file */
	CONST_STRPTR subtitle_font; /* COMMAND_SUBTITLE_FONT: the font file */
	struct
	{
		ULONG size;     /* the font's size in percent of the normal one */
		ULONG distance; /* the gap from the frame's edge, in percent of its rows */
	} subtitle_look; /* COMMAND_SUBTITLE_LOOK */
	struct
	{
		struct engine_chapter *list; /* filled in */
		ULONG room;                  /* how many it holds */
	} chapters; /* COMMAND_CHAPTERS */
	struct
	{
		VCTime audio;     /* the sound behind the picture by this much */
		VCTime subtitles; /* the subtitles behind it by this much */
	} delays; /* COMMAND_DELAYS */
	struct VCRational aspect; /* COMMAND_ASPECT: the shape to show the picture in */
	struct
	{
		BOOL panel; /* draw the panel into the pictures */
		char *text; /* gets the statistics as text; NULL for none */
		ULONG room; /* its bytes */
	} statistics; /* COMMAND_STATISTICS */
};

/* One command and its answer */
struct engine_message
{
	struct Message message;      /* Exec's part */
	enum engine_command command; /* what to do */
	LONG result;                 /* the answer: 0 or ENGINE_ERR_*; COMMAND_TRACKS, COMMAND_CHAPTERS: how many were listed */
	union engine_arguments with; /* what the command carries */
};

/* What the engine counts while it plays */
struct statistics
{
	/* The second being counted */
	VCTime second_start;    /* its E-clock time */
	ULONG frames;           /* pictures shown in it */
	VCTime behind_sum;      /* how far behind their times they were shown, together */
	ULONG behind_measured;  /* the pictures it was measured for */
	VCTime behind_most;     /* the most one was behind */
	ULONG video_bytes;      /* bytes of video packets read in it */
	ULONG audio_bytes;      /* and of audio packets */

	/* The second before */
	ULONG last_rate;        /* pictures shown a second, in hundredths (RATE_HUNDREDTHS) */
	LONG last_behind_mean;  /* milliseconds they were behind on average */
	LONG last_behind_most;  /* and at most */
	ULONG last_video_kbps;  /* the video read, in kbit/s */
	ULONG last_audio_kbps;  /* the audio read */

	/* The last picture shown */
	struct VCRect picture_rect; /* where in its frame it was */
	ULONG frame_width;          /* that frame's pixels */
	ULONG frame_height;         /* and rows */

	/* The last move */
	VCTime seek_target;    /* the time moved to, VC_TIME_UNKNOWN before any move */
	VCTime seek_video;     /* the time of the first video packet read after it, VC_TIME_UNKNOWN before one */
	VCTime seek_audio;     /* and of the first audio packet */

	/* The panel */
	BOOL panel_on;                   /* it is drawn into the pictures */
	char text[STATISTICS_TEXT_SIZE]; /* its lines */
	ULONG text_version;              /* counts the text's changes */
};

/* What the reader counted since the engine last took it */
struct reads
{
	ULONG video_bytes;  /* bytes of video packets read */
	ULONG audio_bytes;  /* and of audio packets */
	VCTime video_first; /* the first video packet's time since the file was opened or moved in, VC_TIME_UNKNOWN before one */
	VCTime audio_first; /* and the first audio packet's */
};

/* The engine */
/* What the video decoder is asked to leave out (VCA_Skip) while pictures come too late, and since when (architecture 6.3) */
struct video_skip
{
	ULONG level;         /* VCSK_*, the one the decoder has; ULONG as VCA_Skip carries it (videocodec.h defines, not an enum) */
	VCTime changed;      /* clock_wall() at the last step up or back */
	VCTime lowered;      /* clock_wall() at the last step back; 0 when the last step was up, or the step back has held */
	VCTime hold;         /* how long without a dropped picture before a step back */
	VCTime last_drop;    /* clock_wall() at the last dropped picture that counted */
	VCTime calm_until;   /* clock_wall() until which drops do not count: the engine upset the pictures' flow itself */
	ULONG drops;         /* drops that counted since drops_since */
	VCTime drops_since;  /* clock_wall() where that count began */
	VCTime key_last;     /* the time of the last key frame sent to the decoder, VC_TIME_UNKNOWN after an open or a move */
	VCTime key_gap_most; /* the longest gap between two key frames seen in the film, 0 before two were seen */
};

struct engine
{
	/* The task */
	struct engine_setup setup;   /* what the front asked for */
	struct Process *process;     /* the engine's task */
	struct MsgPort *reply_port;  /* the front's: where its commands come back */
	struct MsgPort *port;        /* the engine's: where the commands go; not the process's own port, which is dos.library's */
	struct engine_message start; /* the message that starts the task; its answer is the task's readiness */

	/* The timer that wakes the task for the next picture */
	struct MsgPort *timer_port; /* where its request comes back */
	struct timerequest *timer;  /* the request */
	BOOL timer_open;            /* timer.device is open on it */
	BOOL timer_sent;            /* the request is out */
	VCTime wake_time;           /* the E-clock time it is set for */

	/* The playing */
	enum engine_state state;  /* what the engine is doing */
	LONG error;               /* what stopped the playing */
	struct clock clock;       /* the film's time */
	ULONG volume;             /* the level, kept across files */
	VCTime audio_delay;       /* the sound behind the picture by this much, kept across files */
	VCTime subtitle_delay;    /* the subtitles behind it by this much */
	struct VCRational aspect; /* the shape to show the picture in; Num 0 for the stream's own */

	/* The file */
	struct DMXSource *source; /* the file */
	struct DMXInfo file_info; /* what it is */
	volatile BOOL file_end;   /* its last packet was read: the reader's, under queue_lock */

	/* The video */
	LONG video_index;                            /* the stream decoded as video, NO_STREAM for none */
	struct DMXStream video_stream;               /* that stream */
	struct queue video_queue;                    /* its packets read ahead */
	struct VCSession *video_decoder;             /* the decoder, NULL for none */
	char video_backend[VC_BACKEND_NAME_SIZE];    /* the backend videocodec.library gave it (VCA_BackendName) */
	char video_converter[VC_BACKEND_NAME_SIZE];  /* the backend of the converter behind it, empty for none (VCA_ConverterName) */
	BYTE video_signal;                           /* the signal it wakes the engine with */
	struct VCPacket *free_packets[FREE_PACKETS]; /* its input packets waiting to be filled */
	ULONG free_packet_count;                     /* how many */
	ULONG head_sent_bytes;                       /* of the queue's first packet, the bytes the decoder has */
	struct VCFrame *ready_frames[READY_FRAMES];  /* decoded pictures waiting for their time, a ring */
	ULONG ready_head;                            /* the next to show */
	ULONG ready_count;                           /* how many wait */
	BOOL video_end_sent;                         /* the decoder was told the stream's end */
	BOOL video_done;                             /* and gave its last picture */
	struct VCStreamInfo video_shape;             /* the stream's shape as the decoder told it */
	BOOL decoder_sized;                          /* the decoder knows the size to scale to */
	struct display *display;                     /* where pictures go, NULL for nowhere */
	BOOL first_shown;                            /* a picture was shown since the file was opened or moved in */
	BOOL catching_up;                            /* since a move no picture came in time yet: late ones are not counted as dropped */
	struct video_skip skip;                      /* what the decoder is asked to leave out while it falls behind (architecture 6.3) */
	ULONG frames_shown;                          /* pictures shown */
	ULONG frames_dropped;                        /* pictures too late to show */

	/* The audio */
	LONG audio_index;                  /* the stream decoded as audio, NO_STREAM for none */
	struct DMXStream audio_stream;     /* that stream */
	struct queue audio_queue;          /* its packets read ahead */
	struct ACSession *audio_decoder;   /* the decoder, NULL for none */
	char audio_backend[AC_BACKEND_NAME_SIZE]; /* the backend audiocodec.library gave it (ACA_BackendName) */
	struct audio_out *audio_out;       /* the sound output, opened at the first decoded samples */
	BYTE audio_signal;                 /* the signal the output wakes the engine with */
	BOOL audio_32bit;                  /* ahi.device takes 32-bit samples, so the decoder gives them (audio_open()) */
	BOOL audio_surround;               /* the output plays 7.1: the unit's mode is a 7.1 one (audio_open()) */
	struct ACFrame pending_samples;    /* decoded samples not yet all in the output's ring */
	BOOL pending_held;                 /* pending_samples holds samples */
	ULONG pending_written;             /* how many of its sample frames are in the ring */
	struct audio_placement placement;  /* 7.1: where AHI's channels are in the pending samples, for the layout last seen */
	BOOL audio_end_sent;               /* the decoder was told the stream's end */
	BOOL audio_done;                   /* and gave its last samples */
	BOOL sound_waits;                  /* since the open or a move the sound waits for the first picture's time (audio_feed()) */

	/* The subtitles */
	struct subtitles *subtitles;            /* the cues of the stream or file chosen */
	LONG subtitle_index;                    /* the stream shown, ENGINE_TRACK_OFF or ENGINE_TRACK_FILE */
	ULONG subtitle_codec;                   /* DMXS_* of that stream */
	char subtitle_name[SUBTITLE_NAME_SIZE]; /* the SubRip file, empty for none */

	/* The reader: the process that reads the file ahead (engine_reader.c) */
	struct Process *reader;                      /* the process, NULL while none runs */
	struct SignalSemaphore file_lock;            /* held for every DMX_ call on the file: a read by the reader, the rest by the engine */
	struct SignalSemaphore queue_lock;           /* guards the queues, file_end, done_packets and reads */
	BYTE reader_signal;                          /* the engine's signal the reader wakes it with: packets read, the file's end, gone */
	volatile BOOL reader_gone;                   /* the reader has ended */
	struct queue subtitle_queue;                 /* subtitle packets read, for the engine to make cues of */
	struct DMXPacket done_packets[DONE_PACKETS]; /* packets the engine is done with, for the reader to release */
	ULONG done_count;                            /* how many */
	struct reads reads;                          /* what the reader counted */

	struct statistics statistics; /* what is counted */
};

/* engine.c */
void engine_wake_in(struct engine *engine, VCTime delay);
void engine_clock_start(struct engine *engine);

/* engine_file.c */
LONG file_open(struct engine *engine, CONST_STRPTR name);
void file_close(struct engine *engine);
ULONG file_tracks(struct engine *engine, struct engine_track *tracks, ULONG room);
LONG file_select(struct engine *engine, enum engine_track_kind kind, LONG track);
LONG file_load_subtitles(struct engine *engine, CONST_STRPTR name);
ULONG file_chapters(struct engine *engine, struct engine_chapter *chapters, ULONG room);
LONG file_seek(struct engine *engine, VCTime time);

/* engine_reader.c */
BOOL reader_start(struct engine *engine);
void reader_end(struct engine *engine);
void reader_hold(struct engine *engine);
void reader_let_go(struct engine *engine);
const struct DMXPacket *reader_next(struct engine *engine, struct queue *queue, BOOL *ended);
void reader_take(struct engine *engine, struct queue *queue);
void reader_flush(struct engine *engine, struct queue *queue);
void reader_subtitles(struct engine *engine);
void reader_reads_take(struct engine *engine, struct reads *reads);
void reader_reads_reset(struct engine *engine);

/* engine_video.c */
LONG video_open(struct engine *engine);
void video_close(struct engine *engine);
void video_pump(struct engine *engine);
void video_flush(struct engine *engine);
void video_place(struct engine *engine, const struct display_place *place);
void video_calm(struct engine *engine);
void video_attach(struct engine *engine);

/* engine_audio.c */
void audio_open(struct engine *engine);
void audio_close(struct engine *engine);
void audio_pump(struct engine *engine);
void audio_flush(struct engine *engine);

/* engine_stats.c */
void stats_reset(struct engine *engine);
void stats_count_seek(struct engine *engine, VCTime time);
void stats_count_frame(struct engine *engine, const struct VCFrame *frame);
void stats_write(struct engine *engine, char *to, ULONG room, BOOL inside);
void stats_tick(struct engine *engine);

#endif /* KINORA_ENGINE_PRIVATE_H */
