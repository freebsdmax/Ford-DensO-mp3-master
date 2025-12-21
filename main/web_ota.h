#pragma once
#include "esp_err.h"

extern bool g_verbose_logs;
extern bool g_player_logs;
extern bool g_wifi_can_sniffer_logs;
extern bool g_web_ota_logs;
extern bool g_uart_ota_logs;
extern bool g_elm327_logs;

esp_err_t web_ota_start(void);
void web_ota_stop(void);

// I2C slave OTA API
esp_err_t web_ota_i2c_flash(const uint8_t *data, size_t len);
