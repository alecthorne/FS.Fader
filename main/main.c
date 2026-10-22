/*
 * FS*Fader firmware for the M5Stack Unit PoE-P4 + Unit Fader.
 *
 * Receives sACN (ANSI E1.31), replaces one fixture parameter with the local fader value and
 * re-transmits the fixture's footprint on another universe at 44 Hz. Configurable from a web
 * page and over RDMnet (ANSI E1.33).
 */
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "fsf_config.h"
#include "fsf_engine.h"
#include "fsf_fixtures.h"
#include "fsf_io.h"
#include "fsf_net.h"
#include "fsf_rdm.h"
#include "fsf_sacn.h"
#include "fsf_storage.h"
#include "fsf_web.h"

static const char *TAG = "main";

/* After an OTA update the new image must prove itself before rollback is cancelled. */
static void confirm_firmware_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    fsf_engine_status_t es;
    fsf_engine_get_status(&es);
    if (es.frames > 1000) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "firmware marked valid");
    } else {
        ESP_LOGE(TAG, "engine not running after 60 s; rebooting to previous firmware");
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "FS*Fader %s starting (reset reason %d)", esp_app_get_description()->version,
             esp_reset_reason());

    /* Default NVS partition is used by the PHY/system; FS*Fader data lives in "fsfdata". */
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(fsf_storage_init());
    ESP_ERROR_CHECK(fsf_fixtures_init());
    ESP_ERROR_CHECK(fsf_config_init());

    if (fsf_io_init() != ESP_OK) {
        ESP_LOGE(TAG, "local I/O init failed; continuing without fader/pixels");
    }
    ESP_ERROR_CHECK(fsf_net_init());

    uint8_t cid[16];
    fsf_device_cid(1, cid);
    ESP_ERROR_CHECK(fsf_sacn_init(cid, fsf_net_hostname()));
    ESP_ERROR_CHECK(fsf_engine_init());
    ESP_ERROR_CHECK(fsf_web_init());
    ESP_ERROR_CHECK(fsf_rdm_init());

    xTaskCreate(confirm_firmware_task, "fw_confirm", 3072, NULL, 1, NULL);
    ESP_LOGI(TAG, "running");
}
