/* SPDX-License-Identifier: MPL-2.0 */
/*
 * Kinora - a film player for AmigaOS 3.2 on a PiStorm with Emu68.
 *
 *   Kinora FILE/M
 *
 * The files are played one after the other.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos/dos.h>
#include <dos/rdargs.h>
#include <exec/libraries.h>
#include <workbench/startup.h>
#include <workbench/workbench.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>

#include "engine/clock.h"
#include "gui/gui.h"

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define PATH_SIZE 512 /* a film's path, with its NUL */

unsigned long __stack = 65536; /* ReAction and the ASL requester want more than a shell's default */

static const char version_cookie[] __attribute__((used)) = "$VER: Kinora " KINORA_VERSION_TEXT " " KINORA_DATE;
static const char template[] = "FILE/M";

enum
{
	ARG_FILE,
	ARG_COUNT
};

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Library *GadToolsBase;
struct Library *AslBase;
struct Library *P96Base;
struct Library *VideoCodecBase;
struct Library *DemuxerBase;
struct Library *AudioCodecBase;
struct Library *IFFParseBase;
extern struct Library *WindowBase;
extern struct Library *LayoutBase;
extern struct Library *SpaceBase;
extern struct Library *ButtonBase;
extern struct Library *SliderBase;
extern struct Library *StringBase;

/* A library the player needs */
struct needed_library
{
	struct Library **base; /* where its base goes */
	const char *name;      /* its name */
	ULONG version;         /* the lowest version that will do */
};

static const struct needed_library libraries[] = {
    {(struct Library **)&IntuitionBase, "intuition.library", 44},
    {(struct Library **)&GfxBase, "graphics.library", 40},
    {&GadToolsBase, "gadtools.library", 40},
    {&AslBase, "asl.library", 40},
    {&IFFParseBase, "iffparse.library", 39},
    {&P96Base, "Picasso96API.library", 2},
    {&WindowBase, "window.class", 44},
    {&LayoutBase, "gadgets/layout.gadget", 44},
    {&SpaceBase, "gadgets/space.gadget", 44},
    {&ButtonBase, "gadgets/button.gadget", 44},
    {&SliderBase, "gadgets/slider.gadget", 44},
    {&StringBase, "gadgets/string.gadget", 44},
    {&VideoCodecBase, "videocodec.library", 0},
    {&DemuxerBase, "demuxer.library", 0},
    {&AudioCodecBase, "audiocodec.library", 0},
};

/**
 * libraries_close() - Close the libraries that are open.
 */
static void libraries_close(void)
{
	for (ULONG i = 0; i < ARRAY_COUNT(libraries); i++)
	{
		if (*libraries[i].base != NULL)
			CloseLibrary(*libraries[i].base);
		*libraries[i].base = NULL;
	}
}

/**
 * libraries_open() - Open every library the player needs.
 *
 * Return: TRUE, FALSE when one is missing; its name is printed.
 */
static BOOL libraries_open(void)
{
	for (ULONG i = 0; i < ARRAY_COUNT(libraries); i++)
	{
		*libraries[i].base = OpenLibrary((CONST_STRPTR)libraries[i].name, libraries[i].version);
		if (*libraries[i].base == NULL)
		{
			printf("Kinora needs %s version %lu\n", libraries[i].name, (unsigned long)libraries[i].version);
			return FALSE;
		}
	}
	return TRUE;
}

/**
 * workbench_setup() - What the player was started with from the Workbench.
 * @startup: the Workbench's start-up message
 * @setup:   filled in: the icons the program was started with as its films
 *
 * The films' paths stay allocated for the program's life.
 */
static void workbench_setup(const struct WBStartup *startup, struct gui_setup *setup)
{
	ULONG count = startup->sm_NumArgs > 0 ? (ULONG)startup->sm_NumArgs - 1 : 0, taken = 0;
	CONST_STRPTR *files = calloc(count + 1, sizeof(*files));
	for (ULONG i = 0; files != NULL && i < count; i++)
	{
		const struct WBArg *film = &startup->sm_ArgList[i + 1];
		char *path = malloc(PATH_SIZE);
		if (path != NULL && NameFromLock(film->wa_Lock, (STRPTR)path, PATH_SIZE) && AddPart((STRPTR)path, (CONST_STRPTR)film->wa_Name, PATH_SIZE))
			files[taken++] = (CONST_STRPTR)path;
	}
	setup->files = files;
}

/**
 * main() - Read the arguments, open the libraries and run the player.
 * @argc: the argument count; 0 when started from the Workbench
 * @argv: the Workbench's start-up message then; else the arguments, read with ReadArgs() instead
 *
 * Return: RETURN_OK, or RETURN_FAIL for bad arguments, a missing library or a window that does not open.
 */
int main(int argc, char **argv)
{
	LONG args[ARG_COUNT] = {0};
	struct RDArgs *parsed = argc != 0 ? ReadArgs((CONST_STRPTR)template, args, NULL) : NULL;
	if (argc != 0 && parsed == NULL)
	{
		PrintFault(IoErr(), (CONST_STRPTR) "Kinora");
		return RETURN_FAIL;
	}
	struct gui_setup setup = {0};
	int result = RETURN_FAIL;
	if (libraries_open() && clock_open())
	{
		if (argc == 0)
			workbench_setup((const struct WBStartup *)argv, &setup);
		else
			setup.files = (CONST_STRPTR *)args[ARG_FILE];
		result = gui_run(&setup);
	}
	clock_close();
	libraries_close();
	if (parsed != NULL)
		FreeArgs(parsed);
	return result;
}
