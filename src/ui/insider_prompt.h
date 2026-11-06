/*
 * Dulcamara - the insider code typed on Dulcamara's own keyboard (keyboard.h), over Settings: when Test builds is
 * switched on with no code stored, and from Settings > System > Insider code (insider.h). The console's own keyboard
 * can't be used in Dulcamara. Done stores the code (exactly 8 of its characters once spaces and hyphens are gone, as
 * the service reads it; anything else is refused and nothing stored), Done with nothing
 * typed leaves no code, Circle closes it and changes nothing.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef DULCAMARA_INSIDER_PROMPT_H
#define DULCAMARA_INSIDER_PROMPT_H

#include <stdbool.h>
#include <stdint.h>

void insider_prompt_open(void);
/* While it is open it takes every press: true, and Settings gets none of them. False when it isn't open. */
bool insider_prompt_input(uint32_t pressed);
void insider_prompt_draw(void);

#endif
