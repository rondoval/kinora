/* SPDX-License-Identifier: MPL-2.0 */
/*
 * engine_stats.c - the statistics: what the engine counts while it plays,
 * summed up once a second, and the lines of text that say it.
 *
 * The text is what the panel in the picture shows; the front's STATISTICS
 * command answers it with lines on the engine's inner workings behind.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>

#include "engine_private.h"

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define MICROSECONDS_PER_SECOND 1000000
#define MICROSECONDS_PER_MS 1000
#define SECONDS_PER_MINUTE 60
#define MINUTES_PER_HOUR 60
#define BITS_PER_BYTE 8
#define KILO 1000
#define BYTES_PER_KB 1024
#define MB_PER_HIGH_WORD 4096 /* megabytes a unit of a file size's upper 32 bits stands for */
#define BYTES_PER_MB_SHIFT 20
#define RATE_HUNDREDTHS 100 /* statistics.last_rate: pictures a second, in hundredths */
#define MICROSECONDS_PER_TENTH 100000
#define TENTHS_PER_SECOND 10

/* The text being written */
struct text
{
	char *chars; /* the buffer */
	ULONG room;  /* its bytes */
	ULONG used;  /* the characters in it */
};

static const char *const video_codec_names[] = {
    [VCC_H264]    = "H.264",
    [VCC_MJPEG]   = "MJPEG",
    [VCC_H265]    = "H.265",
    [VCC_MPEG1]   = "MPEG-1",
    [VCC_MPEG2]   = "MPEG-2",
    [VCC_MPEG4]   = "MPEG-4",
    [VCC_MSMPEG4] = "MS-MPEG4",
    [VCC_H263]    = "H.263",
    [VCC_Theora]  = "Theora",
};

/* What the decoder leaves out (VCA_Skip), for the pictures' line */
static const char *const skip_names[] = {
    [VCSK_None]         = "",
    [VCSK_NonReference] = "; skipping B-pictures",
    [VCSK_NonKey]       = "; key frames only",
};

static const char *const audio_codec_names[] = {
    [ACC_AAC]      = "AAC",
    [ACC_AAC_LATM] = "AAC (LATM)",
    [ACC_AC3]      = "AC-3",
    [ACC_EAC3]     = "E-AC-3",
    [ACC_DTS]      = "DTS",
    [ACC_MP3]      = "MP3",
    [ACC_MP2]      = "MP2",
    [ACC_Opus]     = "Opus",
    [ACC_Vorbis]   = "Vorbis",
    [ACC_FLAC]     = "FLAC",
    [ACC_TrueHD]   = "TrueHD",
    [ACC_PCM]      = "PCM",
};

static void text_add(struct text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

/**
 * text_add() - Add formatted text.
 * @text:   the text
 * @format: as printf()'s
 */
static void text_add(struct text *text, const char *format, ...)
{
	va_list list;
	va_start(list, format);
	int length = vsnprintf(text->chars + text->used, text->room - text->used, format, list);
	va_end(list);
	if (length > 0)
		text->used = text->used + (ULONG)length < text->room ? text->used + (ULONG)length : text->room - 1;
}

/**
 * text_add_time() - Add a time as h:mm:ss.
 * @text: the text
 * @time: the time, VC_TIME_UNKNOWN for one not known
 */
static void text_add_time(struct text *text, VCTime time)
{
	if (time == VC_TIME_UNKNOWN)
	{
		text_add(text, "?");
		return;
	}
	unsigned long seconds = (unsigned long)(time / MICROSECONDS_PER_SECOND);
	text_add(text, "%lu:%02lu:%02lu",
	         seconds / (SECONDS_PER_MINUTE * MINUTES_PER_HOUR),
	         seconds / SECONDS_PER_MINUTE % MINUTES_PER_HOUR,
	         seconds % SECONDS_PER_MINUTE);
}

/**
 * text_add_seconds() - Add a length of time as seconds with their tenths.
 * @text: the text
 * @time: the time, not negative
 */
static void text_add_seconds(struct text *text, VCTime time)
{
	unsigned long tenths = time > 0 ? (unsigned long)(time / MICROSECONDS_PER_TENTH) : 0;
	text_add(text, "%lu.%lu s", tenths / TENTHS_PER_SECOND, tenths % TENTHS_PER_SECOND);
}

/**
 * name_of() - A codec's name.
 * @names: the names by codec
 * @count: how many
 * @codec: the codec
 *
 * Return: the name, "?" for a codec without one.
 */
static const char *name_of(const char *const *names, ULONG count, ULONG codec)
{
	return codec < count && names[codec] != NULL ? names[codec] : "?";
}

/**
 * stats_reset() - Start counting anew, for a file just opened.
 * @engine: the engine
 */
void stats_reset(struct engine *engine)
{
	BOOL shown = engine->statistics.panel_on;
	memset(&engine->statistics, 0, sizeof(engine->statistics));
	engine->statistics.panel_on = shown;
	engine->statistics.second_start = clock_wall();
	engine->statistics.seek_target = VC_TIME_UNKNOWN;
}

/**
 * stats_count_seek() - Note a move in the film.
 * @engine: the engine
 * @time:   the time asked for
 */
void stats_count_seek(struct engine *engine, VCTime time)
{
	engine->statistics.seek_target = time;
	engine->statistics.seek_video = VC_TIME_UNKNOWN;
	engine->statistics.seek_audio = VC_TIME_UNKNOWN;
}

/**
 * reads_take() - Count what the reader read since the last look.
 * @engine: the engine
 *
 * The first packet of each stream after a move says where the file went on from.
 */
static void reads_take(struct engine *engine)
{
	struct statistics *statistics = &engine->statistics;
	struct reads reads;
	reader_reads_take(engine, &reads);
	statistics->video_bytes += reads.video_bytes;
	statistics->audio_bytes += reads.audio_bytes;
	if (statistics->seek_video == VC_TIME_UNKNOWN)
		statistics->seek_video = reads.video_first;
	if (statistics->seek_audio == VC_TIME_UNKNOWN)
		statistics->seek_audio = reads.audio_first;
}

/**
 * stats_count_frame() - Count a picture shown.
 * @engine: the engine
 * @frame:  the picture, about to be shown
 *
 * With the clock running, how far behind its time the picture is shown is
 * counted too.  frames_show() takes a picture only once its time has come,
 * so every one is behind by something, never ahead: what is measured is how
 * much, from its time to the engine starting to show it.  That holds the
 * timer's wake-up, the display still busy with the picture before and the
 * engine's other work; the subtitles' drawing and the display's own work
 * come after.  Pictures too late to show never get here: they are dropped
 * and counted apart (frame_count()).
 */
void stats_count_frame(struct engine *engine, const struct VCFrame *frame)
{
	struct statistics *statistics = &engine->statistics;
	statistics->frames++;
	statistics->picture_rect = frame->Crop;
	statistics->frame_width = frame->Width;
	statistics->frame_height = frame->Height;
	if (frame->Pts == VC_TIME_UNKNOWN || !engine->clock.running)
		return;
	VCTime behind = clock_now(&engine->clock) - frame->Pts;
	statistics->behind_sum += behind;
	statistics->behind_measured++;
	if (behind > statistics->behind_most)
		statistics->behind_most = behind;
}

/**
 * line_file() - The file: its container, length and size.
 * @engine: the engine
 * @text:   the text
 */
static void line_file(struct engine *engine, struct text *text)
{
	const struct DMXInfo *info = &engine->file_info;
	unsigned long megabytes = (unsigned long)info->FileSizeHigh * MB_PER_HIGH_WORD + (info->FileSizeLow >> BYTES_PER_MB_SHIFT);
	text_add(text, "File: %s, ", info->Format);
	text_add_time(text, info->Duration);
	text_add(text, ", %lu MB\n", megabytes);
}

/**
 * line_video() - The video stream and the size its pictures are shown at.
 * @engine: the engine
 * @text:   the text
 */
static void line_video(struct engine *engine, struct text *text)
{
	const struct DMXStream *stream = &engine->video_stream;
	if (engine->video_decoder == NULL)
	{
		text_add(text, "Video: none\n");
		return;
	}
	text_add(text, "Video: %s %ldx%ld",
	         name_of(video_codec_names, ARRAY_COUNT(video_codec_names), stream->Codec),
	         (long)engine->video_shape.Crop.Width,
	         (long)engine->video_shape.Crop.Height);
	if (stream->FrameRate.Num > 0 && stream->FrameRate.Den > 0)
	{
		unsigned long milli = (unsigned long)((unsigned long long)stream->FrameRate.Num * KILO / (ULONG)stream->FrameRate.Den);
		text_add(text, ", %lu.%03lu pictures a second", milli / KILO, milli % KILO);
	}
	const struct VCRect *picture = &engine->statistics.picture_rect;
	if (picture->Width != 0)
		text_add(text, ", shown at %ldx%ld", (long)picture->Width, (long)picture->Height);
	text_add(text, "\n");
}

/**
 * line_audio() - The audio stream and where its sound goes.
 * @engine: the engine
 * @text:   the text
 *
 * "shared" is ahi.device's writes in place of the unit's own hardware: by
 * the user's choice, or because another program held the unit.
 */
static void line_audio(struct engine *engine, struct text *text)
{
	const struct DMXStream *stream = &engine->audio_stream;
	if (engine->audio_decoder == NULL)
	{
		text_add(text, "Audio: none\n");
		return;
	}
	text_add(text, "Audio: %s %lu Hz, %lu channels, to AHI unit %lu as %s in %u bits%s\n",
	         name_of(audio_codec_names, ARRAY_COUNT(audio_codec_names), stream->Codec),
	         (unsigned long)stream->SampleRate,
	         (unsigned long)stream->Channels,
	         (unsigned long)engine->setup.audio_unit,
	         engine->audio_surround ? "7.1" : "stereo",
	         engine->audio_32bit ? 32u : 16u,
	         engine->audio_out != NULL && !audio_out_low_level(engine->audio_out) ? ", shared" : "");
}

/**
 * line_codecs() - The backends that decode, as the libraries chose them, and the converter behind the video's.
 * @engine: the engine
 * @text:   the text
 */
static void line_codecs(struct engine *engine, struct text *text)
{
	text_add(text, "Codecs:");
	if (engine->video_decoder != NULL)
	{
		text_add(text, " video %s", engine->video_backend);
		if (engine->video_converter[0] != 0)
			text_add(text, ", converted by %s", engine->video_converter);
	}
	if (engine->audio_decoder != NULL)
		text_add(text, "%s audio %s", engine->video_decoder != NULL ? ";" : "", engine->audio_backend);
	text_add(text, "\n");
}

/**
 * line_pictures() - The last second's pictures: how many a second, the drops, how late they were shown, what the decoder leaves out.
 * @engine: the engine
 * @text:   the text
 */
static void line_pictures(struct engine *engine, struct text *text)
{
	const struct statistics *statistics = &engine->statistics;
	if (engine->video_decoder == NULL)
		return;
	text_add(text, "Pictures: %lu.%02lu a second, %lu dropped; %ld ms late, %ld at most%s\n",
	         (unsigned long)statistics->last_rate / RATE_HUNDREDTHS,
	         (unsigned long)statistics->last_rate % RATE_HUNDREDTHS,
	         (unsigned long)engine->frames_dropped,
	         (long)statistics->last_behind_mean,
	         (long)statistics->last_behind_most,
	         name_of(skip_names, ARRAY_COUNT(skip_names), engine->skip.level));
}

/**
 * line_bit_rate() - The last second's bit rate of each stream played, as read.
 * @engine: the engine
 * @text:   the text
 */
static void line_bit_rate(struct engine *engine, struct text *text)
{
	const struct statistics *statistics = &engine->statistics;
	text_add(text, "Bit rate:");
	if (engine->video_decoder != NULL)
		text_add(text, " video %lu kbit/s", (unsigned long)statistics->last_video_kbps);
	if (engine->audio_decoder != NULL)
		text_add(text, "%s audio %lu kbit/s", engine->video_decoder != NULL ? "," : "", (unsigned long)statistics->last_audio_kbps);
	text_add(text, "\n");
}

/**
 * line_ahead() - How far the reading and the decoding are ahead of the clock: what the queues hold, the pictures waiting, the sound in the ring.
 * @engine: the engine
 * @text:   the text
 */
static void line_ahead(struct engine *engine, struct text *text)
{
	unsigned long ring = 0;
	if (engine->audio_out != NULL && engine->audio_stream.SampleRate != 0)
		ring = (unsigned long)((unsigned long long)audio_out_queued(engine->audio_out) * KILO / engine->audio_stream.SampleRate);
	ObtainSemaphore(&engine->queue_lock); /* the reader adds to the queues meanwhile */
	VCTime video = queue_duration(&engine->video_queue);
	VCTime audio = queue_duration(&engine->audio_queue);
	ReleaseSemaphore(&engine->queue_lock);
	text_add(text, "Read ahead: video ");
	text_add_seconds(text, video);
	text_add(text, ", audio ");
	text_add_seconds(text, audio);
	text_add(text, "; decoded: %lu pictures, %lu ms of sound\n", (unsigned long)engine->ready_count, ring);
}

/**
 * line_frame() - The frame the last picture was shown in, the picture's place in it, and a full screen's parts.
 * @engine: the engine
 * @text:   the text
 */
static void line_frame(struct engine *engine, struct text *text)
{
	const struct statistics *statistics = &engine->statistics;
	if (engine->video_decoder == NULL)
		return;
	text_add(text, "Frame: %lux%lu, the picture at %ld,%ld",
	         (unsigned long)statistics->frame_width,
	         (unsigned long)statistics->frame_height,
	         (long)statistics->picture_rect.X,
	         (long)statistics->picture_rect.Y);
	ULONG parts = display_parts(engine->display);
	if (parts != 0)
		text_add(text, ", a screen of %lu parts", (unsigned long)parts);
	text_add(text, "\n");
}

/**
 * line_queues() - The packets read ahead, counted.
 * @engine: the engine
 * @text:   the text
 */
static void line_queues(struct engine *engine, struct text *text)
{
	ObtainSemaphore(&engine->queue_lock); /* the reader adds to the queues meanwhile */
	text_add(text, "Queues: video %lu packets, %lu KB; audio %lu packets\n",
	         (unsigned long)engine->video_queue.count,
	         (unsigned long)engine->video_queue.bytes / BYTES_PER_KB,
	         (unsigned long)engine->audio_queue.count);
	ReleaseSemaphore(&engine->queue_lock);
}

/**
 * line_sound() - What the audio output has done: the chunks AHI played, and its restarts.
 * @engine: the engine
 * @text:   the text
 */
static void line_sound(struct engine *engine, struct text *text)
{
	if (engine->audio_out == NULL)
		return;
	ULONG chunks;
	ULONG restarts;
	ULONG step;
	audio_out_counts(engine->audio_out, &chunks, &restarts, &step);
	text_add(text, "Sound: %lu chunks played, started anew %lu times (step %lu)\n",
	         (unsigned long)chunks,
	         (unsigned long)restarts,
	         (unsigned long)step);
}

/**
 * line_clock() - The film's time and what the clock is doing.
 * @engine: the engine
 * @text:   the text
 */
static void line_clock(struct engine *engine, struct text *text)
{
	static const char *const states[] = {"no film", "paused", "playing", "ended"};
	VCTime now = clock_now(&engine->clock);
	text_add(text, "Clock: ");
	text_add_time(text, now);
	text_add(text, ".%03ld, %s, %s\n",
	         (long)(now / MICROSECONDS_PER_MS % KILO),
	         states[engine->state],
	         engine->clock.running ? "running" : engine->clock.waiting ? "waiting for the sound" : "stopped");
}

/**
 * line_seek() - The last move in the film: where to, and where the file's packets went on from.
 * @engine: the engine
 * @text:   the text
 */
static void line_seek(struct engine *engine, struct text *text)
{
	const struct statistics *statistics = &engine->statistics;
	if (statistics->seek_target == VC_TIME_UNKNOWN)
		return;
	text_add(text, "Last move: to ");
	text_add_time(text, statistics->seek_target);
	text_add(text, ", video from ");
	text_add_time(text, statistics->seek_video);
	text_add(text, ", audio from ");
	text_add_time(text, statistics->seek_audio);
	text_add(text, "\n");
}

/**
 * line_state() - Where the engine's parts stand, for finding out why a film does not move.
 * @engine: the engine
 * @text:   the text
 */
static void line_state(struct engine *engine, struct text *text)
{
	text_add(text, "State: file %s; ", engine->file_end ? "read to its end" : "being read");
	text_add(text, "video %lu packets to fill, %s%s%s%s; ",
	         (unsigned long)engine->free_packet_count,
	         display_busy(engine->display) ? "display busy" : "display free",
	         engine->first_shown ? "" : ", no picture yet",
	         engine->video_end_sent ? ", end sent" : "",
	         engine->video_done ? ", done" : "");
	text_add(text, "audio %s%s%s\n",
	         engine->pending_held ? "samples pending" : "nothing pending",
	         engine->audio_end_sent ? ", end sent" : "",
	         engine->audio_done ? ", done" : "");
}

/**
 * stats_write() - Write the statistics as lines of text.
 * @engine: the engine
 * @to:     gets the text, lines separated by newlines
 * @room:   its bytes
 * @inside: also the lines on the engine's inner workings, after the panel's (ARexx STATISTICS)
 *
 * The panel's lines say what is played, by what, and whether it keeps up;
 * the others are for looking into a film that does not play as it should.
 */
void stats_write(struct engine *engine, char *to, ULONG room, BOOL inside)
{
	struct text text = {.chars = to, .room = room};
	to[0] = 0;
	if (engine->state == ENGINE_EMPTY)
		return;
	line_file(engine, &text);
	line_video(engine, &text);
	line_audio(engine, &text);
	line_codecs(engine, &text);
	line_pictures(engine, &text);
	line_bit_rate(engine, &text);
	line_ahead(engine, &text);
	if (!inside)
		return;
	line_frame(engine, &text);
	line_queues(engine, &text);
	line_sound(engine, &text);
	line_clock(engine, &text);
	line_seek(engine, &text);
	line_state(engine, &text);
}

/**
 * stats_tick() - Sum up the second that has passed, if one has, and write the panel's text anew.
 * @engine: the engine
 */
void stats_tick(struct engine *engine)
{
	struct statistics *statistics = &engine->statistics;
	reads_take(engine);
	VCTime passed = clock_wall() - statistics->second_start;
	if (passed < MICROSECONDS_PER_SECOND)
		return;
	unsigned long long ms = (unsigned long long)(passed / MICROSECONDS_PER_MS);

	statistics->last_rate = (ULONG)((unsigned long long)statistics->frames * RATE_HUNDREDTHS * KILO / ms);
	VCTime behind_mean = statistics->behind_measured != 0 ? statistics->behind_sum / (VCTime)statistics->behind_measured : 0;
	statistics->last_behind_mean = (LONG)(behind_mean / MICROSECONDS_PER_MS);
	statistics->last_behind_most = (LONG)(statistics->behind_most / MICROSECONDS_PER_MS);
	statistics->last_video_kbps = (ULONG)((unsigned long long)statistics->video_bytes * BITS_PER_BYTE / ms);
	statistics->last_audio_kbps = (ULONG)((unsigned long long)statistics->audio_bytes * BITS_PER_BYTE / ms);
	statistics->frames = 0;
	statistics->behind_sum = 0;
	statistics->behind_measured = 0;
	statistics->behind_most = 0;
	statistics->video_bytes = 0;
	statistics->audio_bytes = 0;
	statistics->second_start += passed;
	if (statistics->panel_on)
	{
		stats_write(engine, statistics->text, sizeof(statistics->text), FALSE);
		statistics->text_version++;
	}
}
