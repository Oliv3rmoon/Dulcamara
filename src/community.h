/*
 * Dulcamara - nightly, the community on Discord: Settings > About > Join the community shows its name and short link,
 * and Cross a QR code of its invitation; the empty library mentions it once (ui/shelf.c, Settings.community_seen).
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The name and the link are never translated. tests/test_community.py builds tests/community_check.c with these and
 * src/ui/qr.h, and reads the QR code back: it must open COMMUNITY_URL and nothing else.
 */
#ifndef NP_COMMUNITY_H
#define NP_COMMUNITY_H

#define COMMUNITY_NAME "nightly"                       /* the Discord server, by its own name */
#define COMMUNITY_URL "https://discord.gg/pSqgRU3YP9"  /* its invitation: what the QR code opens */
#define COMMUNITY_LINK "discord.gg/pSqgRU3YP9"         /* the same, as it is written on screen */

#endif
