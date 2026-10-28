#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

esp_err_t ble_ota_begin(size_t total_size);
esp_err_t ble_ota_write_at(size_t offset, const uint8_t *data, size_t len);
esp_err_t ble_ota_end(void);
void ble_ota_abort(void);
bool ble_ota_active(void);
size_t ble_ota_written(void);
size_t ble_ota_total(void);
