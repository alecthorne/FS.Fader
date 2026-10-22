#include "fsf_fixtures.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fsf_storage.h"

static const char *TAG = "fixtures";

extern const char builtin_json_start[] asm("_binary_builtin_fixtures_json_start");
extern const char builtin_json_end[] asm("_binary_builtin_fixtures_json_end");

static SemaphoreHandle_t s_lock;
static fsf_fixture_t *s_fixtures; /* PSRAM-backed array */
static size_t s_count;

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static void nvs_key_for(const char *id, char key[16])
{
    uint32_t h = 2166136261u; /* FNV-1a */
    for (const char *p = id; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    snprintf(key, 16, "u%08lx", (unsigned long)h);
}

/* ------------------------------------------------------------------------------------------ */

static bool id_valid(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n >= FSF_FIXTURE_ID_LEN) {
        return false;
    }
    for (const char *p = id; *p; p++) {
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '-' || *p == '_' ||
              *p == '.')) {
            return false;
        }
    }
    return true;
}

static bool get_str(const cJSON *o, const char *k, char *out, size_t sz, bool required, char *err,
                    size_t err_len)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!it) {
        out[0] = '\0';
        if (required) {
            snprintf(err, err_len, "'%s' is required", k);
        }
        return !required;
    }
    if (!cJSON_IsString(it) || strlen(it->valuestring) >= sz) {
        snprintf(err, err_len, "'%s' must be a string (max %u characters)", k, (unsigned)(sz - 1));
        return false;
    }
    strlcpy(out, it->valuestring, sz);
    return true;
}

static bool get_int(const cJSON *it, long lo, long hi, long *out)
{
    if (!cJSON_IsNumber(it) || it->valuedouble != (double)(long)it->valuedouble) {
        return false;
    }
    long v = (long)it->valuedouble;
    if (v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

static esp_err_t parse_obj(const cJSON *root, fsf_fixture_t *fx, char *err, size_t err_len)
{
    memset(fx, 0, sizeof(*fx));
    if (!cJSON_IsObject(root)) {
        snprintf(err, err_len, "Profile must be a JSON object");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *fmt = cJSON_GetObjectItemCaseSensitive(root, "format");
    if (!cJSON_IsString(fmt) || strcmp(fmt->valuestring, FSF_PROFILE_FORMAT) != 0) {
        snprintf(err, err_len, "'format' must be \"%s\"", FSF_PROFILE_FORMAT);
        return ESP_ERR_INVALID_ARG;
    }
    if (!get_str(root, "id", fx->id, sizeof(fx->id), true, err, err_len) ||
        !get_str(root, "manufacturer", fx->manufacturer, sizeof(fx->manufacturer), true, err, err_len) ||
        !get_str(root, "model", fx->model, sizeof(fx->model), true, err, err_len) ||
        !get_str(root, "mode", fx->mode, sizeof(fx->mode), false, err, err_len)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!id_valid(fx->id)) {
        snprintf(err, err_len, "'id' may only contain a-z, 0-9, '-', '_' and '.' (max %d)",
                 FSF_FIXTURE_ID_LEN - 1);
        return ESP_ERR_INVALID_ARG;
    }
    long v;
    if (!get_int(cJSON_GetObjectItemCaseSensitive(root, "footprint"), 1, 512, &v)) {
        snprintf(err, err_len, "'footprint' must be 1-512");
        return ESP_ERR_INVALID_ARG;
    }
    fx->footprint = (uint16_t)v;

    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "parameters");
    if (!cJSON_IsObject(params)) {
        snprintf(err, err_len, "'parameters' must be an object");
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *p;
    cJSON_ArrayForEach(p, params)
    {
        fsf_param_t pid;
        if (!fsf_param_from_key(p->string, &pid)) {
            snprintf(err, err_len, "Unknown parameter '%s' (use intensity, iris, zoom, edge, frost)",
                     p->string);
            return ESP_ERR_INVALID_ARG;
        }
        fsf_fixture_param_t *fp = &fx->params[pid];
        long bits = 8;
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(p, "bits");
        if (b && (!get_int(b, 8, 16, &bits) || (bits != 8 && bits != 16))) {
            snprintf(err, err_len, "%s.bits must be 8 or 16", p->string);
            return ESP_ERR_INVALID_ARG;
        }
        if (!get_int(cJSON_GetObjectItemCaseSensitive(p, "offset"), 1, fx->footprint - (bits == 16 ? 1 : 0),
                     &v)) {
            snprintf(err, err_len, "%s.offset must be inside the footprint (1-%u)", p->string,
                     fx->footprint - (bits == 16 ? 1 : 0));
            return ESP_ERR_INVALID_ARG;
        }
        fp->present = true;
        fp->offset = (uint16_t)v;
        fp->bits = (uint8_t)bits;
        long max = bits == 16 ? 65535 : 255;
        fp->from = 0;
        fp->to = (uint16_t)max;
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(p, "range");
        if (r) {
            long a, z;
            if (!cJSON_IsArray(r) || cJSON_GetArraySize(r) != 2 ||
                !get_int(cJSON_GetArrayItem(r, 0), 0, max, &a) ||
                !get_int(cJSON_GetArrayItem(r, 1), 0, max, &z) || a == z) {
                snprintf(err, err_len, "%s.range must be [from, to] within 0-%ld", p->string, max);
                return ESP_ERR_INVALID_ARG;
            }
            fp->from = (uint16_t)a;
            fp->to = (uint16_t)z;
        }
    }
    /* No two parameters may overlap. */
    for (int i = 0; i < FSF_PARAM_COUNT; i++) {
        for (int j = i + 1; j < FSF_PARAM_COUNT; j++) {
            const fsf_fixture_param_t *a = &fx->params[i], *c = &fx->params[j];
            if (!a->present || !c->present) {
                continue;
            }
            int a0 = a->offset, a1 = a->offset + (a->bits == 16), c0 = c->offset,
                c1 = c->offset + (c->bits == 16);
            if (a0 <= c1 && c0 <= a1) {
                snprintf(err, err_len, "Parameters %s and %s overlap", fsf_param_key(i), fsf_param_key(j));
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

esp_err_t fsf_fixture_parse(const char *json, size_t len, fsf_fixture_t *out, char *err, size_t err_len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        snprintf(err, err_len, "Not valid JSON");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t res = parse_obj(root, out, err, err_len);
    cJSON_Delete(root);
    return res;
}

/* ------------------------------------------------------------------------------------------ */

static int find_locked(const char *id)
{
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_fixtures[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool load_user_cb(const char *key, void *ctx)
{
    size_t len;
    char *json = fsf_storage_read(FSF_NS_FIXTURES, key, &len);
    if (!json) {
        return true;
    }
    fsf_fixture_t fx;
    char err[96];
    if (fsf_fixture_parse(json, len, &fx, err, sizeof(err)) == ESP_OK && find_locked(fx.id) < 0 &&
        s_count < FSF_MAX_FIXTURES) {
        fx.builtin = false;
        s_fixtures[s_count++] = fx;
    } else {
        ESP_LOGW(TAG, "skipping stored profile %s", key);
    }
    free(json);
    return true;
}

esp_err_t fsf_fixtures_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_fixtures = heap_caps_calloc(FSF_MAX_FIXTURES, sizeof(fsf_fixture_t), MALLOC_CAP_DEFAULT);
    if (!s_fixtures) {
        return ESP_ERR_NO_MEM;
    }
    cJSON *arr = cJSON_ParseWithLength(builtin_json_start, builtin_json_end - builtin_json_start);
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        char err[96];
        fsf_fixture_t fx;
        if (parse_obj(it, &fx, err, sizeof(err)) == ESP_OK && s_count < FSF_MAX_FIXTURES) {
            fx.builtin = true;
            s_fixtures[s_count++] = fx;
        } else {
            ESP_LOGE(TAG, "built-in profile invalid: %s", err);
        }
    }
    cJSON_Delete(arr);
    size_t builtins = s_count;
    fsf_storage_foreach(FSF_NS_FIXTURES, load_user_cb, NULL);
    ESP_LOGI(TAG, "%u built-in and %u user fixture profiles", (unsigned)builtins,
             (unsigned)(s_count - builtins));
    return ESP_OK;
}

size_t fsf_fixture_count(void)
{
    return s_count;
}

bool fsf_fixture_at(size_t index, fsf_fixture_t *out)
{
    lock();
    bool ok = index < s_count;
    if (ok) {
        *out = s_fixtures[index];
    }
    unlock();
    return ok;
}

bool fsf_fixture_get(const char *id, fsf_fixture_t *out)
{
    lock();
    int i = find_locked(id);
    if (i >= 0 && out) {
        *out = s_fixtures[i];
    }
    unlock();
    return i >= 0;
}

int fsf_fixture_index(const char *id)
{
    lock();
    int i = find_locked(id);
    unlock();
    return i;
}

esp_err_t fsf_fixture_install(const char *json, size_t len, char *id_out, size_t id_len, char *err,
                              size_t err_len)
{
    fsf_fixture_t fx;
    esp_err_t res = fsf_fixture_parse(json, len, &fx, err, err_len);
    if (res != ESP_OK) {
        return res;
    }
    if (id_out) {
        strlcpy(id_out, fx.id, id_len);
    }
    lock();
    int i = find_locked(fx.id);
    if (i >= 0 && s_fixtures[i].builtin) {
        unlock();
        snprintf(err, err_len, "'%s' is a built-in profile; use a different id", fx.id);
        return ESP_ERR_INVALID_STATE;
    }
    if (i < 0 && s_count >= FSF_MAX_FIXTURES) {
        unlock();
        snprintf(err, err_len, "Profile storage is full (%d user profiles)", FSF_MAX_USER_FIXTURES);
        return ESP_ERR_NO_MEM;
    }
    /* Store a normalised copy (unformatted) of the submitted object. */
    cJSON *root = cJSON_ParseWithLength(json, len);
    char *compact = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    char key[16];
    nvs_key_for(fx.id, key);
    res = compact ? fsf_storage_write(FSF_NS_FIXTURES, key, compact, strlen(compact)) : ESP_ERR_NO_MEM;
    free(compact);
    if (res == ESP_OK) {
        fx.builtin = false;
        if (i >= 0) {
            s_fixtures[i] = fx;
        } else {
            s_fixtures[s_count++] = fx;
        }
    } else {
        snprintf(err, err_len, "Saving to flash failed (%s)", esp_err_to_name(res));
    }
    unlock();
    return res;
}

esp_err_t fsf_fixture_delete(const char *id, char *err, size_t err_len)
{
    fsf_config_t cfg;
    fsf_config_get(&cfg);
    if (strcmp(cfg.fixture_id, id) == 0) {
        snprintf(err, err_len, "Profile is in use; select another fixture first");
        return ESP_ERR_INVALID_STATE;
    }
    lock();
    int i = find_locked(id);
    esp_err_t res = ESP_OK;
    if (i < 0) {
        snprintf(err, err_len, "No such profile");
        res = ESP_ERR_NOT_FOUND;
    } else if (s_fixtures[i].builtin) {
        snprintf(err, err_len, "Built-in profiles cannot be deleted");
        res = ESP_ERR_INVALID_STATE;
    } else {
        char key[16];
        nvs_key_for(id, key);
        res = fsf_storage_erase(FSF_NS_FIXTURES, key);
        if (res == ESP_OK || res == ESP_ERR_NVS_NOT_FOUND) {
            memmove(&s_fixtures[i], &s_fixtures[i + 1], (s_count - i - 1) * sizeof(fsf_fixture_t));
            s_count--;
            res = ESP_OK;
        } else {
            snprintf(err, err_len, "Erase failed (%s)", esp_err_to_name(res));
        }
    }
    unlock();
    return res;
}

char *fsf_fixture_json(const char *id)
{
    fsf_fixture_t fx;
    if (!fsf_fixture_get(id, &fx)) {
        return NULL;
    }
    if (!fx.builtin) {
        char key[16];
        nvs_key_for(id, key);
        char *s = fsf_storage_read(FSF_NS_FIXTURES, key, NULL);
        if (!s) {
            return NULL;
        }
        cJSON *root = cJSON_Parse(s);
        free(s);
        char *out = root ? cJSON_Print(root) : NULL;
        cJSON_Delete(root);
        return out;
    }
    char *out = NULL;
    cJSON *arr = cJSON_ParseWithLength(builtin_json_start, builtin_json_end - builtin_json_start);
    const cJSON *it;
    cJSON_ArrayForEach(it, arr)
    {
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
        if (cJSON_IsString(jid) && strcmp(jid->valuestring, id) == 0) {
            out = cJSON_Print(it);
            break;
        }
    }
    cJSON_Delete(arr);
    return out;
}

void fsf_fixture_display_name(const fsf_fixture_t *fx, char *buf, size_t len)
{
    if (fx->mode[0]) {
        snprintf(buf, len, "%s %s (%s)", fx->manufacturer, fx->model, fx->mode);
    } else {
        snprintf(buf, len, "%s %s", fx->manufacturer, fx->model);
    }
}
