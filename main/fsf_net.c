#include "fsf_net.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "sdkconfig.h"
#include "fsf_config.h"

static const char *TAG = "net";

static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth;
static uint8_t s_mac[6];
static char s_hostname[24];
static char s_dns_name[24];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static fsf_net_status_t s_status;
static volatile bool s_link_up;

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        s_link_up = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "link down");
        s_link_up = false;
        break;
    default:
        break;
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR " mask " IPSTR " gw " IPSTR, IP2STR(&e->ip_info.ip),
                 IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw));
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        ESP_LOGW(TAG, "lost IP");
    }
}

/*
 * lwIP's AutoIP (link-local) fallback does not raise esp_netif events, so the address is
 * polled here. Every change bumps `generation`, which the sACN and RDMnet tasks watch to
 * re-join multicast groups and reconnect.
 */
static void monitor_task(void *arg)
{
    for (;;) {
        esp_netif_ip_info_t info = {0};
        esp_netif_get_ip_info(s_netif, &info);
        fsf_config_t cfg;
        fsf_config_get(&cfg);
        bool link = s_link_up;
        bool has_ip = link && info.ip.addr != 0;

        taskENTER_CRITICAL(&s_mux);
        bool changed = s_status.link_up != link || s_status.ip != info.ip.addr ||
                       s_status.netmask != info.netmask.addr || s_status.gateway != info.gw.addr ||
                       s_status.has_ip != has_ip;
        s_status.link_up = link;
        s_status.has_ip = has_ip;
        s_status.dhcp = cfg.net.dhcp;
        s_status.ip = info.ip.addr;
        s_status.netmask = info.netmask.addr;
        s_status.gateway = info.gw.addr;
        s_status.link_local = (ntohl(info.ip.addr) >> 16) == 0xA9FE;
        if (changed) {
            s_status.generation++;
        }
        taskEXIT_CRITICAL(&s_mux);

        if (changed && has_ip) {
            ESP_LOGI(TAG, "address now " IPSTR "%s", IP2STR(&info.ip),
                     s_status.link_local ? " (link-local fallback)" : "");
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void fsf_net_apply_config(void)
{
    fsf_config_t cfg;
    fsf_config_get(&cfg);
    if (cfg.net.dhcp) {
        esp_err_t err = esp_netif_dhcpc_start(s_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGE(TAG, "DHCP start failed: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "IP mode: DHCP (link-local fallback)");
    } else {
        esp_netif_dhcpc_stop(s_netif);
        esp_netif_ip_info_t info = {
            .ip.addr = cfg.net.ip,
            .netmask.addr = cfg.net.netmask,
            .gw.addr = cfg.net.gateway,
        };
        esp_err_t err = esp_netif_set_ip_info(s_netif, &info);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "static IP failed: %s", esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "IP mode: static " IPSTR "/" IPSTR " gw " IPSTR, IP2STR(&info.ip),
                 IP2STR(&info.netmask), IP2STR(&info.gw));
    }
}

void fsf_net_renew_dhcp(void)
{
    fsf_config_t cfg;
    fsf_config_get(&cfg);
    if (cfg.net.dhcp) {
        esp_netif_dhcpc_stop(s_netif);
        esp_netif_dhcpc_start(s_netif);
    }
}

static esp_timer_handle_t s_apply_timer;

static void apply_timer_cb(void *arg)
{
    fsf_net_apply_config();
}

static void on_config(const fsf_config_t *o, const fsf_config_t *n)
{
    if (o->net.dhcp != n->net.dhcp || o->net.ip != n->net.ip || o->net.netmask != n->net.netmask ||
        o->net.gateway != n->net.gateway) {
        /* Delay slightly so the web/RDM reply that caused the change still reaches the sender. */
        esp_timer_stop(s_apply_timer);
        esp_timer_start_once(s_apply_timer, 1500 * 1000);
    }
}

esp_err_t fsf_net_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    /* MAC / PHY: IP101GRI at address 1, 50 MHz RMII reference clock from the PHY on GPIO50. */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = 31;
    emac_config.smi_gpio.mdio_num = 52;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = 50;
    emac_config.emac_dataif_gpio.rmii.tx_en_num = 49;
    emac_config.emac_dataif_gpio.rmii.txd0_num = 34;
    emac_config.emac_dataif_gpio.rmii.txd1_num = 35;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = 28;
    emac_config.emac_dataif_gpio.rmii.rxd0_num = 29;
    emac_config.emac_dataif_gpio.rmii.rxd1_num = 30;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_FSF_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_FSF_ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    if (!mac || !phy) {
        ESP_LOGE(TAG, "failed to create Ethernet MAC/PHY");
        return ESP_FAIL;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, s_mac);

    snprintf(s_hostname, sizeof(s_hostname), "FS*Fader %02X%02X", s_mac[4], s_mac[5]);
    snprintf(s_dns_name, sizeof(s_dns_name), "fsfader-%02x%02x", s_mac[4], s_mac[5]);

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth));
    /* Sent to the DHCP server (option 12). */
    esp_netif_set_hostname(s_netif, s_hostname);

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event, NULL));

    fsf_net_apply_config();
    const esp_timer_create_args_t targs = {.callback = apply_timer_cb, .name = "net_apply"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_apply_timer));
    fsf_config_add_listener(on_config);

    err = esp_eth_start(s_eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "hostname '%s', MAC " MACSTR, s_hostname, MAC2STR(s_mac));
    xTaskCreate(monitor_task, "net_mon", 3072, NULL, 4, NULL);
    return ESP_OK;
}

void fsf_net_get_status(fsf_net_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_status;
    taskEXIT_CRITICAL(&s_mux);
}

esp_netif_t *fsf_net_netif(void)
{
    return s_netif;
}

const uint8_t *fsf_net_mac(void)
{
    return s_mac;
}

const char *fsf_net_hostname(void)
{
    return s_hostname;
}

const char *fsf_net_dns_name(void)
{
    return s_dns_name;
}
