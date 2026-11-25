/* SPDX-License-Identifier: MPL-2.0 */
/*
 * gui.h - the player's front: one window with the picture above and the
 * controls below, the rest in the menus (docs/architecture.md section 11).
 */
#ifndef KINORA_GUI_H
#define KINORA_GUI_H

#include <exec/types.h>

/* What the player was started with */
struct gui_setup
{
	CONST_STRPTR *files; /* the films to play, a NULL at their end; NULL for none */
};

int gui_run(const struct gui_setup *setup);

#endif /* KINORA_GUI_H */
