/*
 * Dulcamara - what links.c, link_fetch.c and link_unpack.c share (links.h is their interface).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_LINK_INTERNAL_H
#define NP_LINK_INTERNAL_H

#include "links.h"

/* Sets r's error and message; returns false. */
bool link_fail(LinkResult *r, LinkError error, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void link_progress(const LinkObserver *o, LinkStage stage, uint64_t done, uint64_t total);
bool link_cancelled(const LinkObserver *o);
/* path and all below it removed; links are removed, never followed */
bool link_remove_tree(const char *path);
/* url without its query (which may hold a token) or a user:password@, for the log and messages */
void link_url_for_log(const char *url, char *out, size_t size);
/* where url's host starts: after the scheme's "://" and any user:password@ */
const char *link_url_host(const char *url);

#endif
