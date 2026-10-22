/*
 * FS*Fader - Ethernet bring-up (IP101 PHY on the internal EMAC), DHCP with link-local fallback,
 * static addressing, and hostname.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

typedef struct {
    bool link_up;
    bool has_ip;
    bool dhcp;       /* configured mode */
    bool link_local; /* current address is 169.254/16 (AutoIP fallback) */
    uint32_t ip, netmask, gateway; /* network byte order */
    uint32_t generation; /* increments whenever the address or link changes */
} fsf_net_status_t;

esp_err_t fsf_net_init(void);
void fsf_net_get_status(fsf_net_status_t *out);
esp_netif_t *fsf_net_netif(void);
const uint8_t *fsf_net_mac(void);   /* 6 bytes */
const char *fsf_net_hostname(void); /* "FS*Fader 1A2B" */
const char *fsf_net_dns_name(void); /* "fsfader-1a2b" (DNS-safe form) */

/* Re-apply the stored IP configuration (DHCP/static). */
void fsf_net_apply_config(void);
void fsf_net_renew_dhcp(void);
