#include "fsf_sacn.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "fsf_config.h"
#include "fsf_net.h"

static const char *TAG = "sacn";

/* ---- E1.31 constants ---- */
#define ACN_PREAMBLE_SIZE 0x0010
#define VECTOR_ROOT_E131_DATA 0x00000004
#define VECTOR_ROOT_E131_EXTENDED 0x00000008
#define VECTOR_E131_DATA_PACKET 0x00000002
#define VECTOR_E131_EXTENDED_DISCOVERY 0x00000002
#define VECTOR_DMP_SET_PROPERTY 0x02
#define VECTOR_UNIVERSE_DISCOVERY_UNIVERSE_LIST 0x00000001
#define DISCOVERY_UNIVERSE 64214
#define NETWORK_DATA_LOSS_TIMEOUT_US 2500000LL
#define OPT_PREVIEW 0x80
#define OPT_TERMINATED 0x40

static const uint8_t ACN_PID[12] = {0x41, 0x53, 0x43, 0x2d, 0x45, 0x31, 0x2e, 0x31, 0x37, 0x00, 0x00, 0x00};

/* Byte offsets in a data packet */
#define OFS_ROOT_FLEN 16
#define OFS_ROOT_VECTOR 18
#define OFS_CID 22
#define OFS_FRAME_FLEN 38
#define OFS_FRAME_VECTOR 40
#define OFS_SOURCE_NAME 44
#define OFS_PRIORITY 108
#define OFS_SYNC 109
#define OFS_SEQ 111
#define OFS_OPTIONS 112
#define OFS_UNIVERSE 113
#define OFS_DMP_FLEN 115
#define OFS_DMP_VECTOR 117
#define OFS_DMP_TYPE 118
#define OFS_DMP_FIRST 119
#define OFS_DMP_INC 121
#define OFS_DMP_COUNT 123
#define OFS_START_CODE 125
#define OFS_SLOTS 126
#define DATA_PACKET_LEN (OFS_SLOTS + 512)

static inline uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xFF;
}
static inline void wr32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

static uint32_t mcast_group(uint16_t universe)
{
    return htonl(0xEFFF0000u | universe); /* 239.255.hi.lo */
}

/* ---- receiver state ---- */
typedef struct {
    bool used;
    uint8_t cid[16];
    char name[FSF_SACN_SOURCE_NAME_LEN];
    uint8_t priority;
    uint8_t last_seq;
    uint32_t ip;
    int64_t last_us;
    uint16_t slots;
    uint8_t data[512];
} source_t;

static SemaphoreHandle_t s_rx_lock;
static source_t s_sources[FSF_SACN_MAX_SOURCES];
static uint8_t s_merged[512];
static fsf_sacn_rx_status_t s_rx_status;
static uint8_t s_cid[16];
static char s_source_name[FSF_SACN_SOURCE_NAME_LEN];

/* ---- transmitter state ---- */
static int s_tx_sock = -1;
static uint32_t s_tx_if_generation = UINT32_MAX;
static uint8_t s_tx_buf[DATA_PACKET_LEN];
static uint16_t s_seq_universe[2]; /* small cache: universe -> sequence */
static uint8_t s_seq[2];

/* Must be called with s_rx_lock held. */
static void merge_locked(int64_t now)
{
    int best = -1;
    int count = 0;
    for (int i = 0; i < FSF_SACN_MAX_SOURCES; i++) {
        if (!s_sources[i].used) {
            continue;
        }
        count++;
        if (best < 0 || s_sources[i].priority > best) {
            best = s_sources[i].priority;
        }
    }
    s_rx_status.num_sources = count;
    s_rx_status.source_present = count > 0;
    if (count == 0) {
        return; /* hold last look */
    }
    s_rx_status.priority = (uint8_t)best;
    /* Highest priority wins; equal-priority sources are merged highest-takes-precedence. */
    uint8_t out[512] = {0};
    for (int i = 0; i < FSF_SACN_MAX_SOURCES; i++) {
        const source_t *s = &s_sources[i];
        if (!s->used || s->priority != best) {
            continue;
        }
        for (int k = 0; k < s->slots; k++) {
            if (s->data[k] > out[k]) {
                out[k] = s->data[k];
            }
        }
    }
    memcpy(s_merged, out, sizeof(out));
    s_rx_status.have_data = true;
    (void)now;
}

static void expire_sources(int64_t now)
{
    bool changed = false;
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    for (int i = 0; i < FSF_SACN_MAX_SOURCES; i++) {
        if (s_sources[i].used && now - s_sources[i].last_us > NETWORK_DATA_LOSS_TIMEOUT_US) {
            ESP_LOGW(TAG, "source '%s' timed out", s_sources[i].name);
            s_sources[i].used = false;
            changed = true;
        }
    }
    if (changed) {
        merge_locked(now);
    }
    xSemaphoreGive(s_rx_lock);
}

static void handle_packet(const uint8_t *p, int len, uint32_t from_ip, uint16_t want_universe)
{
    /* Root layer */
    if (len < OFS_SLOTS || rd16(p) != ACN_PREAMBLE_SIZE || rd16(p + 2) != 0 ||
        memcmp(p + 4, ACN_PID, 12) != 0) {
        return;
    }
    if (rd32(p + OFS_ROOT_VECTOR) != VECTOR_ROOT_E131_DATA) {
        return; /* extended (sync / discovery) packets are not needed by this receiver */
    }
    if (rd32(p + OFS_FRAME_VECTOR) != VECTOR_E131_DATA_PACKET) {
        return;
    }
    uint16_t universe = rd16(p + OFS_UNIVERSE);
    if (universe != want_universe) {
        return;
    }
    const uint8_t *cid = p + OFS_CID;
    if (memcmp(cid, s_cid, 16) == 0) {
        return; /* our own output looped back */
    }
    /* DMP layer */
    if (p[OFS_DMP_VECTOR] != VECTOR_DMP_SET_PROPERTY || p[OFS_DMP_TYPE] != 0xA1 ||
        rd16(p + OFS_DMP_FIRST) != 0x0000 || rd16(p + OFS_DMP_INC) != 0x0001) {
        return;
    }
    uint16_t count = rd16(p + OFS_DMP_COUNT);
    if (count < 1 || count > 513 || len < OFS_START_CODE + count) {
        return;
    }
    /* PDU lengths must agree with the datagram */
    if ((rd16(p + OFS_ROOT_FLEN) & 0x0FFF) != OFS_START_CODE + count - OFS_ROOT_FLEN ||
        (rd16(p + OFS_FRAME_FLEN) & 0x0FFF) != OFS_START_CODE + count - OFS_FRAME_FLEN ||
        (rd16(p + OFS_DMP_FLEN) & 0x0FFF) != OFS_START_CODE + count - OFS_DMP_FLEN) {
        return;
    }
    uint8_t priority = p[OFS_PRIORITY];
    uint8_t options = p[OFS_OPTIONS];
    uint8_t seq = p[OFS_SEQ];
    if (priority > 200 || (options & OPT_PREVIEW)) {
        return;
    }

    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    int idx = -1, free_idx = -1;
    for (int i = 0; i < FSF_SACN_MAX_SOURCES; i++) {
        if (s_sources[i].used && memcmp(s_sources[i].cid, cid, 16) == 0) {
            idx = i;
            break;
        }
        if (!s_sources[i].used && free_idx < 0) {
            free_idx = i;
        }
    }
    if (idx >= 0) {
        /* E1.31 6.7.2 sequence check */
        int8_t diff = (int8_t)(seq - s_sources[idx].last_seq);
        if (diff <= 0 && diff > -20) {
            s_rx_status.seq_dropped++;
            xSemaphoreGive(s_rx_lock);
            return;
        }
    }
    if (options & OPT_TERMINATED) {
        if (idx >= 0) {
            ESP_LOGI(TAG, "source '%s' terminated stream", s_sources[idx].name);
            s_sources[idx].used = false;
            merge_locked(now);
        }
        xSemaphoreGive(s_rx_lock);
        return;
    }
    if (idx < 0) {
        if (free_idx < 0) {
            xSemaphoreGive(s_rx_lock);
            return; /* source limit exceeded */
        }
        idx = free_idx;
        source_t *s = &s_sources[idx];
        memset(s, 0, sizeof(*s));
        s->used = true;
        memcpy(s->cid, cid, 16);
        memcpy(s->name, p + OFS_SOURCE_NAME, FSF_SACN_SOURCE_NAME_LEN);
        s->name[FSF_SACN_SOURCE_NAME_LEN - 1] = '\0';
        ESP_LOGI(TAG, "new source '%s' on universe %u", s->name, universe);
    }
    source_t *s = &s_sources[idx];
    s->last_seq = seq;
    s->last_us = now;
    s->ip = from_ip;
    s->priority = priority;
    if (p[OFS_START_CODE] == 0x00) {
        s->slots = count - 1;
        memcpy(s->data, p + OFS_SLOTS, s->slots);
        memset(s->data + s->slots, 0, 512 - s->slots);
        s_rx_status.packets++;
        s_rx_status.last_rx_us = now;
        merge_locked(now);
    }
    /* Other start codes (e.g. 0xDD per-address priority) only refresh the source timer. */
    xSemaphoreGive(s_rx_lock);
}

static void rx_task(void *arg)
{
    static uint8_t buf[700];
    int sock = -1;
    uint16_t joined_universe = 0;
    uint32_t joined_ip = 0;
    uint32_t seen_generation = UINT32_MAX;

    for (;;) {
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                ESP_LOGE(TAG, "rx socket failed (%d)", errno);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            int one = 1;
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            int rcvbuf = 8 * 1024;
            setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
            struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_port = htons(FSF_SACN_PORT),
                .sin_addr.s_addr = htonl(INADDR_ANY),
            };
            if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                ESP_LOGE(TAG, "rx bind failed (%d)", errno);
                close(sock);
                sock = -1;
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            joined_universe = 0;
            seen_generation = UINT32_MAX;
        }

        fsf_config_t cfg;
        fsf_config_get(&cfg);
        fsf_net_status_t ns;
        fsf_net_get_status(&ns);

        /* Address/link changed: start over with a fresh socket. Closing it releases every
         * multicast membership, so nothing can leak however often the address changes. */
        if (seen_generation != UINT32_MAX && ns.generation != seen_generation) {
            close(sock);
            sock = -1;
            continue;
        }

        /* (Re-)join the source universe's multicast group. */
        if (ns.has_ip && cfg.src_universe != joined_universe) {
            struct ip_mreq m;
            if (joined_universe) {
                m.imr_multiaddr.s_addr = mcast_group(joined_universe);
                m.imr_interface.s_addr = joined_ip;
                setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &m, sizeof(m));
                joined_universe = 0;
            }
            m.imr_multiaddr.s_addr = mcast_group(cfg.src_universe);
            m.imr_interface.s_addr = ns.ip;
            if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m)) == 0) {
                joined_universe = cfg.src_universe;
                joined_ip = ns.ip;
                seen_generation = ns.generation;
                ESP_LOGI(TAG, "listening on universe %u", joined_universe);
            } else {
                ESP_LOGW(TAG, "IGMP join for universe %u failed (%d)", cfg.src_universe, errno);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }

        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n > 0) {
            handle_packet(buf, n, from.sin_addr.s_addr, cfg.src_universe);
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "rx error %d; reopening socket", errno);
            close(sock);
            sock = -1;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        expire_sources(esp_timer_get_time());
    }
}

static void on_config(const fsf_config_t *o, const fsf_config_t *n)
{
    if (o->src_universe != n->src_universe) {
        /* Different universe: forget the old universe's sources and data. */
        xSemaphoreTake(s_rx_lock, portMAX_DELAY);
        memset(s_sources, 0, sizeof(s_sources));
        memset(s_merged, 0, sizeof(s_merged));
        memset(&s_rx_status, 0, sizeof(s_rx_status));
        xSemaphoreGive(s_rx_lock);
    }
}

void fsf_sacn_rx_snapshot(uint8_t out[512], fsf_sacn_rx_status_t *status)
{
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    if (out) {
        memcpy(out, s_merged, 512);
    }
    if (status) {
        *status = s_rx_status;
    }
    xSemaphoreGive(s_rx_lock);
}

int fsf_sacn_rx_sources(fsf_sacn_source_info_t *out, int max)
{
    int n = 0;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_rx_lock, portMAX_DELAY);
    for (int i = 0; i < FSF_SACN_MAX_SOURCES && n < max; i++) {
        if (s_sources[i].used) {
            memcpy(out[n].name, s_sources[i].name, sizeof(out[n].name));
            memcpy(out[n].cid, s_sources[i].cid, 16);
            out[n].priority = s_sources[i].priority;
            out[n].ip = s_sources[i].ip;
            out[n].age_ms = (uint32_t)((now - s_sources[i].last_us) / 1000);
            n++;
        }
    }
    xSemaphoreGive(s_rx_lock);
    return n;
}

/* ------------------------------------------------------------------------------------------ */
/* Transmit                                                                                    */

static bool tx_ready(void)
{
    fsf_net_status_t ns;
    fsf_net_get_status(&ns);
    if (!ns.has_ip) {
        return false;
    }
    if (s_tx_sock < 0) {
        s_tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_tx_sock < 0) {
            return false;
        }
        uint8_t ttl = 20;
        setsockopt(s_tx_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        uint8_t loop = 0;
        setsockopt(s_tx_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        s_tx_if_generation = UINT32_MAX;
    }
    if (s_tx_if_generation != ns.generation) {
        struct in_addr ifa = {.s_addr = ns.ip};
        setsockopt(s_tx_sock, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof(ifa));
        s_tx_if_generation = ns.generation;
    }
    return true;
}

static uint8_t next_seq(uint16_t universe)
{
    for (int i = 0; i < 2; i++) {
        if (s_seq_universe[i] == universe) {
            return s_seq[i]++;
        }
    }
    /* Replace the older entry (only the current and the previous universe are ever in use). */
    s_seq_universe[1] = s_seq_universe[0];
    s_seq[1] = s_seq[0];
    s_seq_universe[0] = universe;
    s_seq[0] = 1;
    return 0;
}

static void build_data(uint16_t universe, uint8_t priority, uint8_t options, const uint8_t *data)
{
    uint8_t *p = s_tx_buf;
    memset(p, 0, OFS_SLOTS);
    wr16(p, ACN_PREAMBLE_SIZE);
    wr16(p + 2, 0);
    memcpy(p + 4, ACN_PID, 12);
    wr16(p + OFS_ROOT_FLEN, 0x7000 | (DATA_PACKET_LEN - OFS_ROOT_FLEN));
    wr32(p + OFS_ROOT_VECTOR, VECTOR_ROOT_E131_DATA);
    memcpy(p + OFS_CID, s_cid, 16);
    wr16(p + OFS_FRAME_FLEN, 0x7000 | (DATA_PACKET_LEN - OFS_FRAME_FLEN));
    wr32(p + OFS_FRAME_VECTOR, VECTOR_E131_DATA_PACKET);
    memcpy(p + OFS_SOURCE_NAME, s_source_name, strnlen(s_source_name, FSF_SACN_SOURCE_NAME_LEN - 1));
    p[OFS_PRIORITY] = priority;
    wr16(p + OFS_SYNC, 0);
    p[OFS_SEQ] = next_seq(universe);
    p[OFS_OPTIONS] = options;
    wr16(p + OFS_UNIVERSE, universe);
    wr16(p + OFS_DMP_FLEN, 0x7000 | (DATA_PACKET_LEN - OFS_DMP_FLEN));
    p[OFS_DMP_VECTOR] = VECTOR_DMP_SET_PROPERTY;
    p[OFS_DMP_TYPE] = 0xA1;
    wr16(p + OFS_DMP_FIRST, 0x0000);
    wr16(p + OFS_DMP_INC, 0x0001);
    wr16(p + OFS_DMP_COUNT, 513);
    p[OFS_START_CODE] = 0x00;
    if (data) {
        memcpy(p + OFS_SLOTS, data, 512);
    } else {
        memset(p + OFS_SLOTS, 0, 512);
    }
}

static void send_to_universe(uint16_t universe, const uint8_t *buf, size_t len)
{
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(FSF_SACN_PORT),
        .sin_addr.s_addr = mcast_group(universe),
    };
    if (sendto(s_tx_sock, buf, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        if (errno != ENOMEM && errno != EAGAIN) {
            ESP_LOGW(TAG, "tx error %d; reopening socket", errno);
            close(s_tx_sock);
            s_tx_sock = -1;
        }
        /* ENOMEM: transient pbuf exhaustion; the next frame retries. */
    }
}

void fsf_sacn_tx_send(uint16_t universe, uint8_t priority, const uint8_t data[512])
{
    if (!tx_ready()) {
        return;
    }
    build_data(universe, priority, 0, data);
    send_to_universe(universe, s_tx_buf, DATA_PACKET_LEN);
}

void fsf_sacn_tx_terminate(uint16_t universe, uint8_t priority)
{
    if (!tx_ready()) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        build_data(universe, priority, OPT_TERMINATED, NULL);
        send_to_universe(universe, s_tx_buf, DATA_PACKET_LEN);
    }
}

void fsf_sacn_tx_discovery(uint16_t universe)
{
    if (!tx_ready()) {
        return;
    }
    /* Root(38) + framing(74) + discovery layer(8) + 1 universe(2) = 122 bytes */
    uint8_t p[122];
    const int len = sizeof(p);
    memset(p, 0, sizeof(p));
    wr16(p, ACN_PREAMBLE_SIZE);
    memcpy(p + 4, ACN_PID, 12);
    wr16(p + 16, 0x7000 | (len - 16));
    wr32(p + 18, VECTOR_ROOT_E131_EXTENDED);
    memcpy(p + 22, s_cid, 16);
    /* Framing layer */
    wr16(p + 38, 0x7000 | (len - 38));
    wr32(p + 40, VECTOR_E131_EXTENDED_DISCOVERY);
    memcpy(p + 44, s_source_name, strnlen(s_source_name, FSF_SACN_SOURCE_NAME_LEN - 1));
    /* p[108..111] reserved = 0 */
    /* Universe discovery layer */
    wr16(p + 112, 0x7000 | (len - 112));
    wr32(p + 114, VECTOR_UNIVERSE_DISCOVERY_UNIVERSE_LIST);
    p[118] = 0; /* page */
    p[119] = 0; /* last page */
    wr16(p + 120, universe);
    send_to_universe(DISCOVERY_UNIVERSE, p, len);
}

esp_err_t fsf_sacn_init(const uint8_t cid[16], const char *source_name)
{
    memcpy(s_cid, cid, 16);
    strlcpy(s_source_name, source_name, sizeof(s_source_name));
    s_rx_lock = xSemaphoreCreateMutex();
    fsf_config_add_listener(on_config);
    if (xTaskCreate(rx_task, "sacn_rx", 4096, NULL, 10, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
