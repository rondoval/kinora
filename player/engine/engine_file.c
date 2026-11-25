/* SPDX-License-Identifier: MPL-2.0 */
/*
 * engine_file.c - the engine's file: opened, its streams chosen, its tracks
 * and chapters listed, seeking.  Once the reader runs (engine_reader.c),
 * every DMX_ call here is made with it held.
 */
#include <string.h>
#include <proto/demuxer.h>

#include "engine_private.h"

/**
 * stream_better() - Whether a stream is a better choice than the one chosen so far.
 * @stream: the stream looked at
 * @kind:   DMXK_*: the kind wanted
 * @chosen: the choice so far, valid when @have
 * @have:   a stream was chosen already
 *
 * Return: TRUE for the first stream of the kind a decoder is named for, and for the file's default over one that is not.
 */
static BOOL stream_better(const struct DMXStream *stream, ULONG kind, const struct DMXStream *chosen, BOOL have)
{
	if (stream->Kind != kind || stream->Codec == 0)
		return FALSE;
	return !have || ((stream->Flags & DMXSF_Default) != 0 && (chosen->Flags & DMXSF_Default) == 0);
}

/**
 * streams_choose() - Choose the video and the audio stream to play.
 * @engine: the engine, its source open
 */
static void streams_choose(struct engine *engine)
{
	engine->video_index = NO_STREAM;
	engine->audio_index = NO_STREAM;
	for (ULONG index = 0; index < engine->file_info.Streams && index < SOURCE_STREAMS; index++)
	{
		struct DMXStream stream = {.Size = sizeof(stream)};
		if (DMX_GetStream(engine->source, index, &stream) != 0)
			continue;
		if (stream_better(&stream, DMXK_Video, &engine->video_stream, engine->video_index != NO_STREAM))
		{
			engine->video_stream = stream;
			engine->video_index = (LONG)index;
		}
		if (stream_better(&stream, DMXK_Audio, &engine->audio_stream, engine->audio_index != NO_STREAM))
		{
			engine->audio_stream = stream;
			engine->audio_index = (LONG)index;
		}
	}
}

/**
 * streams_request() - Tell the demuxer which streams' packets are wanted.
 * @engine: the engine, its source open; the reader held when it runs
 */
static void streams_request(struct engine *engine)
{
	ULONG mask = 0;
	if (engine->video_index != NO_STREAM)
		mask |= 1UL << engine->video_index;
	if (engine->audio_index != NO_STREAM)
		mask |= 1UL << engine->audio_index;
	if (engine->subtitle_index >= 0)
		mask |= 1UL << engine->subtitle_index;
	reader_hold(engine);
	DMX_SelectStreams(engine->source, mask);
	reader_let_go(engine);
}

/**
 * file_load_subtitles() - Take a SubRip file as the film's subtitles and show them.
 * @engine: the engine, a file open
 * @name:   the SubRip file
 *
 * Return: 0, ENGINE_ERR_FILE when it does not open or holds no cue; what was shown before then stays.
 */
LONG file_load_subtitles(struct engine *engine, CONST_STRPTR name)
{
	if (engine->source == NULL || strlen((const char *)name) >= SUBTITLE_NAME_SIZE || subtitles_load(engine->subtitles, name) != 0)
		return ENGINE_ERR_FILE;
	strcpy(engine->subtitle_name, (const char *)name);
	engine->subtitle_index = ENGINE_TRACK_FILE;
	streams_request(engine);
	return 0;
}

/**
 * subtitle_file_find() - Take the SubRip file that lies beside the film, if there is one.
 * @engine: the engine, the film open
 * @name:   the film's name; the SubRip file's is the same with .srt for its last part
 */
static void subtitle_file_find(struct engine *engine, CONST_STRPTR name)
{
	char beside[SUBTITLE_NAME_SIZE];
	size_t length = strlen((const char *)name);
	if (length + sizeof(".srt") > sizeof(beside))
		return;
	strcpy(beside, (const char *)name);
	char *dot = strrchr(beside, '.');
	if (dot != NULL && strpbrk(dot, "/:") == NULL)
		*dot = 0;
	strcat(beside, ".srt");
	file_load_subtitles(engine, (CONST_STRPTR)beside);
}

/**
 * file_open() - Open a file and choose what to play of it.
 * @engine: the engine, no file open
 * @name:   the file
 *
 * Return: 0, ENGINE_ERR_FILE or ENGINE_ERR_NOSTREAM.
 */
LONG file_open(struct engine *engine, CONST_STRPTR name)
{
	engine->source = DMX_OpenTags(DMXA_FileName, (ULONG)name, TAG_DONE);
	if (engine->source == NULL)
		return ENGINE_ERR_FILE;
	engine->file_info = (struct DMXInfo){.Size = sizeof(engine->file_info)};
	DMX_GetInfo(engine->source, &engine->file_info);
	streams_choose(engine);
	if (engine->video_index == NO_STREAM && engine->audio_index == NO_STREAM)
		return ENGINE_ERR_NOSTREAM;

	engine->subtitle_index = ENGINE_TRACK_OFF;
	engine->subtitle_name[0] = 0;
	subtitle_file_find(engine, name);
	streams_request(engine);
	engine->file_end = FALSE;
	reader_reads_reset(engine);
	return 0;
}

/**
 * file_close() - End the reader, release the packets read ahead and close the file.
 * @engine: the engine; without a file nothing happens
 */
void file_close(struct engine *engine)
{
	if (engine->source == NULL)
		return;
	reader_end(engine);
	reader_hold(engine);
	reader_flush(engine, &engine->video_queue);
	reader_flush(engine, &engine->audio_queue);
	reader_flush(engine, &engine->subtitle_queue);
	reader_let_go(engine);
	engine->head_sent_bytes = 0;
	DMX_Close(engine->source);
	engine->source = NULL;
}

/**
 * file_seek() - Move the file to the key frame at or before a time.
 * @engine: the engine, a file open
 * @time:   the film's time
 *
 * The reader is held meanwhile, and the packets read ahead are released.
 *
 * Return: 0 or ENGINE_ERR_SEEK.
 */
LONG file_seek(struct engine *engine, VCTime time)
{
	reader_hold(engine);
	reader_flush(engine, &engine->video_queue);
	reader_flush(engine, &engine->audio_queue);
	reader_flush(engine, &engine->subtitle_queue);
	engine->head_sent_bytes = 0;
	LONG error = DMX_Seek(engine->source, &time, DMXSEEK_Before) != 0 ? ENGINE_ERR_SEEK : 0;
	if (error == 0)
	{
		engine->file_end = FALSE;
		reader_reads_reset(engine);
	}
	reader_let_go(engine);
	return error;
}

/**
 * track_fill() - Describe a stream as a track.
 * @engine: the engine
 * @index:  the stream's number
 * @stream: the stream
 * @track:  filled in
 */
static void track_fill(const struct engine *engine, ULONG index, const struct DMXStream *stream, struct engine_track *track)
{
	LONG playing = stream->Kind == DMXK_Audio ? (engine->audio_decoder != NULL ? engine->audio_index : NO_STREAM) : engine->subtitle_index;
	*track = (struct engine_track){
	    .stream = (LONG)index,
	    .kind = stream->Kind == DMXK_Audio ? ENGINE_TRACK_AUDIO : ENGINE_TRACK_SUBTITLE,
	    .channels = stream->Channels,
	    .selected = playing == (LONG)index,
	};
	memcpy(track->language, stream->Language, sizeof(track->language) - 1); /* the last byte stays the NUL */
	memcpy(track->title, stream->Title, sizeof(track->title) - 1);
}

/**
 * track_playable() - Whether a stream is a track the engine can play or show.
 * @stream: the stream
 *
 * Return: TRUE for an audio stream a decoder is named for, and for a subtitle stream of text.
 */
static BOOL track_playable(const struct DMXStream *stream)
{
	if (stream->Kind == DMXK_Audio)
		return stream->Codec != 0;
	return stream->Kind == DMXK_Subtitle && (stream->Codec == DMXS_Text || stream->Codec == DMXS_ASS || stream->Codec == DMXS_MovText);
}

/**
 * file_tracks() - List the file's audio and subtitle tracks.
 * @engine: the engine
 * @tracks: filled in
 * @room:   how many it holds
 *
 * The subtitle file, when one was given, is listed last.
 *
 * Return: how many were listed; 0 without a file.
 */
ULONG file_tracks(struct engine *engine, struct engine_track *tracks, ULONG room)
{
	ULONG count = 0;
	if (engine->source == NULL)
		return 0;
	reader_hold(engine);
	for (ULONG index = 0; index < engine->file_info.Streams && index < SOURCE_STREAMS && count < room; index++)
	{
		struct DMXStream stream = {.Size = sizeof(stream)};
		if (DMX_GetStream(engine->source, index, &stream) == 0 && track_playable(&stream))
			track_fill(engine, index, &stream, &tracks[count++]);
	}
	reader_let_go(engine);
	if (engine->subtitle_name[0] != 0 && count < room)
	{
		tracks[count] = (struct engine_track){
		    .stream = ENGINE_TRACK_FILE,
		    .kind = ENGINE_TRACK_SUBTITLE,
		    .selected = engine->subtitle_index == ENGINE_TRACK_FILE,
		};
		strcpy(tracks[count++].title, "File");
	}
	return count;
}

/**
 * subtitle_select() - Show another subtitle track, or none.
 * @engine: the engine, a file open
 * @track:  the stream's number, ENGINE_TRACK_OFF or ENGINE_TRACK_FILE
 *
 * The reader is held while the stream changes, and the packets read of the
 * one before are released.
 *
 * Return: 0, ENGINE_ERR_NOSTREAM for a stream that is no text subtitle, ENGINE_ERR_FILE when the file no longer reads.
 */
static LONG subtitle_select(struct engine *engine, LONG track)
{
	if (track == ENGINE_TRACK_FILE)
		return file_load_subtitles(engine, (CONST_STRPTR)engine->subtitle_name);
	struct DMXStream stream = {.Size = sizeof(stream)};
	reader_hold(engine);
	LONG error = track >= 0 && (DMX_GetStream(engine->source, (ULONG)track, &stream) != 0 || !track_playable(&stream) ||
	                            stream.Kind != DMXK_Subtitle)
	                 ? ENGINE_ERR_NOSTREAM
	                 : 0;
	if (error == 0)
	{
		reader_flush(engine, &engine->subtitle_queue);
		subtitles_flush(engine->subtitles);
		engine->subtitle_index = track >= 0 ? track : ENGINE_TRACK_OFF;
		engine->subtitle_codec = stream.Codec;
		streams_request(engine);
	}
	reader_let_go(engine);
	return error;
}

/**
 * audio_select() - Play another audio track, or none.
 * @engine: the engine, a file open
 * @track:  the stream's number, or ENGINE_TRACK_OFF
 *
 * The decoder and the sound output are closed and made again for the new
 * track, the reader held meanwhile.
 *
 * Return: 0, ENGINE_ERR_NOSTREAM for a stream that is no audio a decoder is named for.
 */
static LONG audio_select(struct engine *engine, LONG track)
{
	struct DMXStream stream = {.Size = sizeof(stream)};
	reader_hold(engine);
	LONG error = track >= 0 && (DMX_GetStream(engine->source, (ULONG)track, &stream) != 0 || !track_playable(&stream) ||
	                            stream.Kind != DMXK_Audio)
	                 ? ENGINE_ERR_NOSTREAM
	                 : 0;
	if (error == 0)
	{
		audio_close(engine);
		reader_flush(engine, &engine->audio_queue);
		engine->audio_index = track >= 0 ? track : NO_STREAM;
		engine->audio_stream = stream;
		streams_request(engine);
		audio_open(engine);
	}
	reader_let_go(engine);
	return error;
}

/**
 * file_select() - Choose the audio or the subtitle track.
 * @engine: the engine
 * @kind:   audio or subtitle
 * @track:  the track's stream, ENGINE_TRACK_OFF, or for subtitles ENGINE_TRACK_FILE
 *
 * Return: 0, ENGINE_ERR_NOSTREAM or ENGINE_ERR_FILE.
 */
LONG file_select(struct engine *engine, enum engine_track_kind kind, LONG track)
{
	if (engine->source == NULL)
		return ENGINE_ERR_NOSTREAM;
	return kind == ENGINE_TRACK_AUDIO ? audio_select(engine, track) : subtitle_select(engine, track);
}

/**
 * file_chapters() - List the film's chapters.
 * @engine:   the engine
 * @chapters: filled in
 * @room:     how many it holds
 *
 * Return: how many were listed; 0 without a file.
 */
ULONG file_chapters(struct engine *engine, struct engine_chapter *chapters, ULONG room)
{
	ULONG count = 0;
	if (engine->source == NULL)
		return 0;
	reader_hold(engine);
	for (ULONG index = 0; index < engine->file_info.Chapters && count < room; index++)
	{
		struct DMXChapter chapter = {.Size = sizeof(chapter)};
		if (DMX_GetChapter(engine->source, index, &chapter) != 0 || chapter.Start == VC_TIME_UNKNOWN)
			continue;
		chapters[count] = (struct engine_chapter){.start = chapter.Start};
		memcpy(chapters[count].title, chapter.Title, sizeof(chapters[count].title) - 1);
		count++;
	}
	reader_let_go(engine);
	return count;
}
