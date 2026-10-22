#include "fsf_io.h"

#include <string.h>
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "io";

#define ESP_RETURN_ON_ERROR_LOG(x)                                             \
    do {                                                                       \
        esp_err_t e_ = (x);                                                    \
        if (e_ != ESP_OK) {                                                    \
            ESP_LOGE(TAG, "%s failed: %s", #x, esp_err_to_name(e_));          \
            return e_;                                                         \
        }                                                                      \
    } while (0)

/* ---- Fader ---- */
#define FADER_SAMPLES 8
#define FADER_HYSTERESIS 96 /* in 0..65535 units (~0.15 %) */

static adc_oneshot_unit_handle_t s_adc;
static adc_channel_t s_adc_chan;
static bool s_adc_ok;
static int32_t s_filt = -1;  /* raw x16 */
static uint16_t s_raw;
static uint16_t s_out;

/* ---- Pixels ---- */
static rmt_channel_handle_t s_rmt;
static rmt_encoder_handle_t s_enc;
static uint8_t s_grb[FSF_PIXELS * 3];

static const int s_led_pins[3] = {CONFIG_FSF_STATUS_LED_R_GPIO, CONFIG_FSF_STATUS_LED_G_GPIO,
                                  CONFIG_FSF_STATUS_LED_B_GPIO};

static esp_err_t fader_init(void)
{
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(CONFIG_FSF_FADER_GPIO, &unit, &s_adc_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d is not an ADC pin", CONFIG_FSF_FADER_GPIO);
        return err;
    }
    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = unit, .ulp_mode = ADC_ULP_MODE_DISABLE};
    ESP_RETURN_ON_ERROR_LOG(adc_oneshot_new_unit(&ucfg, &s_adc));
    adc_oneshot_chan_cfg_t ccfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    err = adc_oneshot_config_channel(s_adc, s_adc_chan, &ccfg);
    s_adc_ok = err == ESP_OK;
    ESP_LOGI(TAG, "fader on GPIO%d (ADC%d ch%d)", CONFIG_FSF_FADER_GPIO, unit + 1, s_adc_chan);
    return err;
}

uint16_t fsf_fader_sample(void)
{
    if (!s_adc_ok) {
        return s_out;
    }
    int sum = 0, n = 0;
    for (int i = 0; i < FADER_SAMPLES; i++) {
        int v;
        if (adc_oneshot_read(s_adc, s_adc_chan, &v) == ESP_OK) {
            sum += v;
            n++;
        }
    }
    if (n == 0) {
        return s_out;
    }
    int raw = sum / n;
    s_raw = (uint16_t)raw;
    if (s_filt < 0) {
        s_filt = raw * 16;
    } else {
        s_filt += (raw * 16 - s_filt) / 3; /* light IIR: ~3 frame time constant */
    }
    const int lo = CONFIG_FSF_FADER_RAW_MIN * 16, hi = CONFIG_FSF_FADER_RAW_MAX * 16;
    int32_t scaled;
    if (s_filt <= lo) {
        scaled = 0;
    } else if (s_filt >= hi) {
        scaled = 65535;
    } else {
        scaled = (int32_t)(((int64_t)(s_filt - lo) * 65535) / (hi - lo));
    }
    /* Hysteresis keeps the output rock-steady when the fader isn't being touched, but the
     * end stops are always reachable exactly. */
    int diff = scaled - (int)s_out;
    if (diff > FADER_HYSTERESIS || diff < -FADER_HYSTERESIS || scaled == 0 || scaled == 65535) {
        s_out = (uint16_t)scaled;
    }
    return s_out;
}

uint16_t fsf_fader_last(void)
{
    return s_out;
}

uint16_t fsf_fader_raw(void)
{
    return s_raw;
}

static esp_err_t pixels_init(void)
{
    rmt_tx_channel_config_t tx = {
        .gpio_num = CONFIG_FSF_PIXEL_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000, /* 0.1 us ticks */
        .mem_block_symbols = 48,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR_LOG(rmt_new_tx_channel(&tx, &s_rmt));
    /* SK6812: T0H 0.3us T0L 0.9us, T1H 0.6us T1L 0.6us, GRB, MSB first. */
    rmt_bytes_encoder_config_t bcfg = {
        .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
        .bit1 = {.level0 = 1, .duration0 = 6, .level1 = 0, .duration1 = 6},
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR_LOG(rmt_new_bytes_encoder(&bcfg, &s_enc));
    ESP_RETURN_ON_ERROR_LOG(rmt_enable(s_rmt));
    return ESP_OK;
}

void fsf_pixels_show(const fsf_rgb_t px[FSF_PIXELS], uint8_t brightness)
{
    if (!s_rmt) {
        return;
    }
    if (brightness > 100) {
        brightness = 100;
    }
    /* Previous frame must be finished before the buffer is reused. */
    if (rmt_tx_wait_all_done(s_rmt, 20) != ESP_OK) {
        ESP_LOGW(TAG, "pixel transmit timeout");
        rmt_disable(s_rmt);
        rmt_enable(s_rmt);
    }
    for (int i = 0; i < FSF_PIXELS; i++) {
        s_grb[i * 3 + 0] = (uint8_t)((px[i].g * brightness + 50) / 100);
        s_grb[i * 3 + 1] = (uint8_t)((px[i].r * brightness + 50) / 100);
        s_grb[i * 3 + 2] = (uint8_t)((px[i].b * brightness + 50) / 100);
    }
    rmt_transmit_config_t tcfg = {.loop_count = 0};
    rmt_transmit(s_rmt, s_enc, s_grb, sizeof(s_grb), &tcfg);
}

void fsf_status_led(bool r, bool g, bool b)
{
    const bool on[3] = {r, g, b};
    for (int i = 0; i < 3; i++) {
        if (s_led_pins[i] >= 0) {
            gpio_set_level(s_led_pins[i], CONFIG_FSF_STATUS_LED_ACTIVE_LOW ? !on[i] : on[i]);
        }
    }
}

bool fsf_button_pressed(void)
{
#if CONFIG_FSF_BUTTON_GPIO >= 0
    return gpio_get_level(CONFIG_FSF_BUTTON_GPIO) == 0;
#else
    return false;
#endif
}

esp_err_t fsf_io_init(void)
{
    for (int i = 0; i < 3; i++) {
        if (s_led_pins[i] >= 0) {
            gpio_reset_pin(s_led_pins[i]);
            gpio_set_direction(s_led_pins[i], GPIO_MODE_OUTPUT);
        }
    }
    fsf_status_led(false, false, false);
#if CONFIG_FSF_BUTTON_GPIO >= 0
    gpio_config_t b = {
        .pin_bit_mask = 1ULL << CONFIG_FSF_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&b);
#endif
    esp_err_t e1 = fader_init();
    esp_err_t e2 = pixels_init();
    if (e2 == ESP_OK) {
        fsf_rgb_t off[FSF_PIXELS] = {0};
        fsf_pixels_show(off, 100);
    }
    return e1 != ESP_OK ? e1 : e2;
}
