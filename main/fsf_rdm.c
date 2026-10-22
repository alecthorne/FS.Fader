#include "fsf_rdm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "etcpal/pack.h"
#include "etcpal/uuid.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "rdm/defs.h"
#include "rdm/uid.h"
#include "rdmnet/common.h"
#include "rdmnet/defs.h"
#include "rdmnet/device.h"
#include "sdkconfig.h"
#include "fsf_config.h"
#include "fsf_engine.h"
#include "fsf_fixtures.h"
#include "fsf_net.h"

static const char *TAG = "rdmnet";

#define MANUFACTURER_LABEL "FS*Fader"
#define MODEL_DESCRIPTION "FS*Fader sACN fader (M5Stack Unit PoE-P4)"
#define DEVICE_MODEL_ID 0x0001
#define INTERFACE_ID 1
#define RESPONSE_BUF_SIZE 512

/* ------------------------------------------------------------------------------------------ */
/* State                                                                                       */

static rdmnet_device_t s_device = RDMNET_DEVICE_INVALID;
static bool s_running;
static uint32_t s_active_generation;
static fsf_config_t s_active_cfg;     /* RDMnet-relevant settings the stack was started with */
static fsf_config_t s_notified_cfg;   /* last config reported to controllers */
static bool s_notified_identify;
static uint8_t s_response_buf[RESPONSE_BUF_SIZE];
static RdmUid s_uid;
static EtcPalUuid s_cid;
static EtcPalLogParams s_log_params;
static volatile rdmnet_disconnect_reason_t s_reconfig_reason = kRdmnetDisconnectUserReconfigure;
static volatile int64_t s_reset_at_us;
static volatile bool s_reset_factory;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static fsf_rdm_status_t s_status;
static EtcPalSockAddr s_broker_addr;

/* E1.37-2 network settings staged until INTERFACE_APPLY_CONFIGURATION */
static bool s_pending_valid;
static fsf_net_config_t s_pending_net;

static const uint16_t SUPPORTED_PIDS[] = {
    E120_PARAMETER_DESCRIPTION,
    E120_DEVICE_MODEL_DESCRIPTION,
    E120_MANUFACTURER_LABEL,
    E120_DEVICE_LABEL,
    E120_FACTORY_DEFAULTS,
    E120_RESET_DEVICE,
    0x1050, /* E1.37-5 IDENTIFY_TIMEOUT */
    E137_2_LIST_INTERFACES,
    E137_2_INTERFACE_LABEL,
    E137_2_INTERFACE_HARDWARE_ADDRESS_TYPE1,
    E137_2_IPV4_DHCP_MODE,
    E137_2_IPV4_ZEROCONF_MODE,
    E137_2_IPV4_CURRENT_ADDRESS,
    E137_2_IPV4_STATIC_ADDRESS,
    E137_2_INTERFACE_RENEW_DHCP,
    E137_2_INTERFACE_APPLY_CONFIGURATION,
    E137_2_IPV4_DEFAULT_ROUTE,
    E137_2_DNS_HOSTNAME,
    E133_COMPONENT_SCOPE,
    E133_SEARCH_DOMAIN,
    E133_TCP_COMMS_STATUS,
    FSF_PID_CONTROLLED_PARAMETER,
    FSF_PID_INTENSITY_MODE,
    FSF_PID_SOURCE_UNIVERSE,
    FSF_PID_SOURCE_ADDRESS,
    FSF_PID_DEST_UNIVERSE,
    FSF_PID_DEST_ADDRESS,
    FSF_PID_DEST_PRIORITY,
    FSF_PID_FIXTURE_PROFILE,
    FSF_PID_FIXTURE_PROFILE_DESCRIPTION,
    FSF_PID_FIXTURE_PROFILE_ID,
    FSF_PID_LOCAL_LEVEL,
    FSF_PID_LED_BRIGHTNESS,
    FSF_PID_PARAMETER_VALUES,
};
#define PID_IDENTIFY_TIMEOUT 0x1050

typedef struct {
    uint16_t pid;
    uint8_t pdl;
    uint8_t data_type;
    uint8_t cc;
    uint32_t min, max, def;
    const char *desc;
} mfr_pid_desc_t;

static const mfr_pid_desc_t MFR_PIDS[] = {
    {FSF_PID_CONTROLLED_PARAMETER, 1, E120_DS_UNSIGNED_BYTE, E120_CC_GET_SET, 0, 4, 0,
     "Controlled Param 0=Int..4=Frost"},
    {FSF_PID_INTENSITY_MODE, 1, E120_DS_UNSIGNED_BYTE, E120_CC_GET_SET, 0, 1, 0,
     "Intensity Mode 0=Scaled 1=Local"},
    {FSF_PID_SOURCE_UNIVERSE, 2, E120_DS_UNSIGNED_WORD, E120_CC_GET_SET, 1, 63999, 1, "Source sACN Universe"},
    {FSF_PID_SOURCE_ADDRESS, 2, E120_DS_UNSIGNED_WORD, E120_CC_GET_SET, 1, 512, 1, "Source Start Address"},
    {FSF_PID_DEST_UNIVERSE, 2, E120_DS_UNSIGNED_WORD, E120_CC_GET_SET, 1, 63999, 2, "Destination sACN Universe"},
    {FSF_PID_DEST_ADDRESS, 2, E120_DS_UNSIGNED_WORD, E120_CC_GET_SET, 1, 512, 1, "Destination Start Address"},
    {FSF_PID_DEST_PRIORITY, 1, E120_DS_UNSIGNED_BYTE, E120_CC_GET_SET, 0, 200, 100, "Destination sACN Priority"},
    {FSF_PID_FIXTURE_PROFILE, 2, E120_DS_NOT_DEFINED, E120_CC_GET_SET, 1, FSF_MAX_FIXTURES, 1, "Fixture Profile"},
    {FSF_PID_FIXTURE_PROFILE_DESCRIPTION, 0x23, E120_DS_NOT_DEFINED, E120_CC_GET, 0, 0, 0,
     "Fixture Profile Description"},
    {FSF_PID_FIXTURE_PROFILE_ID, FSF_FIXTURE_ID_LEN - 1, E120_DS_ASCII, E120_CC_GET_SET, 0, 0, 0,
     "Fixture Profile ID"},
    {FSF_PID_LOCAL_LEVEL, 2, E120_DS_UNSIGNED_WORD, E120_CC_GET, 0, 65535, 0, "Local Fader Level"},
    {FSF_PID_LED_BRIGHTNESS, 1, E120_DS_UNSIGNED_BYTE, E120_CC_GET_SET, 1, 100, 40, "LED Brightness %"},
    {FSF_PID_PARAMETER_VALUES, 4, E120_DS_UNSIGNED_WORD, E120_CC_GET, 0, 65535, 0, "Received/Sent Param Value"},
};

/* ------------------------------------------------------------------------------------------ */
/* Helpers                                                                                     */

void fsf_device_cid(uint32_t index, uint8_t out[16])
{
    EtcPalUuid u;
    if (etcpal_generate_device_uuid("FS*Fader", fsf_net_mac(), index, &u) == kEtcPalErrOk) {
        memcpy(out, u.data, 16);
    } else {
        memset(out, 0, 16);
        memcpy(out, fsf_net_mac(), 6);
        out[15] = (uint8_t)index;
    }
}

static void set_event(const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", buf);
    taskENTER_CRITICAL(&s_mux);
    strlcpy(s_status.last_event, buf, sizeof(s_status.last_event));
    taskEXIT_CRITICAL(&s_mux);
}

static void log_cb(void *context, const EtcPalLogStrings *strings)
{
    ESP_LOGI("etc", "%s", strings->raw);
}

static uint8_t mask_to_cidr(uint32_t mask_n)
{
    uint32_t m = ntohl(mask_n);
    uint8_t n = 0;
    while (m & 0x80000000u) {
        n++;
        m <<= 1;
    }
    return n;
}

static uint32_t cidr_to_mask(uint8_t cidr)
{
    return cidr == 0 ? 0 : htonl(0xFFFFFFFFu << (32 - cidr));
}

#define ACK(len) RDMNET_SYNC_SEND_RDM_ACK(resp, (len))
#define NACK(reason) RDMNET_SYNC_SEND_RDM_NACK(resp, (reason))

/* Apply a config change from an RDM SET; NACKs on validation failure. */
static void commit(const fsf_config_t *cfg, RdmnetSyncRdmResponse *resp)
{
    char err[96];
    esp_err_t e = fsf_config_set(cfg, err, sizeof(err));
    if (e == ESP_OK) {
        ACK(0);
    } else if (e == ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "SET rejected: %s", err);
        NACK(kRdmNRDataOutOfRange);
    } else {
        NACK(kRdmNRHardwareFault);
    }
}

static size_t pack_str(uint8_t *buf, const char *s, size_t max)
{
    size_t n = strnlen(s, max);
    memcpy(buf, s, n);
    return n;
}

/* ------------------------------------------------------------------------------------------ */
/* GET handlers                                                                                */

static void handle_get(uint16_t pid, const uint8_t *pd, uint8_t pdl, uint8_t *out, RdmnetSyncRdmResponse *resp)
{
    fsf_config_t cfg;
    fsf_config_get(&cfg);
    fsf_net_status_t ns;
    fsf_net_get_status(&ns);
    uint8_t *p = out;

    switch (pid) {
    case E120_SUPPORTED_PARAMETERS:
        for (size_t i = 0; i < sizeof(SUPPORTED_PIDS) / sizeof(SUPPORTED_PIDS[0]); i++) {
            etcpal_pack_u16b(p, SUPPORTED_PIDS[i]);
            p += 2;
        }
        ACK(p - out);
        return;

    case E120_PARAMETER_DESCRIPTION: {
        if (pdl != 2) {
            NACK(kRdmNRFormatError);
            return;
        }
        uint16_t q = etcpal_unpack_u16b(pd);
        for (size_t i = 0; i < sizeof(MFR_PIDS) / sizeof(MFR_PIDS[0]); i++) {
            const mfr_pid_desc_t *d = &MFR_PIDS[i];
            if (d->pid != q) {
                continue;
            }
            etcpal_pack_u16b(p, d->pid);
            p[2] = d->pdl;
            p[3] = d->data_type;
            p[4] = d->cc;
            p[5] = 0; /* type (obsolete) */
            p[6] = E120_UNITS_NONE;
            p[7] = E120_PREFIX_NONE;
            etcpal_pack_u32b(p + 8, d->min);
            etcpal_pack_u32b(p + 12, d->max);
            etcpal_pack_u32b(p + 16, d->def);
            size_t n = pack_str(p + 20, d->desc, 32);
            ACK(20 + n);
            return;
        }
        NACK(kRdmNRDataOutOfRange);
        return;
    }

    case E120_DEVICE_INFO: {
        const esp_app_desc_t *app = esp_app_get_description();
        unsigned maj = 1, min = 0, pat = 0;
        sscanf(app->version, "%u.%u.%u", &maj, &min, &pat);
        etcpal_pack_u16b(p, 0x0100);                       /* RDM protocol version */
        etcpal_pack_u16b(p + 2, DEVICE_MODEL_ID);
        etcpal_pack_u16b(p + 4, E120_PRODUCT_CATEGORY_CONTROL);
        etcpal_pack_u32b(p + 6, (maj << 16) | (min << 8) | pat);
        etcpal_pack_u16b(p + 10, 0);      /* DMX512 footprint: no DMX512 port */
        etcpal_pack_u16b(p + 12, 0);      /* personality */
        etcpal_pack_u16b(p + 14, 0xFFFF); /* no DMX start address */
        etcpal_pack_u16b(p + 16, 0);      /* sub-devices */
        p[18] = 0;                        /* sensors */
        ACK(19);
        return;
    }

    case E120_DEVICE_MODEL_DESCRIPTION:
        ACK(pack_str(out, MODEL_DESCRIPTION, 32));
        return;
    case E120_MANUFACTURER_LABEL:
        ACK(pack_str(out, MANUFACTURER_LABEL, 32));
        return;
    case E120_DEVICE_LABEL:
        ACK(pack_str(out, cfg.device_label, 32));
        return;
    case E120_SOFTWARE_VERSION_LABEL: {
        char v[48];
        snprintf(v, sizeof(v), "FS*Fader %s", esp_app_get_description()->version);
        ACK(pack_str(out, v, 32));
        return;
    }
    case E120_FACTORY_DEFAULTS: {
        fsf_config_t d;
        fsf_config_defaults(&d);
        char *a = fsf_config_to_json(&d), *b = fsf_config_to_json(&cfg);
        out[0] = (a && b && strcmp(a, b) == 0) ? 1 : 0;
        free(a);
        free(b);
        ACK(1);
        return;
    }
    case E120_IDENTIFY_DEVICE:
        out[0] = fsf_identify_get() ? 1 : 0;
        ACK(1);
        return;
    case PID_IDENTIFY_TIMEOUT:
        etcpal_pack_u16b(out, cfg.identify_timeout_s);
        ACK(2);
        return;

    /* ---- E1.37-2 ---- */
    case E137_2_LIST_INTERFACES:
        etcpal_pack_u32b(p, INTERFACE_ID);
        etcpal_pack_u16b(p + 4, 0x0001); /* ARP hardware type: Ethernet */
        ACK(6);
        return;
    case E137_2_INTERFACE_LABEL:
    case E137_2_INTERFACE_HARDWARE_ADDRESS_TYPE1:
    case E137_2_IPV4_DHCP_MODE:
    case E137_2_IPV4_ZEROCONF_MODE:
    case E137_2_IPV4_CURRENT_ADDRESS:
    case E137_2_IPV4_STATIC_ADDRESS: {
        if (pdl != 4) {
            NACK(kRdmNRFormatError);
            return;
        }
        if (etcpal_unpack_u32b(pd) != INTERFACE_ID) {
            NACK(kRdmNRDataOutOfRange);
            return;
        }
        etcpal_pack_u32b(p, INTERFACE_ID);
        const fsf_net_config_t *pn = s_pending_valid ? &s_pending_net : &cfg.net;
        switch (pid) {
        case E137_2_INTERFACE_LABEL:
            ACK(4 + pack_str(p + 4, "Ethernet", 32));
            return;
        case E137_2_INTERFACE_HARDWARE_ADDRESS_TYPE1:
            memcpy(p + 4, fsf_net_mac(), 6);
            ACK(10);
            return;
        case E137_2_IPV4_DHCP_MODE:
            p[4] = pn->dhcp ? 1 : 0;
            ACK(5);
            return;
        case E137_2_IPV4_ZEROCONF_MODE:
            p[4] = pn->dhcp ? 1 : 0; /* link-local fallback runs alongside DHCP */
            ACK(5);
            return;
        case E137_2_IPV4_CURRENT_ADDRESS:
            memcpy(p + 4, &ns.ip, 4);
            p[8] = mask_to_cidr(ns.netmask);
            p[9] = cfg.net.dhcp ? (ns.has_ip && !ns.link_local ? 1 : 0) : 0;
            ACK(10);
            return;
        case E137_2_IPV4_STATIC_ADDRESS:
            memcpy(p + 4, &pn->ip, 4);
            p[8] = mask_to_cidr(pn->netmask);
            ACK(9);
            return;
        }
        return;
    }
    case E137_2_IPV4_DEFAULT_ROUTE: {
        const fsf_net_config_t *pn = s_pending_valid ? &s_pending_net : &cfg.net;
        uint32_t gw = pn->dhcp ? ns.gateway : pn->gateway;
        etcpal_pack_u32b(p, gw ? INTERFACE_ID : 0);
        memcpy(p + 4, &gw, 4);
        ACK(8);
        return;
    }
    case E137_2_DNS_HOSTNAME:
        ACK(pack_str(out, fsf_net_hostname(), 63));
        return;

    /* ---- E1.33 ---- */
    case E133_COMPONENT_SCOPE: {
        if (pdl != 2) {
            NACK(kRdmNRFormatError);
            return;
        }
        if (etcpal_unpack_u16b(pd) != 1) {
            NACK(kRdmNRDataOutOfRange);
            return;
        }
        memset(p, 0, 2 + E133_SCOPE_STRING_PADDED_LENGTH + 1 + 4 + 16 + 2);
        etcpal_pack_u16b(p, 1);
        memcpy(p + 2, cfg.scope, strnlen(cfg.scope, E133_SCOPE_STRING_PADDED_LENGTH - 1));
        uint8_t *q = p + 2 + E133_SCOPE_STRING_PADDED_LENGTH;
        if (cfg.static_broker) {
            *q++ = E133_STATIC_CONFIG_IPV4;
            memcpy(q, &cfg.static_broker_ip, 4);
            q += 4 + 16;
            etcpal_pack_u16b(q, cfg.static_broker_port);
        } else {
            *q++ = E133_NO_STATIC_CONFIG;
            q += 4 + 16;
            etcpal_pack_u16b(q, 0);
        }
        ACK(2 + E133_SCOPE_STRING_PADDED_LENGTH + 1 + 4 + 16 + 2);
        return;
    }
    case E133_SEARCH_DOMAIN:
        ACK(pack_str(out, cfg.search_domain, E133_DOMAIN_STRING_PADDED_LENGTH - 1));
        return;
    case E133_TCP_COMMS_STATUS: {
        memset(p, 0, E133_SCOPE_STRING_PADDED_LENGTH + 4 + 16 + 2 + 2);
        memcpy(p, cfg.scope, strnlen(cfg.scope, E133_SCOPE_STRING_PADDED_LENGTH - 1));
        uint8_t *q = p + E133_SCOPE_STRING_PADDED_LENGTH;
        taskENTER_CRITICAL(&s_mux);
        bool connected = s_status.connected;
        EtcPalSockAddr ba = s_broker_addr;
        uint16_t unhealthy = s_status.unhealthy_tcp_events;
        taskEXIT_CRITICAL(&s_mux);
        if (connected && ETCPAL_IP_IS_V4(&ba.ip)) {
            etcpal_pack_u32b(q, ETCPAL_IP_V4_ADDRESS(&ba.ip));
        }
        q += 4;
        if (connected && ETCPAL_IP_IS_V6(&ba.ip)) {
            memcpy(q, ETCPAL_IP_V6_ADDRESS(&ba.ip), 16);
        }
        q += 16;
        etcpal_pack_u16b(q, connected ? ba.port : 0);
        q += 2;
        etcpal_pack_u16b(q, unhealthy);
        q += 2;
        ACK(q - p);
        return;
    }

    /* ---- Manufacturer ---- */
    case FSF_PID_CONTROLLED_PARAMETER:
        out[0] = (uint8_t)cfg.parameter;
        ACK(1);
        return;
    case FSF_PID_INTENSITY_MODE:
        out[0] = (uint8_t)cfg.intensity_mode;
        ACK(1);
        return;
    case FSF_PID_SOURCE_UNIVERSE:
        etcpal_pack_u16b(out, cfg.src_universe);
        ACK(2);
        return;
    case FSF_PID_SOURCE_ADDRESS:
        etcpal_pack_u16b(out, cfg.src_address);
        ACK(2);
        return;
    case FSF_PID_DEST_UNIVERSE:
        etcpal_pack_u16b(out, cfg.dst_universe);
        ACK(2);
        return;
    case FSF_PID_DEST_ADDRESS:
        etcpal_pack_u16b(out, cfg.dst_address);
        ACK(2);
        return;
    case FSF_PID_DEST_PRIORITY:
        out[0] = cfg.dst_priority;
        ACK(1);
        return;
    case FSF_PID_FIXTURE_PROFILE: {
        int idx = fsf_fixture_index(cfg.fixture_id);
        size_t count = fsf_fixture_count();
        out[0] = (uint8_t)(idx + 1);
        out[1] = (uint8_t)(count > 255 ? 255 : count);
        ACK(2);
        return;
    }
    case FSF_PID_FIXTURE_PROFILE_DESCRIPTION: {
        if (pdl != 1) {
            NACK(kRdmNRFormatError);
            return;
        }
        fsf_fixture_t fx;
        if (pd[0] == 0 || !fsf_fixture_at(pd[0] - 1, &fx)) {
            NACK(kRdmNRDataOutOfRange);
            return;
        }
        out[0] = pd[0];
        etcpal_pack_u16b(out + 1, fx.footprint);
        char name[160];
        fsf_fixture_display_name(&fx, name, sizeof(name));
        ACK(3 + pack_str(out + 3, name, 64));
        return;
    }
    case FSF_PID_FIXTURE_PROFILE_ID:
        ACK(pack_str(out, cfg.fixture_id, FSF_FIXTURE_ID_LEN - 1));
        return;
    case FSF_PID_LOCAL_LEVEL: {
        fsf_engine_status_t es;
        fsf_engine_get_status(&es);
        etcpal_pack_u16b(out, es.fader);
        ACK(2);
        return;
    }
    case FSF_PID_LED_BRIGHTNESS:
        out[0] = cfg.led_brightness;
        ACK(1);
        return;
    case FSF_PID_PARAMETER_VALUES: {
        fsf_engine_status_t es;
        fsf_engine_get_status(&es);
        etcpal_pack_u16b(out, es.rx_value);
        etcpal_pack_u16b(out + 2, es.tx_value);
        ACK(4);
        return;
    }
    default:
        NACK(kRdmNRUnknownPid);
        return;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* SET handlers                                                                                */

static bool is_settable(uint16_t pid)
{
    switch (pid) {
    case E120_DEVICE_LABEL:
    case E120_FACTORY_DEFAULTS:
    case E120_RESET_DEVICE:
    case E120_IDENTIFY_DEVICE:
    case PID_IDENTIFY_TIMEOUT:
    case E137_2_IPV4_DHCP_MODE:
    case E137_2_IPV4_STATIC_ADDRESS:
    case E137_2_INTERFACE_RENEW_DHCP:
    case E137_2_INTERFACE_APPLY_CONFIGURATION:
    case E137_2_IPV4_DEFAULT_ROUTE:
    case E133_COMPONENT_SCOPE:
    case E133_SEARCH_DOMAIN:
    case E133_TCP_COMMS_STATUS:
    case FSF_PID_CONTROLLED_PARAMETER:
    case FSF_PID_INTENSITY_MODE:
    case FSF_PID_SOURCE_UNIVERSE:
    case FSF_PID_SOURCE_ADDRESS:
    case FSF_PID_DEST_UNIVERSE:
    case FSF_PID_DEST_ADDRESS:
    case FSF_PID_DEST_PRIORITY:
    case FSF_PID_FIXTURE_PROFILE:
    case FSF_PID_FIXTURE_PROFILE_ID:
    case FSF_PID_LED_BRIGHTNESS:
        return true;
    default:
        return false;
    }
}

static bool is_known(uint16_t pid)
{
    if (pid == E120_SUPPORTED_PARAMETERS || pid == E120_DEVICE_INFO || pid == E120_SOFTWARE_VERSION_LABEL ||
        pid == E120_IDENTIFY_DEVICE) {
        return true;
    }
    for (size_t i = 0; i < sizeof(SUPPORTED_PIDS) / sizeof(SUPPORTED_PIDS[0]); i++) {
        if (SUPPORTED_PIDS[i] == pid) {
            return true;
        }
    }
    return false;
}

static bool u16_param(const uint8_t *pd, uint8_t pdl, uint16_t lo, uint16_t hi, uint16_t *out,
                      RdmnetSyncRdmResponse *resp)
{
    if (pdl != 2) {
        NACK(kRdmNRFormatError);
        return false;
    }
    uint16_t v = etcpal_unpack_u16b(pd);
    if (v < lo || v > hi) {
        NACK(kRdmNRDataOutOfRange);
        return false;
    }
    *out = v;
    return true;
}

static bool u8_param(const uint8_t *pd, uint8_t pdl, uint8_t lo, uint8_t hi, uint8_t *out,
                     RdmnetSyncRdmResponse *resp)
{
    if (pdl != 1) {
        NACK(kRdmNRFormatError);
        return false;
    }
    if (pd[0] < lo || pd[0] > hi) {
        NACK(kRdmNRDataOutOfRange);
        return false;
    }
    *out = pd[0];
    return true;
}

static void handle_set(uint16_t pid, const uint8_t *pd, uint8_t pdl, bool llrp, RdmnetSyncRdmResponse *resp)
{
    fsf_config_t cfg;
    fsf_config_get(&cfg);
    uint8_t u8;
    uint16_t u16;

    switch (pid) {
    case E120_DEVICE_LABEL:
        if (pdl > FSF_DEVICE_LABEL_LEN) {
            NACK(kRdmNRFormatError);
            return;
        }
        memcpy(cfg.device_label, pd, pdl);
        cfg.device_label[pdl] = '\0';
        if (pdl == 0) {
            strlcpy(cfg.device_label, "FS*Fader", sizeof(cfg.device_label));
        }
        commit(&cfg, resp);
        return;

    case E120_FACTORY_DEFAULTS:
        if (pdl != 0) {
            NACK(kRdmNRFormatError);
            return;
        }
        s_reset_factory = true;
        s_reset_at_us = esp_timer_get_time() + 1000000;
        ACK(0);
        return;

    case E120_RESET_DEVICE:
        if (pdl != 1) {
            NACK(kRdmNRFormatError);
            return;
        }
        if (pd[0] != 0x01 && pd[0] != 0xFF) {
            NACK(kRdmNRDataOutOfRange);
            return;
        }
        s_reset_at_us = esp_timer_get_time() + 1000000;
        ACK(0);
        return;

    case E120_IDENTIFY_DEVICE:
        if (!u8_param(pd, pdl, 0, 1, &u8, resp)) {
            return;
        }
        fsf_identify_set(u8 != 0);
        ACK(0);
        return;

    case PID_IDENTIFY_TIMEOUT:
        if (!u16_param(pd, pdl, 0, 65535, &u16, resp)) {
            return;
        }
        cfg.identify_timeout_s = u16;
        commit(&cfg, resp);
        return;

    /* ---- E1.37-2: staged until INTERFACE_APPLY_CONFIGURATION ---- */
    case E137_2_IPV4_DHCP_MODE:
    case E137_2_IPV4_STATIC_ADDRESS:
    case E137_2_INTERFACE_RENEW_DHCP:
    case E137_2_INTERFACE_APPLY_CONFIGURATION: {
        size_t need = pid == E137_2_IPV4_DHCP_MODE ? 5 : pid == E137_2_IPV4_STATIC_ADDRESS ? 9 : 4;
        if (pdl != need) {
            NACK(kRdmNRFormatError);
            return;
        }
        if (etcpal_unpack_u32b(pd) != INTERFACE_ID) {
            NACK(kRdmNRDataOutOfRange);
            return;
        }
        if (!s_pending_valid) {
            s_pending_net = cfg.net;
            s_pending_valid = true;
        }
        if (pid == E137_2_IPV4_DHCP_MODE) {
            if (pd[4] > 1) {
                NACK(kRdmNRDataOutOfRange);
                return;
            }
            s_pending_net.dhcp = pd[4] == 1;
            ACK(0);
        } else if (pid == E137_2_IPV4_STATIC_ADDRESS) {
            uint32_t ip;
            memcpy(&ip, pd + 4, 4);
            uint8_t cidr = pd[8];
            if (cidr < 1 || cidr > 31) {
                NACK(kRdmNRDataOutOfRange);
                return;
            }
            s_pending_net.ip = ip;
            s_pending_net.netmask = cidr_to_mask(cidr);
            ACK(0);
        } else if (pid == E137_2_INTERFACE_RENEW_DHCP) {
            if (!cfg.net.dhcp) {
                NACK(kRdmNRActionNotSupported);
                return;
            }
            fsf_net_renew_dhcp();
            ACK(0);
        } else {
            fsf_config_t n = cfg;
            n.net = s_pending_net;
            s_pending_valid = false;
            commit(&n, resp);
        }
        return;
    }
    case E137_2_IPV4_DEFAULT_ROUTE: {
        if (pdl != 8) {
            NACK(kRdmNRFormatError);
            return;
        }
        uint32_t ifid = etcpal_unpack_u32b(pd);
        if (ifid != INTERFACE_ID && ifid != 0) {
            NACK(kRdmNRDataOutOfRange);
            return;
        }
        if (!s_pending_valid) {
            s_pending_net = cfg.net;
            s_pending_valid = true;
        }
        uint32_t gw;
        memcpy(&gw, pd + 4, 4);
        s_pending_net.gateway = ifid == 0 ? 0 : gw;
        ACK(0);
        return;
    }

    /* ---- E1.33 ---- */
    case E133_COMPONENT_SCOPE: {
        if (pdl != 2 + E133_SCOPE_STRING_PADDED_LENGTH + 1 + 4 + 16 + 2) {
            NACK(kRdmNRFormatError);
            return;
        }
        if (etcpal_unpack_u16b(pd) != 1) {
            NACK(kRdmNRDataOutOfRange);
            return;
        }
        char scope[E133_SCOPE_STRING_PADDED_LENGTH];
        memcpy(scope, pd + 2, sizeof(scope));
        scope[sizeof(scope) - 1] = '\0';
        if (scope[0] == '\0') {
            NACK(kRdmNRUnknownScope);
            return;
        }
        const uint8_t *q = pd + 2 + E133_SCOPE_STRING_PADDED_LENGTH;
        uint8_t type = *q++;
        uint32_t ip;
        memcpy(&ip, q, 4);
        uint16_t port = etcpal_unpack_u16b(q + 4 + 16);
        if (type == E133_STATIC_CONFIG_IPV4) {
            if (ip == 0 || ip == 0xFFFFFFFF) {
                NACK(kRdmNRInvalidIpv4Address);
                return;
            }
            if (port == 0) {
                NACK(kRdmNRInvalidPort);
                return;
            }
            cfg.static_broker = true;
            cfg.static_broker_ip = ip;
            cfg.static_broker_port = port;
        } else if (type == E133_NO_STATIC_CONFIG) {
            cfg.static_broker = false;
            cfg.static_broker_ip = 0;
            cfg.static_broker_port = 0;
        } else {
            NACK(kRdmNRInvalidStaticConfigType); /* IPv6 static broker not supported */
            return;
        }
        strlcpy(cfg.scope, scope, sizeof(cfg.scope));
        s_reconfig_reason = llrp ? kRdmnetDisconnectLlrpReconfigure : kRdmnetDisconnectRptReconfigure;
        commit(&cfg, resp);
        return;
    }
    case E133_SEARCH_DOMAIN:
        if (pdl == 0 || pdl > FSF_DOMAIN_LEN) {
            NACK(pdl == 0 ? kRdmNRDataOutOfRange : kRdmNRFormatError);
            return;
        }
        memcpy(cfg.search_domain, pd, pdl);
        cfg.search_domain[pdl] = '\0';
        s_reconfig_reason = llrp ? kRdmnetDisconnectLlrpReconfigure : kRdmnetDisconnectRptReconfigure;
        commit(&cfg, resp);
        return;
    case E133_TCP_COMMS_STATUS: {
        /* SET with a scope resets the unhealthy-event counter for that scope. */
        if (pdl != E133_SCOPE_STRING_PADDED_LENGTH) {
            NACK(kRdmNRFormatError);
            return;
        }
        char scope[E133_SCOPE_STRING_PADDED_LENGTH];
        memcpy(scope, pd, sizeof(scope));
        scope[sizeof(scope) - 1] = '\0';
        if (strcmp(scope, cfg.scope) != 0) {
            NACK(kRdmNRUnknownScope);
            return;
        }
        taskENTER_CRITICAL(&s_mux);
        s_status.unhealthy_tcp_events = 0;
        taskEXIT_CRITICAL(&s_mux);
        ACK(0);
        return;
    }

    /* ---- Manufacturer ---- */
    case FSF_PID_CONTROLLED_PARAMETER:
        if (u8_param(pd, pdl, 0, FSF_PARAM_COUNT - 1, &u8, resp)) {
            cfg.parameter = (fsf_param_t)u8;
            commit(&cfg, resp);
        }
        return;
    case FSF_PID_INTENSITY_MODE:
        if (u8_param(pd, pdl, 0, 1, &u8, resp)) {
            cfg.intensity_mode = (fsf_intensity_mode_t)u8;
            commit(&cfg, resp);
        }
        return;
    case FSF_PID_SOURCE_UNIVERSE:
        if (u16_param(pd, pdl, 1, 63999, &u16, resp)) {
            cfg.src_universe = u16;
            commit(&cfg, resp);
        }
        return;
    case FSF_PID_SOURCE_ADDRESS:
        if (u16_param(pd, pdl, 1, 512, &u16, resp)) {
            cfg.src_address = u16;
            commit(&cfg, resp);
        }
        return;
    case FSF_PID_DEST_UNIVERSE:
        if (u16_param(pd, pdl, 1, 63999, &u16, resp)) {
            cfg.dst_universe = u16;
            commit(&cfg, resp);
        }
        return;
    case FSF_PID_DEST_ADDRESS:
        if (u16_param(pd, pdl, 1, 512, &u16, resp)) {
            cfg.dst_address = u16;
            commit(&cfg, resp);
        }
        return;
    case FSF_PID_DEST_PRIORITY:
        if (u8_param(pd, pdl, 0, 200, &u8, resp)) {
            cfg.dst_priority = u8;
            commit(&cfg, resp);
        }
        return;
    case FSF_PID_FIXTURE_PROFILE: {
        fsf_fixture_t fx;
        if (pdl != 1) {
            NACK(kRdmNRFormatError);
            return;
        }
        if (pd[0] == 0 || !fsf_fixture_at(pd[0] - 1, &fx)) {
            NACK(kRdmNRDataOutOfRange);
            return;
        }
        strlcpy(cfg.fixture_id, fx.id, sizeof(cfg.fixture_id));
        commit(&cfg, resp);
        return;
    }
    case FSF_PID_FIXTURE_PROFILE_ID:
        if (pdl == 0 || pdl >= FSF_FIXTURE_ID_LEN) {
            NACK(kRdmNRFormatError);
            return;
        }
        memcpy(cfg.fixture_id, pd, pdl);
        cfg.fixture_id[pdl] = '\0';
        commit(&cfg, resp);
        return;
    case FSF_PID_LED_BRIGHTNESS:
        if (u8_param(pd, pdl, 1, 100, &u8, resp)) {
            cfg.led_brightness = u8;
            commit(&cfg, resp);
        }
        return;
    default:
        NACK(is_known(pid) ? kRdmNRUnsupportedCommandClass : kRdmNRUnknownPid);
        return;
    }
}

static void handle_command(const RdmCommandHeader *hdr, const uint8_t *data, uint8_t data_len, bool llrp,
                           RdmnetSyncRdmResponse *resp)
{
    if (hdr->subdevice != 0 && hdr->subdevice != 0xFFFF) {
        NACK(kRdmNRSubDeviceOutOfRange);
        return;
    }
    if (hdr->command_class == kRdmCCGetCommand) {
        if (hdr->subdevice == 0xFFFF) {
            NACK(kRdmNRSubDeviceOutOfRange); /* GET to all sub-devices is not allowed */
            return;
        }
        if (!is_known(hdr->param_id)) {
            NACK(kRdmNRUnknownPid);
            return;
        }
        /* Fixed-size GETs that take no parameter data */
        handle_get(hdr->param_id, data, data_len, s_response_buf, resp);
    } else if (hdr->command_class == kRdmCCSetCommand) {
        if (!is_known(hdr->param_id)) {
            NACK(kRdmNRUnknownPid);
            return;
        }
        if (!is_settable(hdr->param_id)) {
            NACK(kRdmNRUnsupportedCommandClass);
            return;
        }
        handle_set(hdr->param_id, data, data_len, llrp, resp);
    } else {
        NACK(kRdmNRUnsupportedCommandClass);
    }
}

/* ------------------------------------------------------------------------------------------ */
/* RDMnet callbacks                                                                            */

static void cb_connected(rdmnet_device_t h, const RdmnetClientConnectedInfo *info, void *ctx)
{
    char ip[ETCPAL_IP_STRING_BYTES] = "";
    etcpal_ip_to_string(&info->broker_addr.ip, ip);
    taskENTER_CRITICAL(&s_mux);
    s_status.connected = true;
    s_broker_addr = info->broker_addr;
    snprintf(s_status.broker, sizeof(s_status.broker), "%s:%u", ip, info->broker_addr.port);
    taskEXIT_CRITICAL(&s_mux);
    set_event("Connected to broker %s:%u", ip, info->broker_addr.port);
}

static void cb_connect_failed(rdmnet_device_t h, const RdmnetClientConnectFailedInfo *info, void *ctx)
{
    set_event("Broker connect failed: %s%s", rdmnet_connect_fail_event_to_string(info->event),
              info->will_retry ? " (retrying)" : "");
}

static void cb_disconnected(rdmnet_device_t h, const RdmnetClientDisconnectedInfo *info, void *ctx)
{
    taskENTER_CRITICAL(&s_mux);
    s_status.connected = false;
    s_status.broker[0] = '\0';
    if (info->event == kRdmnetDisconnectNoHeartbeat || info->event == kRdmnetDisconnectAbruptClose) {
        if (s_status.unhealthy_tcp_events < 0xFFFF) {
            s_status.unhealthy_tcp_events++;
        }
    }
    taskEXIT_CRITICAL(&s_mux);
    set_event("Disconnected from broker: %s%s", rdmnet_disconnect_event_to_string(info->event),
              info->will_retry ? " (retrying)" : "");
}

static void cb_rdm(rdmnet_device_t h, const RdmnetRdmCommand *cmd, RdmnetSyncRdmResponse *resp, void *ctx)
{
    if (!RDMNET_COMMAND_IS_TO_DEFAULT_RESPONDER(cmd)) {
        NACK(kRdmNREndpointNumberInvalid);
        return;
    }
    handle_command(&cmd->rdm_header, cmd->data, cmd->data_len, false, resp);
}

static void cb_llrp_rdm(rdmnet_device_t h, const LlrpRdmCommand *cmd, RdmnetSyncRdmResponse *resp, void *ctx)
{
    handle_command(&cmd->rdm_header, cmd->data, cmd->data_len, true, resp);
}

/* ------------------------------------------------------------------------------------------ */
/* Lifecycle                                                                                   */

static void scope_config_from(const fsf_config_t *cfg, RdmnetScopeConfig *sc)
{
    sc->scope = cfg->scope;
    if (cfg->static_broker) {
        ETCPAL_IP_SET_V4_ADDRESS(&sc->static_broker_addr.ip, ntohl(cfg->static_broker_ip));
        sc->static_broker_addr.port = cfg->static_broker_port;
    } else {
        ETCPAL_IP_SET_INVALID(&sc->static_broker_addr.ip);
        sc->static_broker_addr.port = 0;
    }
}

static void stop_stack(rdmnet_disconnect_reason_t reason)
{
    if (!s_running) {
        return;
    }
    rdmnet_device_destroy(s_device, reason);
    rdmnet_deinit();
    s_device = RDMNET_DEVICE_INVALID;
    s_running = false;
    taskENTER_CRITICAL(&s_mux);
    s_status.running = false;
    s_status.connected = false;
    s_status.broker[0] = '\0';
    taskEXIT_CRITICAL(&s_mux);
}

static bool start_stack(const fsf_config_t *cfg)
{
    /* Restrict RDMnet (LLRP multicast and broker discovery) to the Ethernet interface; lwIP's
     * loopback interface would otherwise be enumerated too. */
    EtcPalMcastNetintId netint = {.ip_type = kEtcPalIpTypeV4,
                                  .index = (unsigned)esp_netif_get_netif_impl_index(fsf_net_netif())};
    RdmnetNetintConfig netint_cfg = {.netints = &netint, .num_netints = 1, .no_netints = false};
    etcpal_error_t res = rdmnet_init(&s_log_params, &netint_cfg);
    if (res != kEtcPalErrOk) {
        set_event("RDMnet init failed: %s", etcpal_strerror(res));
        return false;
    }
    RdmnetDeviceConfig dc = RDMNET_DEVICE_CONFIG_DEFAULT_INIT(CONFIG_FSF_RDM_MANUFACTURER_ID);
    dc.cid = s_cid;
    dc.uid = s_uid; /* static UID */
    dc.response_buf = s_response_buf;
    dc.search_domain = cfg->search_domain;
    scope_config_from(cfg, &dc.scope_config);
    rdmnet_device_set_callbacks(&dc, cb_connected, cb_connect_failed, cb_disconnected, cb_rdm, cb_llrp_rdm,
                                NULL, NULL);
    res = rdmnet_device_create(&dc, &s_device);
    if (res != kEtcPalErrOk) {
        set_event("RDMnet device create failed: %s", etcpal_strerror(res));
        rdmnet_deinit();
        return false;
    }
    s_running = true;
    s_active_cfg = *cfg;
    taskENTER_CRITICAL(&s_mux);
    s_status.running = true;
    taskEXIT_CRITICAL(&s_mux);
    set_event("RDMnet started on scope '%s'%s", cfg->scope, cfg->static_broker ? " (static broker)" : "");
    return true;
}

/* Tell connected controllers about values changed outside RDMnet (web UI, other controller). */
static void notify_changes(const fsf_config_t *cfg)
{
    if (!s_running) {
        s_notified_cfg = *cfg;
        return;
    }
    uint8_t b[64];
    const fsf_config_t *o = &s_notified_cfg;
#define UPD(pid, len) rdmnet_device_send_rdm_update(s_device, 0, (pid), b, (len))
    if (strcmp(o->device_label, cfg->device_label) != 0) {
        UPD(E120_DEVICE_LABEL, pack_str(b, cfg->device_label, 32));
    }
    if (o->parameter != cfg->parameter) {
        b[0] = cfg->parameter;
        UPD(FSF_PID_CONTROLLED_PARAMETER, 1);
    }
    if (o->intensity_mode != cfg->intensity_mode) {
        b[0] = cfg->intensity_mode;
        UPD(FSF_PID_INTENSITY_MODE, 1);
    }
    if (o->src_universe != cfg->src_universe) {
        etcpal_pack_u16b(b, cfg->src_universe);
        UPD(FSF_PID_SOURCE_UNIVERSE, 2);
    }
    if (o->src_address != cfg->src_address) {
        etcpal_pack_u16b(b, cfg->src_address);
        UPD(FSF_PID_SOURCE_ADDRESS, 2);
    }
    if (o->dst_universe != cfg->dst_universe) {
        etcpal_pack_u16b(b, cfg->dst_universe);
        UPD(FSF_PID_DEST_UNIVERSE, 2);
    }
    if (o->dst_address != cfg->dst_address) {
        etcpal_pack_u16b(b, cfg->dst_address);
        UPD(FSF_PID_DEST_ADDRESS, 2);
    }
    if (o->dst_priority != cfg->dst_priority) {
        b[0] = cfg->dst_priority;
        UPD(FSF_PID_DEST_PRIORITY, 1);
    }
    if (strcmp(o->fixture_id, cfg->fixture_id) != 0) {
        UPD(FSF_PID_FIXTURE_PROFILE_ID, pack_str(b, cfg->fixture_id, FSF_FIXTURE_ID_LEN - 1));
    }
    if (o->led_brightness != cfg->led_brightness) {
        b[0] = cfg->led_brightness;
        UPD(FSF_PID_LED_BRIGHTNESS, 1);
    }
    if (o->identify_timeout_s != cfg->identify_timeout_s) {
        etcpal_pack_u16b(b, cfg->identify_timeout_s);
        UPD(PID_IDENTIFY_TIMEOUT, 2);
    }
    bool ident = fsf_identify_get();
    if (ident != s_notified_identify) {
        b[0] = ident;
        UPD(E120_IDENTIFY_DEVICE, 1);
        s_notified_identify = ident;
    }
#undef UPD
    s_notified_cfg = *cfg;
}

static void manager_task(void *arg)
{
    fsf_config_t cfg;
    fsf_config_get(&s_notified_cfg);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        fsf_config_get(&cfg);
        fsf_net_status_t ns;
        fsf_net_get_status(&ns);

        if (s_reset_at_us && esp_timer_get_time() >= s_reset_at_us) {
            stop_stack(kRdmnetDisconnectSoftwareReset);
            if (s_reset_factory) {
                fsf_config_factory_reset();
            }
            esp_restart();
        }

        if (!ns.has_ip) {
            if (s_running) {
                set_event("No IP address; RDMnet stopped");
                stop_stack(kRdmnetDisconnectShutdown);
            }
            continue;
        }
        if (s_running && ns.generation != s_active_generation) {
            /* The address changed (e.g. DHCP lease after link-local): restart so LLRP and the
             * broker connection bind to the new address. */
            stop_stack(kRdmnetDisconnectUserReconfigure);
        }
        if (!s_running) {
            if (start_stack(&cfg)) {
                s_active_generation = ns.generation;
            } else {
                vTaskDelay(pdMS_TO_TICKS(5000));
            }
            continue;
        }

        bool scope_changed = strcmp(cfg.scope, s_active_cfg.scope) != 0 ||
                             cfg.static_broker != s_active_cfg.static_broker ||
                             cfg.static_broker_ip != s_active_cfg.static_broker_ip ||
                             cfg.static_broker_port != s_active_cfg.static_broker_port;
        if (scope_changed) {
            RdmnetScopeConfig sc;
            scope_config_from(&cfg, &sc);
            etcpal_error_t r = rdmnet_device_change_scope(s_device, &sc, s_reconfig_reason);
            set_event("Scope changed to '%s' (%s)", cfg.scope, etcpal_strerror(r));
            s_reconfig_reason = kRdmnetDisconnectUserReconfigure;
            strlcpy(s_active_cfg.scope, cfg.scope, sizeof(s_active_cfg.scope));
            s_active_cfg.static_broker = cfg.static_broker;
            s_active_cfg.static_broker_ip = cfg.static_broker_ip;
            s_active_cfg.static_broker_port = cfg.static_broker_port;
        }
        if (strcmp(cfg.search_domain, s_active_cfg.search_domain) != 0) {
            etcpal_error_t r = rdmnet_device_change_search_domain(s_device, cfg.search_domain, s_reconfig_reason);
            set_event("Search domain changed to '%s' (%s)", cfg.search_domain, etcpal_strerror(r));
            s_reconfig_reason = kRdmnetDisconnectUserReconfigure;
            strlcpy(s_active_cfg.search_domain, cfg.search_domain, sizeof(s_active_cfg.search_domain));
        }
        notify_changes(&cfg);
    }
}

esp_err_t fsf_rdm_init(void)
{
    const uint8_t *mac = fsf_net_mac();
    s_uid.manu = CONFIG_FSF_RDM_MANUFACTURER_ID;
    s_uid.id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    fsf_device_cid(0, s_cid.data);

    s_log_params = (EtcPalLogParams)ETCPAL_LOG_PARAMS_INIT;
    s_log_params.action = ETCPAL_LOG_CREATE_HUMAN_READABLE;
    s_log_params.log_fn = log_cb;
    s_log_params.log_mask = ETCPAL_LOG_UPTO(ETCPAL_LOG_NOTICE);

    etcpal_error_t r = etcpal_init(ETCPAL_FEATURE_LOGGING);
    if (r != kEtcPalErrOk) {
        ESP_LOGW(TAG, "etcpal_init: %s", etcpal_strerror(r));
    }

    snprintf(s_status.uid, sizeof(s_status.uid), "%04X:%08lX", s_uid.manu, (unsigned long)s_uid.id);
    etcpal_uuid_to_string(&s_cid, s_status.cid);
    ESP_LOGI(TAG, "UID %s CID %s", s_status.uid, s_status.cid);

    if (xTaskCreate(manager_task, "rdmnet_mgr", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void fsf_rdm_get_status(fsf_rdm_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}
