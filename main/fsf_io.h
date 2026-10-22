/*
 * FS*Fader - local hardware: Unit Fader potentiometer + 14 SK6812 pixels, on-board RGB LED,
 * user button.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define FSF_PIXELS 14

typedef struct {
    uint8_t r, g, b;
} fsf_rgb_t;

esp_err_t fsf_io_init(void);

/* Sample the fader (call at the frame rate). Returns a filtered, de-jittered 0..65535. */
uint16_t fsf_fader_sample(void);
uint16_t fsf_fader_last(void);
uint16_t fsf_fader_raw(void); /* last averaged ADC reading, 0..4095 */

/* Write all 14 pixels (index 0 = pixel 1). Brightness 1..100 %. */
void fsf_pixels_show(const fsf_rgb_t px[FSF_PIXELS], uint8_t brightness);

/* On-board RGB LED (on/off per channel). */
void fsf_status_led(bool r, bool g, bool b);

/* True while the user button is held. */
bool fsf_button_pressed(void);
