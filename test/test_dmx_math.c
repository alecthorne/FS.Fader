/* Host unit tests for the engine's value math: cc -I../main test_dmx_math.c -o t && ./t */
#include <assert.h>
#include <stdio.h>
#include "fsf_dmx_math.h"

static int failures;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

int main(void)
{
    /* Fader mapping over the full 8- and 16-bit ranges */
    CHECK(lerp16(0, 255, 0) == 0);
    CHECK(lerp16(0, 255, 65535) == 255);
    CHECK(lerp16(0, 255, 32768) == 128);
    CHECK(lerp16(0, 65535, 65535) == 65535);
    CHECK(lerp16(0, 65535, 12345) == 12345);
    /* Reversed range (e.g. iris profile with range [255, 0]) */
    CHECK(lerp16(255, 0, 0) == 255);
    CHECK(lerp16(255, 0, 65535) == 0);
    /* Limited range (MAC Ultra iris 0..51400) */
    CHECK(lerp16(0, 51400, 65535) == 51400);

    /* Scaled intensity: output = received x fader */
    CHECK(lerp16(0, 200, 65535) == 200);   /* fader full -> received level */
    CHECK(lerp16(0, 200, 32768) == 100);   /* half -> half of received */
    CHECK(lerp16(0, 0, 65535) == 0);       /* console at zero -> zero */
    for (unsigned rx = 0; rx <= 255; rx++) {
        for (unsigned f = 0; f <= 65535; f += 257) {
            uint16_t tx = lerp16(0, rx, f);
            CHECK(tx <= rx); /* never exceeds the received level */
        }
    }

    /* 16-bit read/write round trip */
    uint8_t dmx[4] = {0};
    write_native(dmx, 1, 16, 0xABCD);
    CHECK(dmx[1] == 0xAB && dmx[2] == 0xCD);
    CHECK(read_native(dmx, 1, 16) == 0xABCD);
    write_native(dmx, 0, 8, 0x7F);
    CHECK(read_native(dmx, 0, 8) == 0x7F);

    /* LED bar: 7 pixels per side, any non-zero value lights at least one */
    CHECK(lit_count(position_permille(0, 0, 255)) == 0);
    CHECK(lit_count(position_permille(1, 0, 255)) == 1);
    CHECK(lit_count(position_permille(255, 0, 255)) == 7);
    CHECK(lit_count(position_permille(128, 0, 255)) == 4);
    CHECK(lit_count(position_permille(65535, 0, 51400)) == 7); /* clamped above range */
    CHECK(lit_count(position_permille(0, 255, 0)) == 7);        /* reversed range */
    for (int pm = 0; pm <= 1000; pm++) {
        int n = lit_count(pm);
        CHECK(n >= 0 && n <= SIDE_PIXELS);
    }

    if (failures == 0) {
        printf("all DMX math tests passed\n");
    }
    return failures ? 1 : 0;
}
