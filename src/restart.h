/* Atomic restart intent; consume only after an accepted matching launch. */
/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NP_RESTART_H
#define NP_RESTART_H
#include <stdbool.h>
#include <stddef.h>
bool restart_write(const char *path, const char *game_id);
bool restart_read(const char *path, char *game_id, size_t size);
bool restart_acknowledge(const char *path, const char *game_id);
#endif
