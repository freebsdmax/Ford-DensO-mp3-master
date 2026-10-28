#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define SLAVE_OTA_IDLE      0
#define SLAVE_OTA_RECEIVING 1
#define SLAVE_OTA_SENDING   2
#define SLAVE_OTA_DONE      3
#define SLAVE_OTA_ERROR     4

esp_err_t slave_ota_begin(size_t total_size);
esp_err_t slave_ota_write_at(size_t offset, const uint8_t *data, size_t len);
esp_err_t slave_ota_start(void);
void slave_ota_abort(void);
bool slave_ota_busy(void);
void slave_ota_get_status(int *state, size_t *written, size_t *total, size_t *sent);
