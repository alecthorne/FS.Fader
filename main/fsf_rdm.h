/*
 * FS*Fader - ANSI E1.33 (RDMnet) Device + LLRP Target, built on ETC's RDMnet library, and the
 * RDM responder that exposes the FS*Fader configuration.
 *
 * Manufacturer-specific PIDs (all on the root device, sub-device 0):
 *   0x8000 FSF_CONTROLLED_PARAMETER     u8  GET/SET 0=Intensity 1=Iris 2=Zoom 3=Edge 4=Frost
 *   0x8001 FSF_INTENSITY_MODE           u8  GET/SET 0=Scaled 1=Local Override
 *   0x8002 FSF_SOURCE_UNIVERSE          u16 GET/SET 1-63999
 *   0x8003 FSF_SOURCE_ADDRESS           u16 GET/SET 1-512
 *   0x8004 FSF_DEST_UNIVERSE            u16 GET/SET 1-63999
 *   0x8005 FSF_DEST_ADDRESS             u16 GET/SET 1-512
 *   0x8006 FSF_DEST_PRIORITY            u8  GET/SET 0-200
 *   0x8007 FSF_FIXTURE_PROFILE          GET -> u8 current (1-based), u8 count; SET u8 index
 *   0x8008 FSF_FIXTURE_PROFILE_DESCRIPTION  GET u8 index -> u8 index, u16 footprint, ASCII name
 *   0x8009 FSF_FIXTURE_PROFILE_ID       ASCII GET/SET (profile id, e.g. "generic-dimmer-8bit")
 *   0x800A FSF_LOCAL_LEVEL              u16 GET (fader position 0-65535)
 *   0x800B FSF_LED_BRIGHTNESS           u8  GET/SET 1-100
 *   0x800C FSF_PARAMETER_VALUES         GET -> u16 received, u16 transmitted (native resolution)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define FSF_PID_CONTROLLED_PARAMETER 0x8000
#define FSF_PID_INTENSITY_MODE 0x8001
#define FSF_PID_SOURCE_UNIVERSE 0x8002
#define FSF_PID_SOURCE_ADDRESS 0x8003
#define FSF_PID_DEST_UNIVERSE 0x8004
#define FSF_PID_DEST_ADDRESS 0x8005
#define FSF_PID_DEST_PRIORITY 0x8006
#define FSF_PID_FIXTURE_PROFILE 0x8007
#define FSF_PID_FIXTURE_PROFILE_DESCRIPTION 0x8008
#define FSF_PID_FIXTURE_PROFILE_ID 0x8009
#define FSF_PID_LOCAL_LEVEL 0x800A
#define FSF_PID_LED_BRIGHTNESS 0x800B
#define FSF_PID_PARAMETER_VALUES 0x800C

typedef struct {
    bool running;      /* RDMnet stack started (requires an IP address) */
    bool connected;    /* connected to a broker */
    char broker[48];   /* "ip:port" */
    char uid[16];      /* "7FF0:1A2B3C4D" */
    char cid[40];
    uint16_t unhealthy_tcp_events;
    char last_event[96];
} fsf_rdm_status_t;

esp_err_t fsf_rdm_init(void);
void fsf_rdm_get_status(fsf_rdm_status_t *out);

/* Deterministic per-device UUIDs (index 0 = RDMnet component CID, 1 = sACN source CID). */
void fsf_device_cid(uint32_t index, uint8_t out[16]);
