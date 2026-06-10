#ifndef DNS_SERVER_H
#define DNS_SERVER_H

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

esp_err_t dns_server_start(esp_ip4_addr_t ip);
esp_err_t dns_server_stop(void);

#endif
