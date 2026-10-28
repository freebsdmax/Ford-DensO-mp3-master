#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define NVS_NAMESPACE "audio_player"

// Minimal helpers only
esp_err_t nvs_write_i32(const char *ns, const char *key, int32_t value);
esp_err_t nvs_read_i32_def(const char *ns, const char *key, int32_t def_val, int32_t *out);
esp_err_t nvs_write_str(const char *ns, const char *key, const char *str);
esp_err_t nvs_read_str_def(const char *ns, const char *key, char *buf, size_t buf_len, const char *def_str);
esp_err_t save_playback_position(int32_t track_index, int32_t position_ms);
esp_err_t load_playback_position(int32_t *track_index, int32_t *position_ms);
esp_err_t clear_playback_position(void);
esp_err_t init_nvs(void);
extern int32_t g_saved_track_index;
extern int32_t g_saved_position_ms;

#define NVS_KEY_LAST_SOURCE "last_source"
#define NVS_KEY_WIFI_MODE  "wifi_mode"
#define NVS_KEY_STA_SSID   "sta_ssid"
#define NVS_KEY_STA_PASS   "sta_pass"
#define NVS_KEY_AP_SSID    "ap_ssid"
#define NVS_KEY_AP_PASS    "ap_pass"
#define NVS_KEY_SNIFFER_ENABLED "sniffer_enabled"
#define NVS_KEY_PLAYER_DATE "player_date"
#define NVS_KEY_PLAYER_TIME "player_time"
#define WIFI_AP_SSID     "ESP32-MusicPlayer"
#define WIFI_AP_PASSWORD "music1234"
#define WIFI_STA_SSID     "House"
#define WIFI_STA_PASSWORD "P!752Drx!r"

esp_err_t save_wifi_mode_pref(int mode);
esp_err_t load_wifi_mode_pref(int *mode);
esp_err_t save_sniffer_enabled(int enabled);
esp_err_t load_sniffer_enabled(int *enabled);
esp_err_t save_last_source_pref(int source);
esp_err_t load_last_source_pref(int *source);
esp_err_t save_sta_credentials(const char *ssid, const char *password);
esp_err_t load_sta_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len);
esp_err_t save_ap_credentials(const char *ssid, const char *password);
esp_err_t load_ap_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len);
