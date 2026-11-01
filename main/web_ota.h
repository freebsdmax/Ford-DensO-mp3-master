#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

extern bool g_verbose_logs;
extern bool g_player_logs;
extern bool g_wifi_can_sniffer_logs;
extern bool g_web_ota_logs;
extern bool g_uart_ota_logs;
extern bool g_elm327_logs;

extern bool g_verbose_logs;
extern bool g_player_logs;
extern bool g_wifi_can_sniffer_logs;
extern bool g_web_ota_logs;
extern bool g_uart_ota_logs;
extern bool g_elm327_logs;

esp_err_t web_ota_start(void);
void web_ota_stop(void);
size_t web_ota_copy_logs(char *buffer, size_t buffer_size);
size_t web_ota_log_buffer_size(void);
esp_err_t web_ota_set_log_buffer_kb(uint16_t kb);
void web_ota_logs_init(void);
size_t web_ota_logs_since(uint32_t *since, char *dst, size_t cap);

// I2C slave OTA API
esp_err_t web_ota_i2c_flash(const uint8_t *data, size_t len);
