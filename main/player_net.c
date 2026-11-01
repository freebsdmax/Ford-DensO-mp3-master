#include "player_net.h"

#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "nvs.h"

static const char *TAG = "Net";

static player_net_cfg_t s_cfg = {
    .use_static = false,
    .ip = "",
    .gw = "",
    .mask = "255.255.255.0",
    .dns1 = "",
    .dns2 = "",
};

static void load_str(nvs_handle_t h, const char *key, char *dst, size_t cap, const char *def)
{
    size_t len = cap;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        strlcpy(dst, def, cap);
    }
}

void player_net_load(void)
{
    nvs_handle_t h;
    if (nvs_open("audio_player", NVS_READONLY, &h) == ESP_OK) {
        uint8_t st = 0;
        if (nvs_get_u8(h, "net_static", &st) == ESP_OK) {
            s_cfg.use_static = (st != 0);
        }
        load_str(h, "net_ip", s_cfg.ip, sizeof(s_cfg.ip), "");
        load_str(h, "net_gw", s_cfg.gw, sizeof(s_cfg.gw), "");
        load_str(h, "net_mask", s_cfg.mask, sizeof(s_cfg.mask), "255.255.255.0");
        load_str(h, "net_dns1", s_cfg.dns1, sizeof(s_cfg.dns1), "");
        load_str(h, "net_dns2", s_cfg.dns2, sizeof(s_cfg.dns2), "");
        nvs_close(h);
    }
    ESP_LOGI(TAG, "net: static=%d ip=%s", (int)s_cfg.use_static, s_cfg.ip);
}

void player_net_get(player_net_cfg_t *out)
{
    if (out) *out = s_cfg;
}

esp_err_t player_net_set(const player_net_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    s_cfg = *cfg;

    nvs_handle_t h;
    esp_err_t err = nvs_open("audio_player", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_u8(h, "net_static", s_cfg.use_static ? 1 : 0);
    nvs_set_str(h, "net_ip", s_cfg.ip);
    nvs_set_str(h, "net_gw", s_cfg.gw);
    nvs_set_str(h, "net_mask", s_cfg.mask);
    nvs_set_str(h, "net_dns1", s_cfg.dns1);
    nvs_set_str(h, "net_dns2", s_cfg.dns2);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool player_net_is_valid(const player_net_cfg_t *cfg)
{
    if (!cfg) return false;
    struct in_addr a;
    return inet_aton(cfg->ip, &a) &&
           inet_aton(cfg->mask, &a) &&
           (cfg->gw[0] == '\0' || inet_aton(cfg->gw, &a));
}

esp_err_t player_net_apply_static(esp_netif_t *netif)
{
    if (!netif) return ESP_ERR_INVALID_ARG;
    if (!s_cfg.use_static) return ESP_OK;
    if (!player_net_is_valid(&s_cfg)) {
        ESP_LOGW(TAG, "static config is invalid, keeping DHCP");
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip = {0};
    ip.ip.addr = inet_addr(s_cfg.ip);
    ip.netmask.addr = inet_addr(s_cfg.mask);
    ip.gw.addr = s_cfg.gw[0] ? inet_addr(s_cfg.gw) : 0;

    esp_netif_dhcpc_stop(netif);
    esp_err_t err = esp_netif_set_ip_info(netif, &ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_ip_info failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_netif_dns_info_t dns = {0};
    if (s_cfg.dns1[0]) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = inet_addr(s_cfg.dns1);
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    }
    if (s_cfg.dns2[0]) {
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = inet_addr(s_cfg.dns2);
        esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns);
    }

    ESP_LOGI(TAG, "static IP applied: %s/%s gw %s", s_cfg.ip, s_cfg.mask, s_cfg.gw);
    return ESP_OK;
}
