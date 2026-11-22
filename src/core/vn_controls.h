/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DULCAMARA_VN_CONTROLS_H
#define DULCAMARA_VN_CONTROLS_H

#include "../nightshade.h"

/* Physical aliases feed the same per-game bindings as the original face buttons. */
static inline void vn_one_hand_input(PadState *pad, int layout)
{
    if (layout == VN_LEFT_HAND)
    {
        if (pad->buttons & BIT(BTN_L1))
            pad->buttons |= BIT(BTN_CROSS);
        if ((pad->buttons & BIT(BTN_L2)) || pad->l2 > 160)
            pad->buttons |= BIT(BTN_CIRCLE);
        pad->buttons &= ~(BIT(BTN_L1) | BIT(BTN_L2));
        pad->l2 = 0;
    }
    else if (layout == VN_RIGHT_HAND)
    {
        if (pad->buttons & BIT(BTN_R1))
            pad->buttons |= BIT(BTN_CROSS);
        pad->buttons &= ~BIT(BTN_R1);
        if (pad->ry < -16000) pad->buttons |= BIT(BTN_UP);
        if (pad->ry > 16000) pad->buttons |= BIT(BTN_DOWN);
        if (pad->rx < -16000) pad->buttons |= BIT(BTN_LEFT);
        if (pad->rx > 16000) pad->buttons |= BIT(BTN_RIGHT);
    }
}
#endif
