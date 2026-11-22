/*
 * Dulcamara - the update's popup, as Nightshade offers its own: when the app opens and finds a newer release,
 * "Update available" (or "Test build available") with Update now and Later; then how far the download has got;
 * then that it is installed (Close Dulcamara, Later), or why it failed (Try again, Later).
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DULCAMARA_UPDATE_POPUP_H
#define DULCAMARA_UPDATE_POPUP_H

#include <stdbool.h>
#include <stdint.h>

/* Shows the popup for the update update.c offers (UPDATE_AVAILABLE). */
void update_popup_open(void);
bool update_popup_is_open(void);
/* While it is open it takes every press (and clears *pressed); false when it isn't open. */
bool update_popup_input(uint32_t *pressed);
void update_popup_draw(void);

#endif
