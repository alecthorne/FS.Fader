/*
 * FS*Fader - the 44 Hz processing loop: fader + received sACN -> transmitted sACN + pixels.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define FSF_FRAME_RATE_HZ 44

typedef struct {
    uint16_t fader;         /* 0..65535 */
    bool param_present;     /* selected parameter exists in the selected fixture profile */
    uint8_t param_bits;
    uint16_t rx_value;      /* received value of the controlled parameter (native resolution) */
    uint16_t tx_value;      /* transmitted value (native resolution) */
    uint8_t rx_percent;
    uint8_t tx_percent;
    uint8_t left_lit, right_lit;
    uint32_t frames;
    bool identify;
    uint32_t identify_remaining_s; /* 0 when no timeout is running */
} fsf_engine_status_t;

esp_err_t fsf_engine_init(void);
void fsf_engine_get_status(fsf_engine_status_t *out);

/* Identify (RDM IDENTIFY_DEVICE, web UI). Uses the configured IDENTIFY_TIMEOUT when turning on. */
void fsf_identify_set(bool on);
bool fsf_identify_get(void);
