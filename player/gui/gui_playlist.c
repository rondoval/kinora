/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_playlist.c - the films to play, one after the other.
 *
 * The open film is the one at playlist.current.  Taken out of the list (by
 * Remove or Clear) it plays on: current then stands where it was, so the
 * film there now is the next one and the film before it the previous one.
 * Every change counts in playlist.changes, by which the playlist window
 * knows to show it anew.
 */
#include <string.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "gui_private.h"

/**
 * playlist_add() - Add a film behind the others.
 * @playlist: the playlist
 * @name:     the film's path; copied
 *
 * Return: TRUE, FALSE when the playlist is full or there is no memory.
 */
BOOL playlist_add(struct playlist *playlist, CONST_STRPTR name)
{
	if (playlist->count == PLAYLIST_FILMS)
		return FALSE;
	char *copy = AllocVec(strlen((const char *)name) + 1, MEMF_ANY);
	if (copy == NULL)
		return FALSE;
	strcpy(copy, (const char *)name);
	playlist->films[playlist->count++] = copy;
	playlist->changes++;
	return TRUE;
}

/**
 * playlist_clear() - Forget every film; the open one, if any, is no longer listed.
 * @playlist: the playlist
 */
void playlist_clear(struct playlist *playlist)
{
	for (ULONG i = 0; i < playlist->count; i++)
		FreeVec(playlist->films[i]);
	playlist->count = 0;
	playlist->current = 0;
	playlist->open_gone = TRUE;
	playlist->changes++;
}

/**
 * playlist_choose() - Make a film the open one.
 * @playlist: the playlist
 * @index:    the film, below the playlist's count
 */
void playlist_choose(struct playlist *playlist, ULONG index)
{
	playlist->current = index;
	playlist->open_gone = FALSE;
	playlist->changes++;
}

/**
 * playlist_remove() - Take a film out.
 * @playlist: the playlist
 * @index:    the film; one past the end does nothing
 */
void playlist_remove(struct playlist *playlist, ULONG index)
{
	if (index >= playlist->count)
		return;
	FreeVec(playlist->films[index]);
	playlist->count--;
	memmove(&playlist->films[index], &playlist->films[index + 1], (playlist->count - index) * sizeof(playlist->films[0]));
	if (index < playlist->current)
		playlist->current--;
	else if (index == playlist->current)
		playlist->open_gone = TRUE; /* the open film went: the one after it takes its place as the next */
	playlist->changes++;
}

/**
 * playlist_move() - Swap a film with the one before or after it.
 * @playlist: the playlist
 * @index:    the film
 * @step:     -1 for up, 1 for down
 *
 * The open film's place goes with it.
 *
 * Return: where the film is now; @index when it cannot move that way.
 */
ULONG playlist_move(struct playlist *playlist, ULONG index, LONG step)
{
	ULONG other = index + (ULONG)step;
	if (index >= playlist->count || other >= playlist->count)
		return index;
	char *film = playlist->films[index];
	playlist->films[index] = playlist->films[other];
	playlist->films[other] = film;
	if (playlist->current == index)
		playlist->current = other;
	else if (playlist->current == other)
		playlist->current = index;
	playlist->changes++;
	return other;
}

/**
 * playlist_next() - The film after the open one.
 * @playlist: the playlist
 *
 * Return: its place; the playlist's count or more when there is none.
 */
ULONG playlist_next(const struct playlist *playlist)
{
	return playlist->open_gone ? playlist->current : playlist->current + 1;
}

/**
 * playlist_previous() - The film before the open one.
 * @playlist: the playlist
 *
 * Return: its place; the playlist's count or more when there is none (it wraps below 0).
 */
ULONG playlist_previous(const struct playlist *playlist)
{
	return playlist->current - 1;
}

/**
 * playlist_name() - A film's name without its drawers, for a list.
 * @playlist: the playlist
 * @index:    the film, below the playlist's count
 *
 * Return: the name.
 */
CONST_STRPTR playlist_name(const struct playlist *playlist, ULONG index)
{
	return FilePart((CONST_STRPTR)playlist->films[index]);
}
