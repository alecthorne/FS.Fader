/*
 * FS*Fader - fixture profiles.
 *
 * A profile tells the firmware how big the fixture's DMX footprint is and where each locally
 * controllable parameter lives inside it. Built-in profiles are compiled in from
 * the JSON files in fixtures/; user profiles are uploaded as JSON and stored in flash.
 *
 * Profile JSON ("fsfader-profile/1"):
 * {
 *   "format": "fsfader-profile/1",
 *   "id": "martin-mac-encore-performance",          // [a-z0-9._-], unique
 *   "manufacturer": "Martin", "model": "MAC Encore Performance", "mode": "Standard",
 *   "footprint": 38,                                 // 1..512 DMX addresses
 *   "parameters": {                                  // any subset of the five
 *     "intensity": { "offset": 2,  "bits": 16 },     // offset = 1-based channel in footprint
 *     "iris":      { "offset": 15, "bits": 8, "range": [0, 255] }
 *   },
 *   "notes": "free text", "source": "where the data came from"
 * }
 * "range" is optional: [value at fader bottom, value at fader top] in the parameter's own
 * resolution (0-255 or 0-65535). It defaults to the full range and may be reversed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "fsf_config.h"

#define FSF_PROFILE_FORMAT "fsfader-profile/1"
#define FSF_MAX_USER_FIXTURES 64
#define FSF_MAX_FIXTURES (FSF_MAX_USER_FIXTURES + 16)

typedef struct {
    bool present;
    uint16_t offset; /* 1-based, within footprint */
    uint8_t bits;    /* 8 or 16 */
    uint16_t from;   /* native value at fader bottom */
    uint16_t to;     /* native value at fader top */
} fsf_fixture_param_t;

typedef struct {
    char id[FSF_FIXTURE_ID_LEN];
    char manufacturer[48];
    char model[64];
    char mode[48];
    uint16_t footprint;
    fsf_fixture_param_t params[FSF_PARAM_COUNT];
    bool builtin;
} fsf_fixture_t;

esp_err_t fsf_fixtures_init(void);

size_t fsf_fixture_count(void);
bool fsf_fixture_at(size_t index, fsf_fixture_t *out); /* stable order: built-ins first */
bool fsf_fixture_get(const char *id, fsf_fixture_t *out);
int fsf_fixture_index(const char *id); /* -1 if missing */

/* Parse + validate profile JSON (a single object). */
esp_err_t fsf_fixture_parse(const char *json, size_t len, fsf_fixture_t *out, char *err, size_t err_len);

/* Install or replace a user profile (built-ins cannot be replaced). */
esp_err_t fsf_fixture_install(const char *json, size_t len, char *id_out, size_t id_len, char *err,
                              size_t err_len);
esp_err_t fsf_fixture_delete(const char *id, char *err, size_t err_len);

/* Original JSON text of a profile (malloc'd; caller frees). */
char *fsf_fixture_json(const char *id);

/* "Manufacturer Model (Mode)" */
void fsf_fixture_display_name(const fsf_fixture_t *fx, char *buf, size_t len);
