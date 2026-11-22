/*
 * Dulcamara - adding games from links in the interface (links.h, docs/game-links.md): Settings > Library's two rows
 * start the queue and show how it goes; when it ends, the library is looked at again and the new game is selected,
 * or why it is not shown is said. The URL is typed on Dulcamara's keyboard (link_prompt.c).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DULCAMARA_LINKS_UI_H
#define DULCAMARA_LINKS_UI_H

#include <stdbool.h>
#include <stdint.h>

/* Settings > Library > Add games from links.txt: the lines of /data/dulcamara/links.txt not done yet. */
void links_ui_from_file(void);
void links_ui_install_shelf(void);
/* A typed link. */
void links_ui_from_url(const char *url);
/* While the queue runs: Cross on either row cancels it. */
bool links_ui_busy(void);
void links_ui_cancel(void);
/* The rows' value: how the queue goes, "" when idle. */
const char *links_ui_status(void);
/* Each frame, after the library's publication (main.c): the end of a queue, and the new game's selection. */
void links_ui_tick(void);

/* The link typed on Dulcamara's keyboard, over Settings. */
void link_prompt_open(void);
bool link_prompt_input(uint32_t pressed); /* while it is open it takes every press */
void link_prompt_draw(void);

#endif
