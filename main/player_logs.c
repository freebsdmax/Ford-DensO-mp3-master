#include "player_logs.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

extern bool g_player_logs;
extern bool g_wifi_can_sniffer_logs;
extern bool g_web_ota_logs;
extern bool g_uart_ota_logs;
extern bool g_elm327_logs;

bool g_log_pipeline = true;
bool g_log_can = true;
bool g_log_usb = true;
bool g_log_ble = true;
bool g_log_wifi = true;
bool g_log_sleep = true;
bool g_log_playlist = true;
bool g_log_console = true;
bool g_log_link = true;
bool g_log_nvs = true;

static const char *s_tags[LOG_MOD_COUNT] = {
    "Pipeline", "Can", "Usb", "Ble", "Wifi",
    "Sleep", "Playlist", "Console", "Link", "Nvs"
};

static const char *s_keys[LOG_MOD_COUNT] = {
    "log_pipeline", "log_can", "log_usb", "log_ble", "log_wifi",
    "log_sleep", "log_playlist", "log_console", "log_link", "log_nvs"
};

static const char *s_group_keys[LOG_GROUP_COUNT] = {
    "player", "connect", "ota", "car", "system"
};

const char *player_logs_group_key(int group)
{
    if (group < 0 || group >= LOG_GROUP_COUNT) return "";
    return s_group_keys[group];
}

/* Apply one switch to every component that belongs to the group. */
void player_logs_group_set(int group, bool on)
{
    switch (group) {
        case LOG_GROUP_PLAYER:
            g_player_logs = on;
            g_log_pipeline = on;
            g_log_playlist = on;
            break;
        case LOG_GROUP_CONNECT:
            g_log_ble = on;
            g_log_wifi = on;
            g_wifi_can_sniffer_logs = on;
            g_log_link = on;
            break;
        case LOG_GROUP_OTA:
            g_web_ota_logs = on;
            g_uart_ota_logs = on;
            break;
        case LOG_GROUP_CAR:
            g_log_can = on;
            g_elm327_logs = on;
            break;
        case LOG_GROUP_SYSTEM:
            g_log_usb = on;
            g_log_sleep = on;
            g_log_console = on;
            g_log_nvs = on;
            break;
        default:
            break;
    }
}

/* The group is "on" only when all of its components are on. */
bool player_logs_group_get(int group)
{
    switch (group) {
        case LOG_GROUP_PLAYER:
            return g_player_logs && g_log_pipeline && g_log_playlist;
        case LOG_GROUP_CONNECT:
            return g_log_ble && g_log_wifi && g_wifi_can_sniffer_logs && g_log_link;
        case LOG_GROUP_OTA:
            return g_web_ota_logs && g_uart_ota_logs;
        case LOG_GROUP_CAR:
            return g_log_can && g_elm327_logs;
        case LOG_GROUP_SYSTEM:
            return g_log_usb && g_log_sleep && g_log_console && g_log_nvs;
        default:
            return false;
    }
}

const char *player_logs_tag(int idx)
{
    if (idx < 0 || idx >= LOG_MOD_COUNT) return "";
    return s_tags[idx];
}

bool *player_logs_flag_ptr(int idx)
{
    static bool dummy = false;
    switch (idx) {
        case LOG_MOD_PIPELINE: return &g_log_pipeline;
        case LOG_MOD_CAN:      return &g_log_can;
        case LOG_MOD_USB:      return &g_log_usb;
        case LOG_MOD_BLE:      return &g_log_ble;
        case LOG_MOD_WIFI:     return &g_log_wifi;
        case LOG_MOD_SLEEP:    return &g_log_sleep;
        case LOG_MOD_PLAYLIST: return &g_log_playlist;
        case LOG_MOD_CONSOLE:  return &g_log_console;
        case LOG_MOD_LINK:     return &g_log_link;
        case LOG_MOD_NVS:      return &g_log_nvs;
        default: return &dummy;
    }
}

void player_logs_load(void)
{
    nvs_handle_t h;
    if (nvs_open("audio_player", NVS_READONLY, &h) != ESP_OK) return;

    uint8_t mask = 0;
    if (nvs_get_u8(h, "log_groups", &mask) == ESP_OK) {
        /* Groups are the source of truth: expand them to every component. */
        for (int g = 0; g < LOG_GROUP_COUNT; g++) {
            player_logs_group_set(g, ((mask >> g) & 1) != 0);
        }
    } else {
        /* First boot after the update: keep whatever was loaded before. */
        for (int i = 0; i < LOG_MOD_COUNT; i++) {
            uint8_t v = 0;
            if (nvs_get_u8(h, s_keys[i], &v) == ESP_OK) {
                *player_logs_flag_ptr(i) = (v != 0);
            }
        }
    }
    nvs_close(h);
}

void player_logs_save(void)
{
    nvs_handle_t h;
    if (nvs_open("audio_player", NVS_READWRITE, &h) != ESP_OK) return;
    for (int i = 0; i < LOG_MOD_COUNT; i++) {
        nvs_set_u8(h, s_keys[i], *player_logs_flag_ptr(i) ? 1 : 0);
    }
    uint8_t mask = 0;
    for (int g = 0; g < LOG_GROUP_COUNT; g++) {
        if (player_logs_group_get(g)) mask |= (uint8_t)(1u << g);
    }
    nvs_set_u8(h, "log_groups", mask);
    nvs_commit(h);
    nvs_close(h);
}

void player_logs_apply(void)
{
    bool master = g_player_logs;
    for (int i = 0; i < LOG_MOD_COUNT; i++) {
        bool on = master && *player_logs_flag_ptr(i);
        esp_log_level_set(s_tags[i], on ? ESP_LOG_INFO : ESP_LOG_NONE);
    }
    /* ELM327 lives in its own component with its own tag; the "Car" group
       (CAN + ELM327) must be able to silence it too. */
    esp_log_level_set("ELM327_EMU",
                      (master && g_elm327_logs) ? ESP_LOG_INFO : ESP_LOG_NONE);
}
