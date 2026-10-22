/*
 * FS*Fader - persistent configuration (stored as JSON in the "fsfdata" NVS partition).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define FSF_CONFIG_FORMAT "fsfader-config/1"

#define FSF_FIXTURE_ID_LEN 48
#define FSF_DEVICE_LABEL_LEN 32   /* RDM DEVICE_LABEL max */
#define FSF_SCOPE_LEN 62          /* E1.33 scope (63 bytes padded incl. NUL) */
#define FSF_DOMAIN_LEN 230        /* E1.33 search domain (231 bytes padded incl. NUL) */

typedef enum {
    FSF_PARAM_INTENSITY = 0,
    FSF_PARAM_IRIS,
    FSF_PARAM_ZOOM,
    FSF_PARAM_EDGE,
    FSF_PARAM_FROST,
    FSF_PARAM_COUNT
} fsf_param_t;

typedef enum {
    FSF_IMODE_SCALED = 0,         /* default: output = local x received / full */
    FSF_IMODE_LOCAL_OVERRIDE = 1, /* output = local only */
} fsf_intensity_mode_t;

typedef struct {
    bool dhcp;
    uint32_t ip;       /* network byte order (as esp_ip4_addr_t.addr) */
    uint32_t netmask;
    uint32_t gateway;
} fsf_net_config_t;

typedef struct {
    fsf_net_config_t net;

    fsf_param_t parameter;
    fsf_intensity_mode_t intensity_mode;
    char fixture_id[FSF_FIXTURE_ID_LEN];

    uint16_t src_universe; /* 1..63999 */
    uint16_t src_address;  /* 1..512 */
    uint16_t dst_universe; /* 1..63999 */
    uint16_t dst_address;  /* 1..512 */
    uint8_t dst_priority;  /* 0..200 */

    char device_label[FSF_DEVICE_LABEL_LEN + 1];
    char scope[FSF_SCOPE_LEN + 1];
    char search_domain[FSF_DOMAIN_LEN + 1];
    bool static_broker;          /* false = discover broker via DNS-SD */
    uint32_t static_broker_ip;   /* network byte order */
    uint16_t static_broker_port;
    uint16_t identify_timeout_s; /* E1.37-5 IDENTIFY_TIMEOUT, 0 = identify until turned off */

    uint8_t led_brightness; /* 1..100 % */
} fsf_config_t;

/* Names used in JSON, the web UI and RDM descriptions. */
const char *fsf_param_key(fsf_param_t p);   /* "intensity", "iris", ... */
const char *fsf_param_label(fsf_param_t p); /* "Intensity", "Iris", ... */
bool fsf_param_from_key(const char *key, fsf_param_t *out);

void fsf_config_defaults(fsf_config_t *cfg);

/* Load from flash (falls back to defaults and rewrites them if missing or invalid). */
esp_err_t fsf_config_init(void);

/* Thread-safe copy of the active configuration. */
void fsf_config_get(fsf_config_t *out);

/* Validate, persist and activate a new configuration. Calls registered listeners. */
esp_err_t fsf_config_set(const fsf_config_t *cfg, char *err, size_t err_len);

/*
 * JSON helpers. fsf_config_from_json() applies the keys present in `json` on top of `base`
 * (so a partial document edits only what it contains) and validates the result.
 */
esp_err_t fsf_config_from_json(const char *json, size_t len, const fsf_config_t *base,
                               fsf_config_t *out, char *err, size_t err_len);
char *fsf_config_to_json(const fsf_config_t *cfg); /* caller frees */

/* Erase stored configuration and user fixture profiles. */
esp_err_t fsf_config_factory_reset(void);

typedef void (*fsf_config_listener_t)(const fsf_config_t *old_cfg, const fsf_config_t *new_cfg);
void fsf_config_add_listener(fsf_config_listener_t fn);
