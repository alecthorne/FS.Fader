/*
 * FS*Fader - configuration web interface and JSON API.
 *
 *   GET  /                       configuration page
 *   GET  /profile-builder        companion fixture-profile builder (same as tools/profile-builder.html)
 *   GET  /api/status             live status
 *   GET  /api/config             current configuration
 *   POST /api/config             edit: apply the keys present in the posted JSON
 *   GET  /api/config/export      download the configuration file
 *   POST /api/config/import      replace the configuration with the posted file (missing keys = defaults)
 *   GET  /api/fixtures           installed fixture profiles (summary)
 *   GET  /api/fixtures/get?id=   one profile's JSON
 *   POST /api/fixtures           install profile(s): one profile object or an array of them
 *   POST /api/fixtures/delete?id=
 *   POST /api/identify           {"on": true|false}
 *   POST /api/reboot
 *   POST /api/factory-reset
 *   POST /api/ota                raw firmware .bin upload
 */
#include "fsf_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "fsf_config.h"
#include "fsf_engine.h"
#include "fsf_fixtures.h"
#include "fsf_io.h"
#include "fsf_net.h"
#include "fsf_rdm.h"
#include "fsf_sacn.h"

static const char *TAG = "web";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char builder_html_start[] asm("_binary_profile_builder_html_start");
extern const char builder_html_end[] asm("_binary_profile_builder_html_end");

#define MAX_JSON_BODY (64 * 1024)

static esp_timer_handle_t s_reboot_timer;

static void reboot_cb(void *arg)
{
    esp_restart();
}

static void schedule_reboot(void)
{
    esp_timer_start_once(s_reboot_timer, 1000 * 1000);
}

static void cors(httpd_req_t *req)
{
    /* Lets the stand-alone profile builder (opened from disk) upload profiles. */
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    cors(req);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return r;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, "200 OK", o);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg);
    return send_json(req, status, o);
}

/* Read the request body into a heap buffer (NUL terminated). */
static char *read_body(httpd_req_t *req, size_t max, size_t *len_out)
{
    if (req->content_len == 0 || req->content_len > max) {
        return NULL;
    }
    char *buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(req->content_len + 1);
    }
    if (!buf) {
        return NULL;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = '\0';
    *len_out = got;
    return buf;
}

static bool query_param(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char q[128];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
           httpd_query_key_value(q, key, out, len) == ESP_OK;
}

/* ------------------------------------------------------------------------------------------ */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_builder(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, builder_html_start, builder_html_end - builder_html_start - 1);
}

static esp_err_t h_options(httpd_req_t *req)
{
    cors(req);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static void add_ip(cJSON *o, const char *k, uint32_t a)
{
    struct in_addr in = {.s_addr = a};
    cJSON_AddStringToObject(o, k, a ? inet_ntoa(in) : "");
}

static esp_err_t h_status(httpd_req_t *req)
{
    fsf_config_t cfg;
    fsf_config_get(&cfg);
    fsf_net_status_t ns;
    fsf_net_get_status(&ns);
    fsf_engine_status_t es;
    fsf_engine_get_status(&es);
    fsf_sacn_rx_status_t rx;
    fsf_sacn_rx_snapshot(NULL, &rx);
    fsf_rdm_status_t rs;
    fsf_rdm_get_status(&rs);

    cJSON *o = cJSON_CreateObject();
    cJSON *dev = cJSON_AddObjectToObject(o, "device");
    cJSON_AddStringToObject(dev, "hostname", fsf_net_hostname());
    cJSON_AddStringToObject(dev, "label", cfg.device_label);
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(dev, "firmware", app->version);
    cJSON_AddStringToObject(dev, "build", app->date);
    char mac[18];
    const uint8_t *m = fsf_net_mac();
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    cJSON_AddStringToObject(dev, "mac", mac);
    cJSON_AddNumberToObject(dev, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(dev, "free_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(dev, "min_free_heap", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddStringToObject(dev, "reset_reason", (const char *[]){
        "unknown", "power-on", "external", "software", "panic", "int-wdt", "task-wdt", "wdt",
        "deep-sleep", "brownout", "sdio", "usb", "jtag", "efuse", "pwr-glitch", "cpu-lockup"}
        [esp_reset_reason() <= ESP_RST_CPU_LOCKUP ? esp_reset_reason() : 0]);

    cJSON *net = cJSON_AddObjectToObject(o, "network");
    cJSON_AddBoolToObject(net, "link", ns.link_up);
    cJSON_AddStringToObject(net, "mode", cfg.net.dhcp ? "dhcp" : "static");
    cJSON_AddBoolToObject(net, "link_local", ns.link_local);
    add_ip(net, "ip", ns.ip);
    add_ip(net, "subnet", ns.netmask);
    add_ip(net, "gateway", ns.gateway);

    cJSON *s = cJSON_AddObjectToObject(o, "sacn");
    cJSON_AddBoolToObject(s, "receiving", rx.source_present);
    cJSON_AddBoolToObject(s, "have_data", rx.have_data);
    cJSON_AddNumberToObject(s, "priority", rx.priority);
    cJSON_AddNumberToObject(s, "packets", rx.packets);
    cJSON_AddNumberToObject(s, "sequence_dropped", rx.seq_dropped);
    cJSON *srcs = cJSON_AddArrayToObject(s, "sources");
    fsf_sacn_source_info_t si[FSF_SACN_MAX_SOURCES];
    int n = fsf_sacn_rx_sources(si, FSF_SACN_MAX_SOURCES);
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", si[i].name);
        add_ip(e, "ip", si[i].ip);
        cJSON_AddNumberToObject(e, "priority", si[i].priority);
        cJSON_AddNumberToObject(e, "age_ms", si[i].age_ms);
        cJSON_AddItemToArray(srcs, e);
    }

    cJSON *c = cJSON_AddObjectToObject(o, "control");
    cJSON_AddNumberToObject(c, "fader", es.fader);
    cJSON_AddNumberToObject(c, "fader_percent", (es.fader * 100.0) / 65535.0);
    cJSON_AddNumberToObject(c, "fader_adc_raw", fsf_fader_raw());
    cJSON_AddBoolToObject(c, "parameter_in_profile", es.param_present);
    cJSON_AddNumberToObject(c, "bits", es.param_bits);
    cJSON_AddNumberToObject(c, "received", es.rx_value);
    cJSON_AddNumberToObject(c, "sent", es.tx_value);
    cJSON_AddNumberToObject(c, "received_percent", es.rx_percent);
    cJSON_AddNumberToObject(c, "sent_percent", es.tx_percent);
    cJSON_AddNumberToObject(c, "frames", es.frames);
    cJSON_AddBoolToObject(c, "identify", es.identify);
    cJSON_AddNumberToObject(c, "identify_remaining_s", es.identify_remaining_s);

    cJSON *r = cJSON_AddObjectToObject(o, "rdmnet");
    cJSON_AddStringToObject(r, "uid", rs.uid);
    cJSON_AddStringToObject(r, "cid", rs.cid);
    cJSON_AddBoolToObject(r, "running", rs.running);
    cJSON_AddBoolToObject(r, "connected", rs.connected);
    cJSON_AddStringToObject(r, "broker", rs.broker);
    cJSON_AddNumberToObject(r, "unhealthy_tcp_events", rs.unhealthy_tcp_events);
    cJSON_AddStringToObject(r, "last_event", rs.last_event);
    return send_json(req, "200 OK", o);
}

static esp_err_t send_config(httpd_req_t *req, bool attachment)
{
    fsf_config_t cfg;
    fsf_config_get(&cfg);
    char *s = fsf_config_to_json(&cfg);
    if (!s) {
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    cors(req);
    httpd_resp_set_type(req, "application/json");
    char cd[96];
    if (attachment) {
        const uint8_t *m = fsf_net_mac();
        snprintf(cd, sizeof(cd), "attachment; filename=\"fsfader-%02x%02x-config.json\"", m[4], m[5]);
        httpd_resp_set_hdr(req, "Content-Disposition", cd);
    }
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
    return r;
}

static esp_err_t h_config_get(httpd_req_t *req)
{
    return send_config(req, false);
}

static esp_err_t h_config_export(httpd_req_t *req)
{
    return send_config(req, true);
}

static esp_err_t config_post(httpd_req_t *req, bool import)
{
    size_t len;
    char *body = read_body(req, 16 * 1024, &len);
    if (!body) {
        return send_error(req, "400 Bad Request", "missing or oversized body");
    }
    fsf_config_t base, next;
    if (import) {
        fsf_config_defaults(&base);
    } else {
        fsf_config_get(&base);
    }
    char err[128];
    esp_err_t e = fsf_config_from_json(body, len, &base, &next, err, sizeof(err));
    free(body);
    if (e == ESP_OK) {
        e = fsf_config_set(&next, err, sizeof(err));
    }
    if (e != ESP_OK) {
        return send_error(req, "400 Bad Request", err);
    }
    return send_config(req, false);
}

static esp_err_t h_config_post(httpd_req_t *req)
{
    return config_post(req, false);
}

static esp_err_t h_config_import(httpd_req_t *req)
{
    return config_post(req, true);
}

static esp_err_t h_fixtures(httpd_req_t *req)
{
    fsf_config_t cfg;
    fsf_config_get(&cfg);
    cJSON *arr = cJSON_CreateArray();
    size_t n = fsf_fixture_count();
    for (size_t i = 0; i < n; i++) {
        fsf_fixture_t fx;
        if (!fsf_fixture_at(i, &fx)) {
            break;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", fx.id);
        cJSON_AddStringToObject(o, "manufacturer", fx.manufacturer);
        cJSON_AddStringToObject(o, "model", fx.model);
        cJSON_AddStringToObject(o, "mode", fx.mode);
        cJSON_AddNumberToObject(o, "footprint", fx.footprint);
        cJSON_AddBoolToObject(o, "builtin", fx.builtin);
        cJSON_AddBoolToObject(o, "selected", strcmp(fx.id, cfg.fixture_id) == 0);
        cJSON *ps = cJSON_AddObjectToObject(o, "parameters");
        for (int p = 0; p < FSF_PARAM_COUNT; p++) {
            if (fx.params[p].present) {
                cJSON *pp = cJSON_AddObjectToObject(ps, fsf_param_key(p));
                cJSON_AddNumberToObject(pp, "offset", fx.params[p].offset);
                cJSON_AddNumberToObject(pp, "bits", fx.params[p].bits);
                cJSON *rg = cJSON_AddArrayToObject(pp, "range");
                cJSON_AddItemToArray(rg, cJSON_CreateNumber(fx.params[p].from));
                cJSON_AddItemToArray(rg, cJSON_CreateNumber(fx.params[p].to));
            }
        }
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, "200 OK", arr);
}

static esp_err_t h_fixture_get(httpd_req_t *req)
{
    char id[FSF_FIXTURE_ID_LEN];
    if (!query_param(req, "id", id, sizeof(id))) {
        return send_error(req, "400 Bad Request", "id required");
    }
    char *s = fsf_fixture_json(id);
    if (!s) {
        return send_error(req, "404 Not Found", "no such profile");
    }
    cors(req);
    httpd_resp_set_type(req, "application/json");
    char cd[96];
    snprintf(cd, sizeof(cd), "inline; filename=\"%s.json\"", id);
    httpd_resp_set_hdr(req, "Content-Disposition", cd);
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
    return r;
}

static esp_err_t h_fixture_post(httpd_req_t *req)
{
    size_t len;
    char *body = read_body(req, MAX_JSON_BODY, &len);
    if (!body) {
        return send_error(req, "400 Bad Request", "missing or oversized body");
    }
    cJSON *root = cJSON_ParseWithLength(body, len);
    free(body);
    if (!root) {
        return send_error(req, "400 Bad Request", "Not valid JSON");
    }
    cJSON *result = cJSON_CreateObject();
    cJSON *installed = cJSON_AddArrayToObject(result, "installed");
    cJSON *errors = cJSON_AddArrayToObject(result, "errors");
    cJSON *list = cJSON_IsArray(root) ? root : NULL;
    int total = list ? cJSON_GetArraySize(list) : 1;
    for (int i = 0; i < total; i++) {
        cJSON *item = list ? cJSON_GetArrayItem(list, i) : root;
        char *txt = cJSON_PrintUnformatted(item);
        char id[FSF_FIXTURE_ID_LEN] = "", err[128] = "";
        if (txt && fsf_fixture_install(txt, strlen(txt), id, sizeof(id), err, sizeof(err)) == ESP_OK) {
            cJSON_AddItemToArray(installed, cJSON_CreateString(id));
        } else {
            char msg[200];
            snprintf(msg, sizeof(msg), "profile %d%s%s: %s", i + 1, id[0] ? " " : "", id, err);
            cJSON_AddItemToArray(errors, cJSON_CreateString(msg));
        }
        free(txt);
    }
    cJSON_Delete(root);
    bool ok = cJSON_GetArraySize(errors) == 0;
    cJSON_AddBoolToObject(result, "ok", ok);
    return send_json(req, ok ? "200 OK" : "400 Bad Request", result);
}

static esp_err_t h_fixture_delete(httpd_req_t *req)
{
    char id[FSF_FIXTURE_ID_LEN], err[96];
    if (!query_param(req, "id", id, sizeof(id))) {
        return send_error(req, "400 Bad Request", "id required");
    }
    if (fsf_fixture_delete(id, err, sizeof(err)) != ESP_OK) {
        return send_error(req, "400 Bad Request", err);
    }
    return send_ok(req);
}

static esp_err_t h_identify(httpd_req_t *req)
{
    size_t len;
    char *body = read_body(req, 256, &len);
    cJSON *o = body ? cJSON_ParseWithLength(body, len) : NULL;
    free(body);
    const cJSON *on = o ? cJSON_GetObjectItemCaseSensitive(o, "on") : NULL;
    bool v = cJSON_IsTrue(on);
    cJSON_Delete(o);
    if (!on) {
        return send_error(req, "400 Bad Request", "expected {\"on\": true|false}");
    }
    fsf_identify_set(v);
    return send_ok(req);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    schedule_reboot();
    return send_ok(req);
}

static esp_err_t h_factory_reset(httpd_req_t *req)
{
    esp_err_t e = fsf_config_factory_reset();
    if (e != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    }
    schedule_reboot();
    return send_ok(req);
}

static esp_err_t h_ota(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size) {
        return send_error(req, "400 Bad Request", "missing or oversized firmware image");
    }
    esp_ota_handle_t ota;
    esp_err_t e = esp_ota_begin(part, req->content_len, &ota);
    if (e != ESP_OK) {
        return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    }
    char *buf = malloc(4096);
    size_t remaining = req->content_len;
    while (buf && remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            e = ESP_FAIL;
            break;
        }
        e = esp_ota_write(ota, buf, r);
        if (e != ESP_OK) {
            break;
        }
        remaining -= r;
    }
    free(buf);
    if (!buf) {
        e = ESP_ERR_NO_MEM;
    }
    if (e == ESP_OK) {
        e = esp_ota_end(ota);
    } else {
        esp_ota_abort(ota);
    }
    if (e == ESP_OK) {
        e = esp_ota_set_boot_partition(part);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(e));
        return send_error(req, "400 Bad Request",
                          e == ESP_ERR_OTA_VALIDATE_FAILED ? "image is not valid firmware for this device"
                                                           : esp_err_to_name(e));
    }
    ESP_LOGI(TAG, "firmware update written to %s; rebooting", part->label);
    schedule_reboot();
    return send_ok(req);
}

/* ------------------------------------------------------------------------------------------ */

esp_err_t fsf_web_init(void)
{
    const esp_timer_create_args_t targs = {.callback = reboot_cb, .name = "reboot"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_reboot_timer));

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 6;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.task_priority = 5;

    httpd_handle_t srv;
    esp_err_t e = httpd_start(&srv, &cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(e));
        return e;
    }
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = h_index},
        {.uri = "/profile-builder", .method = HTTP_GET, .handler = h_builder},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/config", .method = HTTP_GET, .handler = h_config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = h_config_post},
        {.uri = "/api/config/export", .method = HTTP_GET, .handler = h_config_export},
        {.uri = "/api/config/import", .method = HTTP_POST, .handler = h_config_import},
        {.uri = "/api/fixtures", .method = HTTP_GET, .handler = h_fixtures},
        {.uri = "/api/fixtures", .method = HTTP_POST, .handler = h_fixture_post},
        {.uri = "/api/fixtures/get", .method = HTTP_GET, .handler = h_fixture_get},
        {.uri = "/api/fixtures/delete", .method = HTTP_POST, .handler = h_fixture_delete},
        {.uri = "/api/identify", .method = HTTP_POST, .handler = h_identify},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = h_factory_reset},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota},
        {.uri = "/api/*", .method = HTTP_OPTIONS, .handler = h_options},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(srv, &routes[i]);
    }
    ESP_LOGI(TAG, "web interface ready");
    return ESP_OK;
}
