/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A XeleeNet lobby's fighter list (docs/xeleenet-fighter-list-plan.md): which
 * fighters it allows, as a mask with a bit per CharacterKind (ft/forward.h).
 * Zelda and Sheik are one entry: Sheik's bit always follows Zelda's. RULES
 * carries it to every player (net_handshake.c); the CSS dims and refuses the
 * rest (mncharsel.c). */
#ifndef PC_XELEENET_FIGHTERS_H
#define PC_XELEENET_FIGHTERS_H
#include <stdint.h>

#define XN_FIGHTER_ZELDA 0x12
#define XN_FIGHTER_SHEIK 0x13
#define XN_FIGHTERS_ALL ((1u << 26) - 1)
/* The presets follow the Back Rooms 2021 NTSC tier list: High is S to B-,
 * Low is C+ to F. Zelda / Sheik sits with High (Sheik is B; the entry can't
 * allow one without the other). */
#define XN_FIGHTERS_HIGH                                                                         \
    (1u << 0x02 | 1u << 0x09 | 1u << 0x0F | 1u << 0x14 | 1u << XN_FIGHTER_ZELDA |                \
        1u << XN_FIGHTER_SHEIK | 1u << 0x00 | 1u << 0x0C | 1u << 0x0E | 1u << 0x0D | 1u << 0x11 | \
        1u << 0x10 | 1u << 0x07 | 1u << 0x16)
#define XN_FIGHTERS_LOW (XN_FIGHTERS_ALL & ~XN_FIGHTERS_HIGH)

/* The 25 entries in the CSS's own order (mncharsel.c icons[]): rows of 9, 9
 * and 7. */
#define XN_FIGHTER_ENTRIES 25
static const uint8_t xn_fighter_order[XN_FIGHTER_ENTRIES] = {
    0x16, 0x08, 0x07, 0x05, 0x0C, 0x11, 0x01, 0x00, 0x19, /* Dr. Mario .. Ganondorf */
    0x14, 0x02, 0x0B, 0x0E, 0x04, 0x10, 0x12, 0x06, 0x15, /* Falco .. Young Link */
    0x18, 0x0D, 0x0F, 0x0A, 0x03, 0x09, 0x17,             /* Pichu .. Roy */
};

/* A mask as RULES may carry it: the playable bits only, Sheik with Zelda. */
static inline uint32_t xn_fighters_normal(uint32_t mask) {
    mask &= XN_FIGHTERS_ALL;
    mask &= ~(1u << XN_FIGHTER_SHEIK);
    return (mask >> XN_FIGHTER_ZELDA) & 1 ? mask | 1u << XN_FIGHTER_SHEIK : mask;
}

/* How many entries a mask allows (Zelda / Sheik counts once). */
static inline int xn_fighters_count(uint32_t mask) {
    int n = 0;
    for (int i = 0; i < XN_FIGHTER_ENTRIES; i++) {
        n += (mask >> xn_fighter_order[i]) & 1;
    }
    return n;
}

#endif
