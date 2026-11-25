/* SPDX-License-Identifier: MPL-2.0 */
/*
 * engine.c - the engine: the front's calls, the engine's task, its commands
 * and its loop.
 *
 * The task sleeps on four things: a command from the front, the video
 * decoder's signal, the sound output's "room again" and the timer set for
 * the next picture.  Whatever wakes it, it makes one pass: read the file
 * ahead, decode sound into the output's ring, feed the video decoder and
 * show the pictures that are due.  It never waits inside a pass.
 */
#include <string.h>
#include <exec/memory.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "engine_private.h"
#include "trace.h"

#define TASK_STACK (512 * 1024)    /* the audio decoders run on the engine's stack */
#define TASK_PRIORITY 1             /* below ahi.device's unit process (2): held up by the engine's work, AHI stops playing linked writes */
#define MICROSECONDS_PER_SECOND 1000000
#define WAKE_LONGEST 500000        /* microseconds: the longest the timer sleeps for a picture, as the clock may be corrected meanwhile */

/* ---- the engine's task ------------------------------------------------------------- */

/**
 * engine_wake_in() - Set the timer to wake the engine.
 * @engine: the engine
 * @delay:  microseconds from now
 *
 * A timer already set for an earlier time is left alone; one set for later is set again.
 */
void engine_wake_in(struct engine *engine, VCTime delay)
{
	struct IORequest *request = &engine->timer->tr_node;
	if (delay > WAKE_LONGEST)
		delay = WAKE_LONGEST;
	VCTime wake_time = clock_wall() + delay;
	if (engine->timer_sent)
	{
		if (CheckIO(request) == NULL && engine->wake_time <= wake_time)
			return;
		if (CheckIO(request) == NULL)
			AbortIO(request);
		WaitIO(request);
	}
	engine->timer->tr_node.io_Command = TR_ADDREQUEST;
	engine->timer->tr_time.tv_secs = (LONG)(delay / MICROSECONDS_PER_SECOND);
	engine->timer->tr_time.tv_micro = (LONG)(delay % MICROSECONDS_PER_SECOND);
	SendIO(request);
	engine->timer_sent = TRUE;
	engine->wake_time = wake_time;
}

/**
 * timer_collect() - Take the timer's request back if it has come.
 * @engine: the engine
 */
static void timer_collect(struct engine *engine)
{
	if (engine->timer_sent && CheckIO(&engine->timer->tr_node) != NULL)
	{
		WaitIO(&engine->timer->tr_node);
		engine->timer_sent = FALSE;
	}
}

/**
 * engine_clock_start() - Start the clock, if the engine is playing and the clock can start.
 * @engine: the engine
 *
 * With sound the clock waits for the audio output to say where the sound is.
 * Without, it starts once the first picture is shown, at that picture's time.
 */
void engine_clock_start(struct engine *engine)
{
	if (engine->state != ENGINE_PLAYING)
		return;
	if (engine->audio_decoder != NULL && !engine->audio_done)
	{
		if (engine->audio_out != NULL)
			audio_out_run(engine->audio_out, TRUE);
		clock_run(&engine->clock, TRUE);
	}
	else if (engine->video_decoder == NULL || engine->first_shown)
	{
		clock_run(&engine->clock, FALSE);
	}
}

/**
 * stand_still() - Stop the clock and the sound.
 * @engine: the engine
 * @state:  what the engine is doing from now: ENGINE_PAUSED or ENGINE_ENDED
 */
static void stand_still(struct engine *engine, enum engine_state state)
{
	engine->state = state;
	clock_stop(&engine->clock);
	if (engine->audio_out != NULL)
		audio_out_run(engine->audio_out, FALSE);
}

/**
 * played_out() - Whether everything of the file has been shown and heard.
 * @engine: the engine
 *
 * Return: TRUE at the file's end with both decoders empty and the sound output's ring played.
 */
static BOOL played_out(struct engine *engine)
{
	BOOL video_over = engine->video_decoder == NULL || (engine->video_done && engine->ready_count == 0);
	BOOL audio_over = engine->audio_decoder == NULL ||
	                  (engine->audio_done && !engine->pending_held && (engine->audio_out == NULL || audio_out_queued(engine->audio_out) == 0));
	return engine->file_end && video_over && audio_over;
}

/**
 * pass() - One pass over the subtitles read and both decoders.
 * @engine: the engine, a file open
 *
 * The reader fills the queues meanwhile, and wakes the engine for another
 * pass as it does.  The front is signalled when the film ends or something fails.
 */
static void pass(struct engine *engine)
{
	LONG error_before = engine->error;
	TRACE_T0(started);
	reader_subtitles(engine);
	TRACE_T0(heard);
	audio_pump(engine);
	TRACE_ADD(TRACE_AUDIO, heard);
	TRACE_T0(seen);
	video_pump(engine);
	TRACE_ADD(TRACE_VIDEO, seen);
	TRACE_ADD(TRACE_PASS, started);

	trace_report();
	stats_tick(engine);
	BOOL ended = engine->state == ENGINE_PLAYING && played_out(engine);
	if (ended)
		stand_still(engine, ENGINE_ENDED);
	if (ended || engine->error != error_before)
		Signal(engine->setup.listener, engine->setup.signal);
}

/**
 * film_close() - Close the decoders, the file and the subtitles: the engine stands empty.
 * @engine: the engine
 */
static void film_close(struct engine *engine)
{
	video_close(engine);
	audio_close(engine);
	file_close(engine);
	subtitles_flush(engine->subtitles);
	engine->subtitle_index = ENGINE_TRACK_OFF;
	if (engine->state != ENGINE_EMPTY)
		display_clear(engine->display);
	engine->state = ENGINE_EMPTY;
	clock_set(&engine->clock, 0);
}

/**
 * seek_to() - Move to the key frame at or before a time.
 * @engine: the engine
 * @time:   the film's time
 *
 * A film that has ended stands still at the new place.
 *
 * Return: 0 or ENGINE_ERR_SEEK.
 */
static LONG seek_to(struct engine *engine, VCTime time)
{
	if (engine->state == ENGINE_EMPTY || !engine->file_info.Seekable || file_seek(engine, time) != 0)
		return ENGINE_ERR_SEEK;
	stats_count_seek(engine, time);
	video_flush(engine);
	audio_flush(engine);
	if (engine->state == ENGINE_ENDED)
		engine->state = ENGINE_PAUSED;
	clock_set(&engine->clock, time);
	engine_clock_start(engine);
	return 0;
}

/**
 * reseek() - Start again from where the film is.
 * @engine: the engine, a file open
 *
 * After something changed that the decoders cannot take in passing: another
 * track, another sound unit, another place for a film that stands still.
 * A file that cannot be moved in goes on as it is.
 */
static void reseek(struct engine *engine)
{
	seek_to(engine, clock_now(&engine->clock));
}

/**
 * handle_open() - COMMAND_OPEN: open a file and stand at its first picture.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0 or the ENGINE_ERR_* of what failed; then no file is open.
 */
static LONG handle_open(struct engine *engine, struct engine_message *message)
{
	film_close(engine);
	video_place(engine, message->with.open.place);
	engine->error = 0;
	engine->frames_shown = 0;
	engine->frames_dropped = 0;
	stats_reset(engine);
	memset(&engine->video_shape, 0, sizeof(engine->video_shape));

	LONG error = file_open(engine, message->with.open.name);
	if (error == 0)
		error = video_open(engine);
	if (error == 0)
		audio_open(engine);
	if (error == 0 && engine->video_decoder == NULL && engine->audio_decoder == NULL)
		error = ENGINE_ERR_NOSTREAM;
	if (error == 0 && !reader_start(engine))
		error = ENGINE_ERR_FILE;
	if (error != 0)
	{
		film_close(engine);
		return error;
	}
	engine->state = ENGINE_PAUSED;
	return 0;
}

/**
 * handle_close() - COMMAND_CLOSE.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0.
 */
static LONG handle_close(struct engine *engine, struct engine_message *message)
{
	(void)message;
	film_close(engine);
	return 0;
}

/**
 * handle_play() - COMMAND_PLAY: run or stand still.
 * @engine:  the engine
 * @message: the command
 *
 * A film that has ended stays ended until it is moved in.
 *
 * Return: 0.
 */
static LONG handle_play(struct engine *engine, struct engine_message *message)
{
	if (engine->state == ENGINE_EMPTY || engine->state == ENGINE_ENDED)
		return 0;
	if (message->with.play)
	{
		engine->state = ENGINE_PLAYING;
		engine_clock_start(engine);
	}
	else
	{
		stand_still(engine, ENGINE_PAUSED);
	}
	return 0;
}

/**
 * handle_seek() - COMMAND_SEEK.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0 or ENGINE_ERR_SEEK.
 */
static LONG handle_seek(struct engine *engine, struct engine_message *message)
{
	return seek_to(engine, message->with.seek_time);
}

/**
 * handle_volume() - COMMAND_VOLUME.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0.
 */
static LONG handle_volume(struct engine *engine, struct engine_message *message)
{
	engine->volume = message->with.volume;
	if (engine->audio_out != NULL)
		audio_out_volume(engine->audio_out, engine->volume);
	return 0;
}

/**
 * handle_place() - COMMAND_PLACE: show the pictures in another place.
 * @engine:  the engine
 * @message: the command
 *
 * A film that stands still is started again from where it is first, so
 * that the new place gets a picture.
 *
 * Return: 0, ENGINE_ERR_VIDEO for a place pictures cannot be shown in.
 */
static LONG handle_place(struct engine *engine, struct engine_message *message)
{
	if (engine->video_decoder != NULL && engine->state != ENGINE_PLAYING)
		reseek(engine); /* before the decoder is told its new frames: it takes no flush between that and making them */
	video_place(engine, message->with.place);
	return message->with.place != NULL && engine->display == NULL ? ENGINE_ERR_VIDEO : 0;
}

/**
 * handle_status() - COMMAND_STATUS: say what the engine is doing.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0.
 */
static LONG handle_status(struct engine *engine, struct engine_message *message)
{
	*message->with.status = (struct engine_status){
	    .state = engine->state,
	    .error = engine->error,
	    .position = clock_now(&engine->clock),
	    .duration = engine->state != ENGINE_EMPTY ? engine->file_info.Duration : VC_TIME_UNKNOWN,
	    .seekable = engine->state != ENGINE_EMPTY && engine->file_info.Seekable,
	    .has_video = engine->video_decoder != NULL,
	    .has_audio = engine->audio_decoder != NULL,
	    .width = (ULONG)engine->video_shape.Crop.Width,
	    .height = (ULONG)engine->video_shape.Crop.Height,
	    .frames_shown = engine->frames_shown,
	    .frames_dropped = engine->frames_dropped,
	    .audio_delay = engine->audio_delay,
	    .subtitle_delay = engine->subtitle_delay,
	};
	return 0;
}

/**
 * handle_tracks() - COMMAND_TRACKS.
 * @engine:  the engine
 * @message: the command
 *
 * Return: how many tracks were listed.
 */
static LONG handle_tracks(struct engine *engine, struct engine_message *message)
{
	return (LONG)file_tracks(engine, message->with.tracks.list, message->with.tracks.room);
}

/**
 * handle_select() - COMMAND_SELECT: another audio or subtitle track.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0, ENGINE_ERR_NOSTREAM or ENGINE_ERR_FILE.
 */
static LONG handle_select(struct engine *engine, struct engine_message *message)
{
	LONG error = file_select(engine, message->with.select.kind, message->with.select.track);
	if (error == 0)
		reseek(engine);
	return error;
}

/**
 * handle_unit() - COMMAND_UNIT: the sound goes to another ahi.device unit, or to the same one another way.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0.
 */
static LONG handle_unit(struct engine *engine, struct engine_message *message)
{
	engine->setup.audio_unit = message->with.unit.number;
	engine->setup.audio_shared = message->with.unit.shared;
	if (engine->state != ENGINE_EMPTY && engine->audio_index != NO_STREAM)
		handle_select(engine, &(struct engine_message){.with.select = {.kind = ENGINE_TRACK_AUDIO, .track = engine->audio_index}});
	return 0;
}

/**
 * handle_subtitle_file() - COMMAND_SUBTITLE_FILE: a SubRip file as the film's subtitles.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0 or ENGINE_ERR_FILE.
 */
static LONG handle_subtitle_file(struct engine *engine, struct engine_message *message)
{
	return file_load_subtitles(engine, message->with.subtitle_file);
}

/**
 * handle_subtitle_font() - COMMAND_SUBTITLE_FONT: draw the subtitles with another font.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0 or ENGINE_ERR_FILE.
 */
static LONG handle_subtitle_font(struct engine *engine, struct engine_message *message)
{
	LONG error = subtitles_set_font(engine->subtitles, message->with.subtitle_font);
	if (error == 0 && engine->state != ENGINE_EMPTY)
		reseek(engine);
	return error;
}

/**
 * handle_chapters() - COMMAND_CHAPTERS.
 * @engine:  the engine
 * @message: the command
 *
 * Return: how many chapters were listed.
 */
static LONG handle_chapters(struct engine *engine, struct engine_message *message)
{
	return (LONG)file_chapters(engine, message->with.chapters.list, message->with.chapters.room);
}

/**
 * handle_delays() - COMMAND_DELAYS: put the sound and the subtitles behind the picture.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0.
 */
static LONG handle_delays(struct engine *engine, struct engine_message *message)
{
	engine->audio_delay = message->with.delays.audio;
	engine->subtitle_delay = message->with.delays.subtitles;
	if (engine->audio_out != NULL)
		audio_out_delay(engine->audio_out, engine->audio_delay);
	return 0;
}

/**
 * handle_aspect() - COMMAND_ASPECT: show the picture in another shape.
 * @engine:  the engine
 * @message: the command
 *
 * Return: 0.
 */
static LONG handle_aspect(struct engine *engine, struct engine_message *message)
{
	engine->aspect = message->with.aspect;
	if (engine->video_decoder != NULL && engine->video_shape.Crop.Width != 0)
	{
		if (engine->state != ENGINE_PLAYING)
			reseek(engine); /* before the decoder is told its new frames, as in handle_place() */
		video_attach(engine);
	}
	return 0;
}

/**
 * handle_statistics() - COMMAND_STATISTICS: show or hide the panel, and say the statistics.
 * @engine:  the engine
 * @message: the command
 *
 * A film that stands still is drawn again, with the panel or without.
 *
 * Return: 0.
 */
static LONG handle_statistics(struct engine *engine, struct engine_message *message)
{
	BOOL changed = engine->statistics.panel_on != message->with.statistics.panel;
	engine->statistics.panel_on = message->with.statistics.panel;
	if (message->with.statistics.text != NULL)
		stats_write(engine, message->with.statistics.text, message->with.statistics.room);
	if (changed && engine->state != ENGINE_EMPTY)
	{
		stats_write(engine, engine->statistics.text, sizeof(engine->statistics.text));
		engine->statistics.text_version++;
		if (engine->state != ENGINE_PLAYING)
			reseek(engine);
	}
	return 0;
}

/* What does each command, by its number */
static LONG (*const handlers[COMMAND_END])(struct engine *engine, struct engine_message *message) = {
    [COMMAND_OPEN]          = handle_open,
    [COMMAND_CLOSE]         = handle_close,
    [COMMAND_PLAY]          = handle_play,
    [COMMAND_SEEK]          = handle_seek,
    [COMMAND_VOLUME]        = handle_volume,
    [COMMAND_PLACE]         = handle_place,
    [COMMAND_STATUS]        = handle_status,
    [COMMAND_TRACKS]        = handle_tracks,
    [COMMAND_SELECT]        = handle_select,
    [COMMAND_UNIT]          = handle_unit,
    [COMMAND_SUBTITLE_FILE] = handle_subtitle_file,
    [COMMAND_SUBTITLE_FONT] = handle_subtitle_font,
    [COMMAND_CHAPTERS]      = handle_chapters,
    [COMMAND_DELAYS]        = handle_delays,
    [COMMAND_ASPECT]        = handle_aspect,
    [COMMAND_STATISTICS]    = handle_statistics,
};

/**
 * command_do() - Do one command of the front.
 * @engine:  the engine
 * @message: the command; its result is set
 */
static void command_do(struct engine *engine, struct engine_message *message)
{
	message->result = message->command < COMMAND_END ? handlers[message->command](engine, message) : 0;
}

/**
 * task_setup() - Get what the engine's task needs: two signals and the timer.
 * @engine: the engine
 *
 * Return: TRUE, FALSE when something could not be had.
 */
static BOOL task_setup(struct engine *engine)
{
	engine->video_signal = AllocSignal(-1);
	engine->audio_signal = AllocSignal(-1);
	engine->reader_signal = AllocSignal(-1);
	InitSemaphore(&engine->file_lock);
	InitSemaphore(&engine->queue_lock);
	engine->timer_port = CreateMsgPort();
	engine->port = CreateMsgPort();
	engine->subtitles = subtitles_open();
	engine->subtitle_index = ENGINE_TRACK_OFF;
	if (engine->video_signal == -1 || engine->audio_signal == -1 || engine->reader_signal == -1 || engine->timer_port == NULL ||
	    engine->port == NULL || engine->subtitles == NULL)
		return FALSE;
	engine->timer = (struct timerequest *)CreateIORequest(engine->timer_port, sizeof(*engine->timer));
	if (engine->timer == NULL)
		return FALSE;
	engine->timer_open = OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ, &engine->timer->tr_node, 0) == 0;
	return engine->timer_open;
}

/**
 * task_cleanup() - Let go of what task_setup() got, as far as it got.
 * @engine: the engine, no file open
 */
static void task_cleanup(struct engine *engine)
{
	if (engine->timer_sent)
	{
		AbortIO(&engine->timer->tr_node);
		WaitIO(&engine->timer->tr_node);
	}
	if (engine->timer_open)
		CloseDevice(&engine->timer->tr_node);
	if (engine->timer != NULL)
		DeleteIORequest(&engine->timer->tr_node);
	if (engine->timer_port != NULL)
		DeleteMsgPort(engine->timer_port);
	if (engine->port != NULL)
		DeleteMsgPort(engine->port);
	subtitles_close(engine->subtitles);
	FreeSignal(engine->reader_signal);
	FreeSignal(engine->audio_signal);
	FreeSignal(engine->video_signal);
}

/**
 * task_loop() - The engine's loop: sleep, do the front's commands, make a pass.
 * @engine: the engine
 *
 * Return: the COMMAND_END message, still to be answered.
 */
static struct Message *task_loop(struct engine *engine)
{
	struct MsgPort *control = engine->port;
	ULONG signals = 1UL << control->mp_SigBit | 1UL << engine->video_signal | 1UL << engine->audio_signal |
	                1UL << engine->reader_signal | 1UL << engine->timer_port->mp_SigBit;
	for (;;)
	{
		Wait(signals);
		struct engine_message *message;
		while ((message = (struct engine_message *)GetMsg(control)) != NULL)
		{
			if (message->command == COMMAND_END)
				return &message->message;
			command_do(engine, message);
			ReplyMsg(&message->message);
		}
		timer_collect(engine);
		if (engine->state != ENGINE_EMPTY)
			pass(engine);
	}
}

/**
 * task_entry() - The engine's task: set up, answer the start, serve until the end command, let go, answer it.
 */
static void task_entry(void)
{
	struct MsgPort *own = &((struct Process *)FindTask(NULL))->pr_MsgPort; /* for the start only: dos.library's from then on */
	WaitPort(own);
	struct engine_message *start = (struct engine_message *)GetMsg(own);
	struct engine *engine = start->with.engine;

	struct Message *end = &start->message;
	start->result = task_setup(engine) ? 0 : ENGINE_ERR_NOMEM;
	if (start->result == 0)
	{
		ReplyMsg(&start->message);
		end = task_loop(engine);
		film_close(engine);
		video_place(engine, NULL);
	}
	task_cleanup(engine);
	Forbid(); /* until the task is gone: the front frees what it runs on */
	ReplyMsg(end);
}

/* ---- the front's calls -------------------------------------------------------------- */

/**
 * command_send() - Send the engine a command and wait for its answer.
 * @engine:  the engine
 * @message: the command, filled in
 *
 * The start goes to the process's own port, before the engine's exists.
 *
 * Return: the command's result.
 */
static LONG command_send(struct engine *engine, struct engine_message *message)
{
	message->message.mn_ReplyPort = engine->reply_port;
	message->message.mn_Length = sizeof(*message);
	PutMsg(engine->port != NULL ? engine->port : &engine->process->pr_MsgPort, &message->message);
	WaitPort(engine->reply_port);
	GetMsg(engine->reply_port);
	return message->result;
}

/**
 * engine_start() - Start the engine's task.
 * @setup: whom to signal, and the sound unit
 *
 * Return: the engine, with no file; NULL when it did not come up.
 */
struct engine *engine_start(const struct engine_setup *setup)
{
	struct engine *engine = AllocVec(sizeof(*engine), MEMF_PUBLIC | MEMF_CLEAR);
	if (engine == NULL)
		return NULL;
	engine->setup = *setup;
	engine->volume = AUDIO_VOLUME_FULL;
	engine->video_signal = -1;
	engine->audio_signal = -1;
	engine->reader_signal = -1;
	engine->reply_port = CreateMsgPort();
	if (engine->reply_port != NULL)
		engine->process = CreateNewProcTags(NP_Entry, (ULONG)task_entry, NP_Name, (ULONG) "Kinora engine", NP_StackSize, TASK_STACK,
		                                    NP_Priority, TASK_PRIORITY, TAG_DONE);
	if (engine->process != NULL)
	{
		engine->start.with.engine = engine;
		if (command_send(engine, &engine->start) != 0)
			engine->process = NULL; /* it answered once and is gone */
	}
	if (engine->process == NULL)
	{
		engine_end(engine);
		return NULL;
	}
	return engine;
}

/**
 * engine_end() - Close the file, end the engine's task and free the engine.
 * @engine: the engine, whole or half made; NULL does nothing
 */
void engine_end(struct engine *engine)
{
	if (engine == NULL)
		return;
	if (engine->process != NULL)
	{
		struct engine_message message = {.command = COMMAND_END};
		command_send(engine, &message);
	}
	if (engine->reply_port != NULL)
		DeleteMsgPort(engine->reply_port);
	FreeVec(engine);
}

/**
 * engine_open() - Open a file; it stands at its first picture.
 * @engine: the engine
 * @name:   the file
 * @place:  the box for its pictures; NULL or one without a window plays the sound only
 *
 * Return: 0 or an ENGINE_ERR_*.
 */
LONG engine_open(struct engine *engine, CONST_STRPTR name, const struct display_place *place)
{
	struct engine_message message = {.command = COMMAND_OPEN, .with.open = {.name = name, .place = place}};
	return command_send(engine, &message);
}

/**
 * engine_close() - Close the file.
 * @engine: the engine
 */
void engine_close(struct engine *engine)
{
	struct engine_message message = {.command = COMMAND_CLOSE};
	command_send(engine, &message);
}

/**
 * engine_play() - Run or stand still.
 * @engine: the engine
 * @play:   run
 */
void engine_play(struct engine *engine, BOOL play)
{
	struct engine_message message = {.command = COMMAND_PLAY, .with.play = play};
	command_send(engine, &message);
}

/**
 * engine_seek() - Move to the key frame at or before a time.
 * @engine: the engine
 * @time:   the film's time
 *
 * Return: 0 or ENGINE_ERR_SEEK.
 */
LONG engine_seek(struct engine *engine, VCTime time)
{
	struct engine_message message = {.command = COMMAND_SEEK, .with.seek_time = time};
	return command_send(engine, &message);
}

/**
 * engine_volume() - Set the sound's level.
 * @engine: the engine
 * @volume: 0 to AUDIO_VOLUME_FULL
 */
void engine_volume(struct engine *engine, ULONG volume)
{
	struct engine_message message = {.command = COMMAND_VOLUME, .with.volume = volume};
	command_send(engine, &message);
}

/**
 * engine_place() - Show the pictures in another place.
 * @engine: the engine
 * @place:  the new place: a window's box or a screen; NULL before the window or screen closes
 *
 * Return: 0, ENGINE_ERR_VIDEO for a place pictures cannot be shown in; they then go nowhere.
 */
LONG engine_place(struct engine *engine, const struct display_place *place)
{
	struct engine_message message = {.command = COMMAND_PLACE, .with.place = place};
	return command_send(engine, &message);
}

/**
 * engine_status() - Ask what the engine is doing.
 * @engine: the engine
 * @status: filled in
 */
void engine_status(struct engine *engine, struct engine_status *status)
{
	struct engine_message message = {.command = COMMAND_STATUS, .with.status = status};
	command_send(engine, &message);
}

/**
 * engine_tracks() - List the file's audio and subtitle tracks.
 * @engine: the engine
 * @tracks: filled in
 * @room:   how many it holds
 *
 * Return: how many were listed.
 */
ULONG engine_tracks(struct engine *engine, struct engine_track *tracks, ULONG room)
{
	struct engine_message message = {.command = COMMAND_TRACKS, .with.tracks = {.list = tracks, .room = room}};
	return (ULONG)command_send(engine, &message);
}

/**
 * engine_select() - Choose the audio or the subtitle track.
 * @engine: the engine
 * @kind:   audio or subtitle
 * @stream: an engine_track's stream, or ENGINE_TRACK_OFF
 *
 * Return: 0, ENGINE_ERR_NOSTREAM or ENGINE_ERR_FILE.
 */
LONG engine_select(struct engine *engine, enum engine_track_kind kind, LONG stream)
{
	struct engine_message message = {.command = COMMAND_SELECT, .with.select = {.kind = kind, .track = stream}};
	return command_send(engine, &message);
}

/**
 * engine_audio_unit() - Send the sound to another ahi.device unit, or to the same one another way.
 * @engine: the engine
 * @unit:   the unit
 * @shared: share it with other programs, 6 dB low; else take its hardware, at the file's level
 */
void engine_audio_unit(struct engine *engine, ULONG unit, BOOL shared)
{
	struct engine_message message = {.command = COMMAND_UNIT, .with.unit = {.number = unit, .shared = shared}};
	command_send(engine, &message);
}

/**
 * engine_subtitle_file() - Take a SubRip file as the film's subtitles and show them.
 * @engine: the engine
 * @name:   the file
 *
 * Return: 0, ENGINE_ERR_FILE when it does not open or holds no cue.
 */
LONG engine_subtitle_file(struct engine *engine, CONST_STRPTR name)
{
	struct engine_message message = {.command = COMMAND_SUBTITLE_FILE, .with.subtitle_file = name};
	return command_send(engine, &message);
}

/**
 * engine_subtitle_font() - Draw the subtitles with another font.
 * @engine: the engine
 * @name:   the font file: TrueType or OpenType
 *
 * Return: 0, ENGINE_ERR_FILE when the file is no font; the font before then stays.
 */
LONG engine_subtitle_font(struct engine *engine, CONST_STRPTR name)
{
	struct engine_message message = {.command = COMMAND_SUBTITLE_FONT, .with.subtitle_font = name};
	return command_send(engine, &message);
}

/**
 * engine_chapters() - List the film's chapters.
 * @engine:   the engine
 * @chapters: filled in
 * @room:     how many it holds
 *
 * Return: how many were listed.
 */
ULONG engine_chapters(struct engine *engine, struct engine_chapter *chapters, ULONG room)
{
	struct engine_message message = {.command = COMMAND_CHAPTERS, .with.chapters = {.list = chapters, .room = room}};
	return (ULONG)command_send(engine, &message);
}

/**
 * engine_delays() - Put the sound and the subtitles behind the picture, or ahead of it.
 * @engine:    the engine
 * @audio:     microseconds the sound comes later; negative for earlier
 * @subtitles: microseconds the subtitles come later; negative for earlier
 */
void engine_delays(struct engine *engine, VCTime audio, VCTime subtitles)
{
	struct engine_message message = {.command = COMMAND_DELAYS, .with.delays = {.audio = audio, .subtitles = subtitles}};
	command_send(engine, &message);
}

/**
 * engine_aspect() - Show the picture in another shape.
 * @engine: the engine
 * @width:  the shape's width to its @height; 0 for the film's own; DISPLAY_ASPECT_FILL to stretch it over the place
 * @height: see @width
 */
void engine_aspect(struct engine *engine, LONG width, LONG height)
{
	struct engine_message message = {.command = COMMAND_ASPECT, .with.aspect = {width, height}};
	command_send(engine, &message);
}

/**
 * engine_statistics() - Show or hide the statistics panel in the picture, and get the statistics as text.
 * @engine: the engine
 * @panel:  draw the panel into the pictures
 * @text:   gets the statistics as lines of text; NULL for none
 * @room:   its bytes
 */
void engine_statistics(struct engine *engine, BOOL panel, char *text, ULONG room)
{
	struct engine_message message = {.command = COMMAND_STATISTICS, .with.statistics = {.panel = panel, .text = text, .room = room}};
	command_send(engine, &message);
}
