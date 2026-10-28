#pragma once
#include <stdbool.h>

typedef enum {
    LOG_MOD_PIPELINE = 0,
    LOG_MOD_CAN,
    LOG_MOD_USB,
    LOG_MOD_BLE,
    LOG_MOD_WIFI,
    LOG_MOD_SLEEP,
    LOG_MOD_PLAYLIST,
    LOG_MOD_CONSOLE,
    LOG_MOD_LINK,
    LOG_MOD_NVS,
    LOG_MOD_COUNT
} log_module_t;

extern bool g_log_pipeline;
extern bool g_log_can;
extern bool g_log_usb;
extern bool g_log_ble;
extern bool g_log_wifi;
extern bool g_log_sleep;
extern bool g_log_playlist;
extern bool g_log_console;
extern bool g_log_link;
extern bool g_log_nvs;

/* Logical groups shown in the UI instead of 15 individual components. */
typedef enum {
    LOG_GROUP_PLAYER = 0,   /* Player, Pipeline, Playlist            */
    LOG_GROUP_CONNECT,      /* BLE, WiFi, WiFi CAN Sniffer, Link     */
    LOG_GROUP_OTA,          /* Web OTA, UART OTA                     */
    LOG_GROUP_CAR,          /* CAN, ELM327                           */
    LOG_GROUP_SYSTEM,       /* USB, Sleep, Console, NVS              */
    LOG_GROUP_COUNT
} log_group_t;

const char *player_logs_group_key(int group);
bool player_logs_group_get(int group);
void player_logs_group_set(int group, bool on);

const char *player_logs_tag(int idx);
bool *player_logs_flag_ptr(int idx);
void player_logs_load(void);
void player_logs_save(void);
void player_logs_apply(void);
