/*
 * Dulcamara - text as a QR code made on the console (Settings > About: nightly's invitation), with Project Nayuki's
 * QR Code generator (third_party/qrcodegen, MIT; compiled in by src/vendor.c), as Mandrake makes its own.
 * SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * One way to encode, so that the host check (tests/community_check.c) reads back the very symbol the screen shows:
 * medium error correction, raised when the same size holds more; the smallest version the text fits in; the mask
 * qrcodegen finds best.
 */
#ifndef NP_UI_QR_H
#define NP_UI_QR_H

#include "../../third_party/qrcodegen/qrcodegen.h"

#include <stdbool.h>
#include <stdint.h>

/* false when the text does not fit in a QR code; else qr holds the symbol (qrcodegen_getSize, qrcodegen_getModule) */
static inline bool qr_encode(const char *text, uint8_t qr[qrcodegen_BUFFER_LEN_MAX],
                             uint8_t temp[qrcodegen_BUFFER_LEN_MAX])
{
    return qrcodegen_encodeText(text, temp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                                qrcodegen_Mask_AUTO, true);
}

#endif
