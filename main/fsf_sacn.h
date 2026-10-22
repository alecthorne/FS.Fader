/*
 * FS*Fader - ANSI E1.31-2018 (Streaming ACN) receiver and transmitter.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define FSF_SACN_PORT 5568
#define FSF_SACN_MAX_SOURCES 8
#define FSF_SACN_SOURCE_NAME_LEN 64

typedef struct {
    bool have_data;      /* at least one valid packet seen since boot (data is held after loss) */
    bool source_present; /* a source is currently streaming (not timed out / terminated) */
    uint8_t priority;    /* winning priority */
    uint8_t num_sources;
    uint32_t packets;    /* valid packets accepted for the source universe */
    uint32_t seq_dropped;
    int64_t last_rx_us;
} fsf_sacn_rx_status_t;

typedef struct {
    char name[FSF_SACN_SOURCE_NAME_LEN];
    uint8_t cid[16];
    uint8_t priority;
    uint32_t ip; /* network byte order */
    uint32_t age_ms;
} fsf_sacn_source_info_t;

esp_err_t fsf_sacn_init(const uint8_t cid[16], const char *source_name);

/* Latest merged 512 slots for the configured source universe. */
void fsf_sacn_rx_snapshot(uint8_t out[512], fsf_sacn_rx_status_t *status);
int fsf_sacn_rx_sources(fsf_sacn_source_info_t *out, int max);

/* Send one E1.31 data packet (start code 0, 512 slots). */
void fsf_sacn_tx_send(uint16_t universe, uint8_t priority, const uint8_t data[512]);
/* Send the three stream-terminated packets required when a universe stops being sourced. */
void fsf_sacn_tx_terminate(uint16_t universe, uint8_t priority);
/* Send an E1.31 Universe Discovery packet listing `universe` (call every 10 s). */
void fsf_sacn_tx_discovery(uint16_t universe);
