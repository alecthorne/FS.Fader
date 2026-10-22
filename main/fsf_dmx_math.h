/*
 * FS*Fader - pure DMX value helpers shared by the engine (and host unit tests in test/).
 */
#pragma once

#include <stdint.h>

#define SIDE_PIXELS 7

static inline uint16_t read_native(const uint8_t *dmx, int index, uint8_t bits)
{
    if (bits == 16) {
        return (uint16_t)((dmx[index] << 8) | dmx[index + 1]);
    }
    return dmx[index];
}

static inline void write_native(uint8_t *dmx, int index, uint8_t bits, uint16_t v)
{
    if (bits == 16) {
        dmx[index] = v >> 8;
        dmx[index + 1] = v & 0xFF;
    } else {
        dmx[index] = (uint8_t)v;
    }
}

/* a + (b - a) * frac, frac in 0..65535, rounded */
static inline uint16_t lerp16(uint16_t a, uint16_t b, uint16_t frac)
{
    int32_t d = (int32_t)b - (int32_t)a;
    int64_t v = (int64_t)a * 65535 + (int64_t)d * frac;
    return (uint16_t)((v + 32767) / 65535);
}

/* Position of v between from and to, 0..1000 (per mille), clamped. */
static inline int position_permille(uint16_t v, uint16_t from, uint16_t to)
{
    int32_t span = (int32_t)to - (int32_t)from;
    int32_t pos = (int32_t)v - (int32_t)from;
    int32_t pm = (int32_t)(((int64_t)pos * 1000) / span); /* sign handles reversed ranges */
    return pm < 0 ? 0 : (pm > 1000 ? 1000 : pm);
}

static inline int lit_count(int permille)
{
    if (permille <= 0) {
        return 0;
    }
    int n = (permille * SIDE_PIXELS + 500) / 1000;
    return n < 1 ? 1 : n; /* any non-zero level lights at least one pixel */
}

