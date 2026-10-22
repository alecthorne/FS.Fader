#include "fsf_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "fsf_fixtures.h"
#include "fsf_storage.h"

static const char *TAG = "config";

#define CONFIG_KEY "config"
#define MAX_LISTENERS 8

static SemaphoreHandle_t s_lock;
static fsf_config_t s_cfg;
static fsf_config_listener_t s_listeners[MAX_LISTENERS];
static int s_num_listeners;

static const char *const PARAM_KEYS[FSF_PARAM_COUNT] = {"intensity", "iris", "zoom", "edge", "frost"};
static const char *const PARAM_LABELS[FSF_PARAM_COUNT] = {"Intensity", "Iris", "Zoom", "Edge", "Frost"};

const char *fsf_param_key(fsf_param_t p)
{
    return (p < FSF_PARAM_COUNT) ? PARAM_KEYS[p] : "?";
}

const char *fsf_param_label(fsf_param_t p)
{
    return (p < FSF_PARAM_COUNT) ? PARAM_LABELS[p] : "?";
}

bool fsf_param_from_key(const char *key, fsf_param_t *out)
{
    for (int i = 0; i < FSF_PARAM_COUNT; i++) {
        if (strcasecmp(key, PARAM_KEYS[i]) == 0) {
            *out = (fsf_param_t)i;
            return true;
        }
    }
    return false;
}

void fsf_config_defaults(fsf_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->net.dhcp = true;
    cfg->net.ip = inet_addr("192.168.1.50");
    cfg->net.netmask = inet_addr("255.255.255.0");
    cfg->net.gateway = inet_addr("192.168.1.1");
    cfg->parameter = FSF_PARAM_INTENSITY;
    cfg->intensity_mode = FSF_IMODE_SCALED;
    strlcpy(cfg->fixture_id, "generic-dimmer-8bit", sizeof(cfg->fixture_id));
    cfg->src_universe = 1;
    cfg->src_address = 1;
    cfg->dst_universe = 2;
    cfg->dst_address = 1;
    cfg->dst_priority = 100;
    strlcpy(cfg->device_label, "FS*Fader", sizeof(cfg->device_label));
    strlcpy(cfg->scope, "default", sizeof(cfg->scope));
    strlcpy(cfg->search_domain, "local.", sizeof(cfg->search_domain));
    cfg->static_broker = false;
    cfg->static_broker_port = 0;
    cfg->led_brightness = 40;
}

/* ------------------------------------------------------------------------------------------ */
/* Validation                                                                                  */

static bool netmask_valid(uint32_t mask_n)
{
    uint32_t m = ntohl(mask_n);
    if (m == 0) {
        return false;
    }
    uint32_t inv = ~m;
    return (inv & (inv + 1)) == 0; /* contiguous ones */
}

static esp_err_t validate(const fsf_config_t *c, char *err, size_t err_len)
{
#define FAIL(...)                              \
    do {                                       \
        snprintf(err, err_len, __VA_ARGS__);   \
        return ESP_ERR_INVALID_ARG;            \
    } while (0)

    if (!c->net.dhcp) {
        uint32_t ip = ntohl(c->net.ip);
        if (ip == 0 || ip == 0xFFFFFFFF || (ip >> 28) == 0xE) {
            FAIL("Static IP address is not a valid unicast address");
        }
        if (!netmask_valid(c->net.netmask)) {
            FAIL("Subnet mask is not valid");
        }
        if (c->net.gateway != 0 && ((c->net.gateway ^ c->net.ip) & c->net.netmask) != 0) {
            FAIL("Gateway is not inside the device's subnet");
        }
    }
    if (c->parameter >= FSF_PARAM_COUNT) {
        FAIL("Unknown controlled parameter");
    }
    if (c->intensity_mode != FSF_IMODE_SCALED && c->intensity_mode != FSF_IMODE_LOCAL_OVERRIDE) {
        FAIL("Unknown intensity mode");
    }
    if (c->src_universe < 1 || c->src_universe > 63999) {
        FAIL("Source universe must be 1-63999");
    }
    if (c->dst_universe < 1 || c->dst_universe > 63999) {
        FAIL("Destination universe must be 1-63999");
    }
    if (c->src_address < 1 || c->src_address > 512) {
        FAIL("Source start address must be 1-512");
    }
    if (c->dst_address < 1 || c->dst_address > 512) {
        FAIL("Destination start address must be 1-512");
    }
    if (c->dst_priority > 200) {
        FAIL("sACN priority must be 0-200");
    }
    fsf_fixture_t fx;
    if (!fsf_fixture_get(c->fixture_id, &fx)) {
        FAIL("Fixture profile '%s' is not installed", c->fixture_id);
    }
    if (c->src_address + fx.footprint - 1 > 512) {
        FAIL("Fixture footprint (%u) does not fit at source address %u", fx.footprint, c->src_address);
    }
    if (c->dst_address + fx.footprint - 1 > 512) {
        FAIL("Fixture footprint (%u) does not fit at destination address %u", fx.footprint,
             c->dst_address);
    }
    if (c->device_label[0] == '\0') {
        FAIL("Device label must not be empty");
    }
    if (c->scope[0] == '\0') {
        FAIL("RDMnet scope must not be empty");
    }
    if (c->static_broker && (c->static_broker_ip == 0 || c->static_broker_port == 0)) {
        FAIL("Static broker needs an IPv4 address and port");
    }
    if (c->led_brightness < 1 || c->led_brightness > 100) {
        FAIL("LED brightness must be 1-100");
    }
    return ESP_OK;
#undef FAIL
}

/* ------------------------------------------------------------------------------------------ */
/* JSON                                                                                        */

static bool json_ip(const cJSON *obj, const char *key, uint32_t *out, char *err, size_t err_len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsString(it)) {
        snprintf(err, err_len, "'%s' must be a string like 192.168.1.10", key);
        return false;
    }
    if (it->valuestring[0] == '\0') {
        *out = 0;
        return true;
    }
    struct in_addr a;
    if (inet_aton(it->valuestring, &a) == 0) {
        snprintf(err, err_len, "'%s' is not a valid IPv4 address", key);
        return false;
    }
    *out = a.s_addr;
    return true;
}

static bool json_uint(const cJSON *obj, const char *key, uint32_t max, uint32_t *out, char *err,
                      size_t err_len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsNumber(it) || it->valuedouble < 0 || it->valuedouble > max ||
        it->valuedouble != (double)(uint32_t)it->valuedouble) {
        snprintf(err, err_len, "'%s' must be a whole number 0-%lu", key, (unsigned long)max);
        return false;
    }
    *out = (uint32_t)it->valuedouble;
    return true;
}

static bool json_str(const cJSON *obj, const char *key, char *out, size_t out_size, char *err,
                     size_t err_len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) {
        return true;
    }
    if (!cJSON_IsString(it) || strlen(it->valuestring) >= out_size) {
        snprintf(err, err_len, "'%s' must be a string of at most %u characters", key,
                 (unsigned)(out_size - 1));
        return false;
    }
    strlcpy(out, it->valuestring, out_size);
    return true;
}

esp_err_t fsf_config_from_json(const char *json, size_t len, const fsf_config_t *base,
                               fsf_config_t *out, char *err, size_t err_len)
{
    *out = *base;
    err[0] = '\0';
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        snprintf(err, err_len, "Not a valid JSON object");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t res = ESP_ERR_INVALID_ARG;
    uint32_t u;
    char tmp[64];

    const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
    if (fmt && (!cJSON_IsString(fmt) || strcmp(fmt->valuestring, FSF_CONFIG_FORMAT) != 0)) {
        snprintf(err, err_len, "Unsupported config format (expected \"%s\")", FSF_CONFIG_FORMAT);
        goto done;
    }

    const cJSON *net = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (net) {
        tmp[0] = '\0';
        if (!json_str(net, "mode", tmp, sizeof(tmp), err, err_len)) {
            goto done;
        }
        if (tmp[0]) {
            if (strcasecmp(tmp, "dhcp") == 0) {
                out->net.dhcp = true;
            } else if (strcasecmp(tmp, "static") == 0) {
                out->net.dhcp = false;
            } else {
                snprintf(err, err_len, "network.mode must be \"dhcp\" or \"static\"");
                goto done;
            }
        }
        if (!json_ip(net, "ip", &out->net.ip, err, err_len) ||
            !json_ip(net, "subnet", &out->net.netmask, err, err_len) ||
            !json_ip(net, "gateway", &out->net.gateway, err, err_len)) {
            goto done;
        }
    }

    const cJSON *ctl = cJSON_GetObjectItemCaseSensitive(root, "control");
    if (ctl) {
        tmp[0] = '\0';
        if (!json_str(ctl, "parameter", tmp, sizeof(tmp), err, err_len)) {
            goto done;
        }
        if (tmp[0] && !fsf_param_from_key(tmp, &out->parameter)) {
            snprintf(err, err_len, "control.parameter must be intensity, iris, zoom, edge or frost");
            goto done;
        }
        tmp[0] = '\0';
        if (!json_str(ctl, "intensity_mode", tmp, sizeof(tmp), err, err_len)) {
            goto done;
        }
        if (tmp[0]) {
            if (strcasecmp(tmp, "scaled") == 0) {
                out->intensity_mode = FSF_IMODE_SCALED;
            } else if (strcasecmp(tmp, "local_override") == 0) {
                out->intensity_mode = FSF_IMODE_LOCAL_OVERRIDE;
            } else {
                snprintf(err, err_len, "control.intensity_mode must be \"scaled\" or \"local_override\"");
                goto done;
            }
        }
    }

    if (!json_str(root, "fixture", out->fixture_id, sizeof(out->fixture_id), err, err_len)) {
        goto done;
    }

    const cJSON *src = cJSON_GetObjectItemCaseSensitive(root, "source");
    if (src) {
        u = out->src_universe;
        if (!json_uint(src, "universe", 63999, &u, err, err_len)) {
            goto done;
        }
        out->src_universe = u;
        u = out->src_address;
        if (!json_uint(src, "address", 512, &u, err, err_len)) {
            goto done;
        }
        out->src_address = u;
    }

    const cJSON *dst = cJSON_GetObjectItemCaseSensitive(root, "destination");
    if (dst) {
        u = out->dst_universe;
        if (!json_uint(dst, "universe", 63999, &u, err, err_len)) {
            goto done;
        }
        out->dst_universe = u;
        u = out->dst_address;
        if (!json_uint(dst, "address", 512, &u, err, err_len)) {
            goto done;
        }
        out->dst_address = u;
        u = out->dst_priority;
        if (!json_uint(dst, "priority", 200, &u, err, err_len)) {
            goto done;
        }
        out->dst_priority = u;
    }

    const cJSON *rdm = cJSON_GetObjectItemCaseSensitive(root, "rdmnet");
    if (rdm) {
        if (!json_str(rdm, "device_label", out->device_label, sizeof(out->device_label), err, err_len) ||
            !json_str(rdm, "scope", out->scope, sizeof(out->scope), err, err_len) ||
            !json_str(rdm, "search_domain", out->search_domain, sizeof(out->search_domain), err,
                      err_len)) {
            goto done;
        }
        u = out->identify_timeout_s;
        if (!json_uint(rdm, "identify_timeout", 65535, &u, err, err_len)) {
            goto done;
        }
        out->identify_timeout_s = (uint16_t)u;
        const cJSON *sb = cJSON_GetObjectItemCaseSensitive(rdm, "static_broker");
        if (sb) {
            if (!cJSON_IsString(sb)) {
                snprintf(err, err_len, "rdmnet.static_broker must be \"\" or \"ip:port\"");
                goto done;
            }
            if (sb->valuestring[0] == '\0') {
                out->static_broker = false;
                out->static_broker_ip = 0;
                out->static_broker_port = 0;
            } else {
                char host[32];
                unsigned port = 0;
                struct in_addr a;
                if (sscanf(sb->valuestring, "%31[0-9.]:%u", host, &port) != 2 || port == 0 ||
                    port > 65535 || inet_aton(host, &a) == 0) {
                    snprintf(err, err_len, "rdmnet.static_broker must be \"\" or \"ip:port\"");
                    goto done;
                }
                out->static_broker = true;
                out->static_broker_ip = a.s_addr;
                out->static_broker_port = (uint16_t)port;
            }
        }
    }

    const cJSON *leds = cJSON_GetObjectItemCaseSensitive(root, "leds");
    if (leds) {
        u = out->led_brightness;
        if (!json_uint(leds, "brightness", 100, &u, err, err_len)) {
            goto done;
        }
        out->led_brightness = u;
    }

    res = validate(out, err, err_len);

done:
    cJSON_Delete(root);
    return res;
}

static void ip_str(uint32_t a, char *buf, size_t len)
{
    struct in_addr in = {.s_addr = a};
    strlcpy(buf, a ? inet_ntoa(in) : "", len);
}

char *fsf_config_to_json(const fsf_config_t *c)
{
    char b[48];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", FSF_CONFIG_FORMAT);

    cJSON *net = cJSON_AddObjectToObject(root, "network");
    cJSON_AddStringToObject(net, "mode", c->net.dhcp ? "dhcp" : "static");
    ip_str(c->net.ip, b, sizeof(b));
    cJSON_AddStringToObject(net, "ip", b);
    ip_str(c->net.netmask, b, sizeof(b));
    cJSON_AddStringToObject(net, "subnet", b);
    ip_str(c->net.gateway, b, sizeof(b));
    cJSON_AddStringToObject(net, "gateway", b);

    cJSON *ctl = cJSON_AddObjectToObject(root, "control");
    cJSON_AddStringToObject(ctl, "parameter", fsf_param_key(c->parameter));
    cJSON_AddStringToObject(ctl, "intensity_mode",
                            c->intensity_mode == FSF_IMODE_LOCAL_OVERRIDE ? "local_override" : "scaled");

    cJSON_AddStringToObject(root, "fixture", c->fixture_id);

    cJSON *src = cJSON_AddObjectToObject(root, "source");
    cJSON_AddNumberToObject(src, "universe", c->src_universe);
    cJSON_AddNumberToObject(src, "address", c->src_address);

    cJSON *dst = cJSON_AddObjectToObject(root, "destination");
    cJSON_AddNumberToObject(dst, "universe", c->dst_universe);
    cJSON_AddNumberToObject(dst, "address", c->dst_address);
    cJSON_AddNumberToObject(dst, "priority", c->dst_priority);

    cJSON *rdm = cJSON_AddObjectToObject(root, "rdmnet");
    cJSON_AddStringToObject(rdm, "device_label", c->device_label);
    cJSON_AddStringToObject(rdm, "scope", c->scope);
    cJSON_AddStringToObject(rdm, "search_domain", c->search_domain);
    if (c->static_broker) {
        char sb[64];
        ip_str(c->static_broker_ip, b, sizeof(b));
        snprintf(sb, sizeof(sb), "%s:%u", b, c->static_broker_port);
        cJSON_AddStringToObject(rdm, "static_broker", sb);
    } else {
        cJSON_AddStringToObject(rdm, "static_broker", "");
    }

    cJSON_AddNumberToObject(rdm, "identify_timeout", c->identify_timeout_s);

    cJSON *leds = cJSON_AddObjectToObject(root, "leds");
    cJSON_AddNumberToObject(leds, "brightness", c->led_brightness);

    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    return s;
}

/* ------------------------------------------------------------------------------------------ */
/* Persistence                                                                                 */

static esp_err_t save(const fsf_config_t *c)
{
    char *s = fsf_config_to_json(c);
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = fsf_storage_write(FSF_NS_CONFIG, CONFIG_KEY, s, strlen(s));
    free(s);
    return err;
}

esp_err_t fsf_config_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    fsf_config_t defaults;
    fsf_config_defaults(&defaults);

    size_t len = 0;
    char *json = fsf_storage_read(FSF_NS_CONFIG, CONFIG_KEY, &len);
    if (json) {
        char err[128];
        if (fsf_config_from_json(json, len, &defaults, &s_cfg, err, sizeof(err)) == ESP_OK) {
            free(json);
            ESP_LOGI(TAG, "configuration loaded");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "stored configuration rejected (%s); using defaults", err);
        free(json);
    } else {
        ESP_LOGI(TAG, "no stored configuration; using defaults");
    }
    s_cfg = defaults;
    return save(&s_cfg);
}

void fsf_config_get(fsf_config_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

esp_err_t fsf_config_set(const fsf_config_t *cfg, char *err, size_t err_len)
{
    char dummy[8];
    if (!err) {
        err = dummy;
        err_len = sizeof(dummy);
    }
    esp_err_t res = validate(cfg, err, err_len);
    if (res != ESP_OK) {
        return res;
    }
    fsf_config_t old;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    old = s_cfg;
    if (memcmp(&old, cfg, sizeof(old)) == 0) {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }
    res = save(cfg);
    if (res == ESP_OK) {
        s_cfg = *cfg;
    } else {
        snprintf(err, err_len, "Saving to flash failed (%s)", esp_err_to_name(res));
    }
    xSemaphoreGive(s_lock);

    if (res == ESP_OK) {
        for (int i = 0; i < s_num_listeners; i++) {
            s_listeners[i](&old, cfg);
        }
    }
    return res;
}

esp_err_t fsf_config_factory_reset(void)
{
    esp_err_t e1 = fsf_storage_erase_namespace(FSF_NS_CONFIG);
    esp_err_t e2 = fsf_storage_erase_namespace(FSF_NS_FIXTURES);
    return e1 != ESP_OK ? e1 : e2;
}

void fsf_config_add_listener(fsf_config_listener_t fn)
{
    if (s_num_listeners < MAX_LISTENERS) {
        s_listeners[s_num_listeners++] = fn;
    }
}
