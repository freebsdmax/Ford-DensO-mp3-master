#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"

/**
 * Мережеві налаштування: статична IP/DNS для STA та mDNS-ім'я.
 * Зберігаються в NVS (audio_player), застосовуються при підключенні.
 */

typedef struct {
    bool use_static;
    char ip[16];
    char gw[16];
    char mask[16];
    char dns1[16];
    char dns2[16];
} player_net_cfg_t;

void player_net_load(void);
void player_net_get(player_net_cfg_t *out);
esp_err_t player_net_set(const player_net_cfg_t *cfg);

/** true, якщо IP-адресу/маску/шлюз вдалося розібрати. */
bool player_net_is_valid(const player_net_cfg_t *cfg);

/** Зупиняє DHCP і застосовує статичну конфігурацію на STA-інтерфейсі. */
esp_err_t player_net_apply_static(esp_netif_t *netif);
