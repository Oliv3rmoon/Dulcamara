/*
 * Dulcamara - the settings file (settings.ini in the data folder) and the data folder's layout.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_CONFIG_H
#define NP_CONFIG_H

#include "nightshade.h"

void config_defaults(Settings *s);
/* Reads the file over the defaults; a missing file or key keeps the default. */
void config_load(Settings *s, const char *path);
bool config_save(const Settings *s, const char *path);
/* The data folder's paths below root. */
void config_paths(Paths *p, const char *root);
/* The game buttons back to the core's defaults (cores.h). */
void config_default_buttons(Settings *s, int core);

#endif
