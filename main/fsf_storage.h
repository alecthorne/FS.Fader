/*
 * FS*Fader - small JSON document store on a dedicated NVS partition ("fsfdata").
 *
 * NVS gives atomic, power-fail-safe, wear-levelled writes, which matters for a device that may
 * be power-cycled at any time and left running for months.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define FSF_NS_CONFIG "cfg"
#define FSF_NS_FIXTURES "fixtures"

esp_err_t fsf_storage_init(void);

/* Returns a malloc'd NUL-terminated string (caller frees) or NULL if missing. */
char *fsf_storage_read(const char *ns, const char *key, size_t *len_out);
esp_err_t fsf_storage_write(const char *ns, const char *key, const char *data, size_t len);
esp_err_t fsf_storage_erase(const char *ns, const char *key);
esp_err_t fsf_storage_erase_namespace(const char *ns);

typedef bool (*fsf_storage_iter_cb_t)(const char *key, void *ctx); /* return false to stop */
void fsf_storage_foreach(const char *ns, fsf_storage_iter_cb_t cb, void *ctx);
