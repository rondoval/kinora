/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui_playlist.c - the films to play, one after the other.
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
	return TRUE;
}

/**
 * playlist_clear() - Forget every film.
 * @playlist: the playlist
 */
void playlist_clear(struct playlist *playlist)
{
	for (ULONG i = 0; i < playlist->count; i++)
		FreeVec(playlist->films[i]);
	playlist->count = 0;
	playlist->current = 0;
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
