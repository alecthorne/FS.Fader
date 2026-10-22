#include "fsf_engine.h"

#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fsf_config.h"
#include "fsf_fixtures.h"
#include "fsf_io.h"
#include "fsf_net.h"
#include "fsf_sacn.h"
#include "fsf_dmx_math.h"

static const char *TAG = "engine";

#define FRAME_PERIOD_US (1000000 / FSF_FRAME_RATE_HZ) /* 22727 us */
#define DISCOVERY_INTERVAL_US 10000000LL              /* E1.31 universe discovery: 10 s */
#define IDENTIFY_HALF_PERIOD_US 250000LL              /* 2 Hz white/blue alternation */
#define FACTORY_RESET_HOLD_US 10000000LL

static TaskHandle_t s_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static fsf_engine_status_t s_status;

static volatile bool s_identify;
static volatile int64_t s_identify_until_us; /* 0 = no timeout */

static const fsf_rgb_t COLOR_UP = {0, 0, 255};       /* blue: above received */
static const fsf_rgb_t COLOR_DOWN = {0, 255, 0};     /* green: below received */
static const fsf_rgb_t COLOR_SAME = {150, 0, 255};   /* purple: equal */
static const fsf_rgb_t COLOR_WHITE = {255, 255, 255};
static const fsf_rgb_t COLOR_BLUE = {0, 0, 255};
static const fsf_rgb_t COLOR_FAULT = {60, 0, 0};

void fsf_identify_set(bool on)
{
    fsf_config_t cfg;
    fsf_config_get(&cfg);
    int64_t until = 0;
    if (on && cfg.identify_timeout_s > 0) {
        until = esp_timer_get_time() + (int64_t)cfg.identify_timeout_s * 1000000LL;
    }
    taskENTER_CRITICAL(&s_mux);
    s_identify = on;
    s_identify_until_us = until;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "identify %s", on ? "on" : "off");
}

bool fsf_identify_get(void)
{
    return s_identify;
}

void fsf_engine_get_status(fsf_engine_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}

/* ------------------------------------------------------------------------------------------ */

static fsf_rgb_t compare_color(uint16_t current, uint16_t received)
{
    if (current > received) {
        return COLOR_UP;
    }
    if (current < received) {
        return COLOR_DOWN;
    }
    return COLOR_SAME;
}

/* Right side: pixels 1..7 (index 0..6) fill upward from pixel 1.
 * Left side: pixels 14..8 (index 13..7) fill from pixel 14 toward pixel 8. */
static void draw_side(fsf_rgb_t *px, bool left, int lit, fsf_rgb_t color)
{
    for (int i = 0; i < SIDE_PIXELS; i++) {
        int idx = left ? (FSF_PIXELS - 1 - i) : i;
        px[idx] = (i < lit) ? color : (fsf_rgb_t){0, 0, 0};
    }
}

static void engine_task(void *arg)
{
    static uint8_t in[512];
    static uint8_t out[512];
    fsf_config_t cfg;
    fsf_fixture_t fx;
    fsf_rgb_t px[FSF_PIXELS];
    uint16_t active_universe = 0;
    uint8_t active_priority = 100;
    int64_t next_discovery = 0;
    int64_t button_down_since = 0;
    uint32_t frames = 0;

    esp_task_wdt_add(NULL);

    for (;;) {
        /* Woken by the 44 Hz timer; the timeout keeps the loop alive if the timer ever stalls. */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        esp_task_wdt_reset();
        int64_t now = esp_timer_get_time();
        frames++;

        fsf_config_get(&cfg);
        bool have_fx = fsf_fixture_get(cfg.fixture_id, &fx);
        uint16_t fader = fsf_fader_sample();
        fsf_sacn_rx_status_t rx;
        fsf_sacn_rx_snapshot(in, &rx);

        /* ---- Build output: copy the fixture footprint, then replace the controlled parameter ---- */
        memset(out, 0, sizeof(out));
        const fsf_fixture_param_t *p = have_fx ? &fx.params[cfg.parameter] : NULL;
        bool present = p && p->present;
        uint16_t rx_v = 0, tx_v = 0;
        if (have_fx) {
            int src0 = cfg.src_address - 1, dst0 = cfg.dst_address - 1;
            for (int i = 0; i < fx.footprint; i++) {
                if (src0 + i < 512 && dst0 + i < 512) {
                    out[dst0 + i] = in[src0 + i];
                }
            }
        }
        if (present) {
            int si = cfg.src_address - 1 + p->offset - 1;
            int di = cfg.dst_address - 1 + p->offset - 1;
            rx_v = read_native(in, si, p->bits);
            if (cfg.parameter == FSF_PARAM_INTENSITY && cfg.intensity_mode == FSF_IMODE_SCALED) {
                /* Fader scales between the bottom of the range and the received level. */
                tx_v = lerp16(p->from, rx_v, fader);
            } else {
                tx_v = lerp16(p->from, p->to, fader);
            }
            write_native(out, di, p->bits, tx_v);
        }

        /* ---- Transmit ---- */
        if (active_universe && active_universe != cfg.dst_universe) {
            fsf_sacn_tx_terminate(active_universe, active_priority);
            ESP_LOGI(TAG, "stopped sending universe %u", active_universe);
        }
        if (active_universe != cfg.dst_universe) {
            ESP_LOGI(TAG, "sending universe %u", cfg.dst_universe);
            next_discovery = now;
        }
        active_universe = cfg.dst_universe;
        active_priority = cfg.dst_priority;
        fsf_sacn_tx_send(cfg.dst_universe, cfg.dst_priority, out);
        if (now >= next_discovery) {
            fsf_sacn_tx_discovery(cfg.dst_universe);
            next_discovery = now + DISCOVERY_INTERVAL_US;
        }

        /* ---- Identify timeout ---- */
        bool identify;
        uint32_t ident_left = 0;
        taskENTER_CRITICAL(&s_mux);
        if (s_identify && s_identify_until_us && now >= s_identify_until_us) {
            s_identify = false;
            s_identify_until_us = 0;
        }
        identify = s_identify;
        if (identify && s_identify_until_us) {
            ident_left = (uint32_t)((s_identify_until_us - now + 999999) / 1000000);
        }
        taskEXIT_CRITICAL(&s_mux);

        /* ---- Pixels ---- */
        int right_lit = 0, left_lit = 0, rx_pm = 0, tx_pm = 0;
        if (identify) {
            bool white = ((now / IDENTIFY_HALF_PERIOD_US) & 1) == 0;
            for (int i = 0; i < FSF_PIXELS; i++) {
                px[i] = white ? COLOR_WHITE : COLOR_BLUE;
            }
        } else if (present) {
            tx_pm = position_permille(tx_v, p->from, p->to);
            rx_pm = position_permille(rx_v, p->from, p->to);
            right_lit = lit_count(tx_pm);
            fsf_rgb_t right_color = compare_color(tx_v, rx_v);
            draw_side(px, false, right_lit, right_color);
            if (cfg.parameter == FSF_PARAM_INTENSITY && cfg.intensity_mode == FSF_IMODE_LOCAL_OVERRIDE) {
                left_lit = right_lit;
                draw_side(px, true, left_lit, right_color);
            } else {
                /* Left side shows the incoming value; compared with the last received value it
                 * is always "same", i.e. purple. */
                left_lit = lit_count(rx_pm);
                draw_side(px, true, left_lit, compare_color(rx_v, rx_v));
            }
        } else {
            /* Selected parameter not in this fixture profile: dim red bottom pixels. */
            memset(px, 0, sizeof(px));
            px[0] = COLOR_FAULT;
            px[FSF_PIXELS - 1] = COLOR_FAULT;
        }
        fsf_pixels_show(px, cfg.led_brightness);

        /* ---- On-board status LED ---- */
        fsf_net_status_t ns;
        fsf_net_get_status(&ns);
        if (identify) {
            bool white = ((now / IDENTIFY_HALF_PERIOD_US) & 1) == 0;
            fsf_status_led(white, white, true);
        } else if (!ns.has_ip) {
            fsf_status_led(true, false, false); /* red: no link / no address */
        } else if (rx.source_present) {
            fsf_status_led(false, true, false); /* green: receiving sACN */
        } else {
            bool on = ((now / 500000) & 1) == 0;
            fsf_status_led(false, on, false); /* blinking green: online, no source */
        }

        /* ---- Factory reset: hold the user button for 10 s ---- */
        if (fsf_button_pressed()) {
            if (!button_down_since) {
                button_down_since = now;
            } else if (now - button_down_since > FACTORY_RESET_HOLD_US) {
                ESP_LOGW(TAG, "button held: factory reset");
                fsf_config_factory_reset();
                esp_restart();
            }
        } else {
            button_down_since = 0;
        }

        taskENTER_CRITICAL(&s_mux);
        s_status.fader = fader;
        s_status.param_present = present;
        s_status.param_bits = present ? p->bits : 0;
        s_status.rx_value = rx_v;
        s_status.tx_value = tx_v;
        s_status.rx_percent = (uint8_t)((rx_pm + 5) / 10);
        s_status.tx_percent = (uint8_t)((tx_pm + 5) / 10);
        s_status.left_lit = left_lit;
        s_status.right_lit = right_lit;
        s_status.frames = frames;
        s_status.identify = identify;
        s_status.identify_remaining_s = ident_left;
        taskEXIT_CRITICAL(&s_mux);
    }
}

static void frame_timer_cb(void *arg)
{
    xTaskNotifyGive(s_task);
}

esp_err_t fsf_engine_init(void)
{
    if (xTaskCreatePinnedToCore(engine_task, "engine", 6144, NULL, 15, &s_task, 1) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback = frame_timer_cb,
        .name = "frame",
        .dispatch_method = ESP_TIMER_TASK,
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, FRAME_PERIOD_US));
    ESP_LOGI(TAG, "running at %d Hz", FSF_FRAME_RATE_HZ);
    return ESP_OK;
}
