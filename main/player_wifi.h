#pragma once
#include "esp_err.h"
#include <stddef.h>

esp_err_t set_wifi_settings(const char *sta_ssid, const char *sta_password, const char *ap_ssid, const char *ap_password, int mode);
esp_err_t get_wifi_settings(char *sta_ssid, size_t sta_ssid_len, char *sta_password, size_t sta_pass_len, char *ap_ssid, size_t ap_ssid_len, char *ap_password, size_t ap_pass_len, int *mode);
esp_err_t try_wifi_connection_with_fallback(void);
void sta_connection_task(void *arg);
