#pragma once
#include <stdint.h>
#define ESP_OK 0
struct esp_netif_ip_info_t { struct { uint32_t addr; } ip; };
void *esp_netif_get_handle_from_ifkey(const char *);
int esp_netif_get_ip_info(void *, esp_netif_ip_info_t *);
