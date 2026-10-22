#include "fsf_storage.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";
#define PART "fsfdata"

esp_err_t fsf_storage_init(void)
{
    esp_err_t err = nvs_flash_init_partition(PART);
    if (err != ESP_OK) {
        /* Unformatted, from another firmware, or corrupted: start clean rather than boot-loop. */
        ESP_LOGW(TAG, "data partition needs erase (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase_partition(PART));
        err = nvs_flash_init_partition(PART);
    }
    return err;
}

char *fsf_storage_read(const char *ns, const char *key, size_t *len_out)
{
    nvs_handle_t h;
    if (nvs_open_from_partition(PART, ns, NVS_READONLY, &h) != ESP_OK) {
        return NULL;
    }
    size_t len = 0;
    char *buf = NULL;
    if (nvs_get_blob(h, key, NULL, &len) == ESP_OK && len > 0) {
        buf = malloc(len + 1);
        if (buf && nvs_get_blob(h, key, buf, &len) == ESP_OK) {
            buf[len] = '\0';
            if (len_out) {
                *len_out = len;
            }
        } else {
            free(buf);
            buf = NULL;
        }
    }
    nvs_close(h);
    return buf;
}

esp_err_t fsf_storage_write(const char *ns, const char *key, const char *data, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PART, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write %s/%s failed: %s", ns, key, esp_err_to_name(err));
    }
    return err;
}

esp_err_t fsf_storage_erase(const char *ns, const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PART, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t fsf_storage_erase_namespace(const char *ns)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(PART, ns, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

void fsf_storage_foreach(const char *ns, fsf_storage_iter_cb_t cb, void *ctx)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(PART, ns, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (!cb(info.key, ctx)) {
            break;
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
}
