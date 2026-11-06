/*
 * SwanStationPS5 - minimal HTTPS download (libcurl).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef NP_NET_H
#define NP_NET_H

#include "nightshade.h"

typedef enum
{
    NET_OK,
    NET_NOT_FOUND,   /* the server answered 404: the file does not exist */
    NET_UNAVAILABLE, /* no network, DNS, TLS... worth giving up for this session */
} NetResult;

/* How long a download may go without receiving a byte before it is given up. There is no limit on how long the
 * whole download takes: the update's PPSA99010.zip is over 70 MB, and a limit of 25 seconds on all of it (Mandrake's,
 * meant for small files) failed every update slower than 3 MB/s ("The download failed", 0.2.12 and before). */
#ifndef NET_STALL_SECONDS
#define NET_STALL_SECONDS 30
#endif

/* How far a download has got: bytes received and, once the server has said, how many there are (else 0). Called
 * from the downloading thread. */
typedef void (*NetProgress)(void *user, uint64_t done, uint64_t total);

bool net_available(void); /* false when built without libcurl */
/* Downloads `url` to `dest` (written to dest.part, then renamed), following redirects. Blocking. progress (may be
 * NULL) hears how far it has got. When it fails, why (may be NULL) gets a short reason for the user, such as
 * "nothing arrived for 30 seconds" or "the server answered 403". */
NetResult net_download(const char *url, const char *dest, NetProgress progress, void *user, char *why,
                       size_t why_size);

/* GET (post_data NULL) or POST `url`; the body comes back in *body (malloc'd,
 * NUL-terminated, free() it). Returns the HTTP status, or a negative value
 * when the request could not be made at all. Blocking. */
int net_request(const char *url, const char *post_data, const char *content_type,
                const char *user_agent, char **body, size_t *body_length);

#endif
