/*
 * Dulcamara - nightly's invitation (src/community.h) and the QR code Settings > About draws of it, made on the host.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Built and run by tests/test_community.py with third_party/qrcodegen: prints the name, the short link and the
 * address, then the symbol qr_encode (src/ui/qr.h, as draw_url_qr in src/ui/settings.c makes it on the console)
 * gives for COMMUNITY_URL, one row of modules per line, 1 for dark. The test reads that symbol back with a reader of
 * its own.
 */
#include "community.h"
#include "ui/qr.h"

#include <stdio.h>

int main(void)
{
    static uint8_t qr[qrcodegen_BUFFER_LEN_MAX], temp[qrcodegen_BUFFER_LEN_MAX];
    printf("name %s\nlink %s\nurl %s\n", COMMUNITY_NAME, COMMUNITY_LINK, COMMUNITY_URL);
    if (!qr_encode(COMMUNITY_URL, qr, temp))
    {
        printf("FAIL: qr_encode could not make a QR code of %s\n", COMMUNITY_URL);
        return 1;
    }
    const int size = qrcodegen_getSize(qr);
    printf("qr %d\n", size);
    for (int y = 0; y < size; ++y)
    {
        for (int x = 0; x < size; ++x)
            putchar(qrcodegen_getModule(qr, x, y) ? '1' : '0');
        putchar('\n');
    }
    return 0;
}
