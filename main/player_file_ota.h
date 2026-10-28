#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define FILE_OTA_IDLE      0
#define FILE_OTA_RECEIVING 1
#define FILE_OTA_APPLYING  2
#define FILE_OTA_DONE      3
#define FILE_OTA_ERROR     4

#define FILE_OTA_TARGET_NONE   0
#define FILE_OTA_TARGET_MASTER 1
#define FILE_OTA_TARGET_SLAVE  2

esp_err_t file_ota_begin(size_t total_size, int target);
esp_err_t file_ota_write_at(size_t offset, const uint8_t *data, size_t len);
esp_err_t file_ota_finish(void);
void file_ota_abort(void);
bool file_ota_busy(void);
void file_ota_get_error(char *buf, size_t len);
void file_ota_set_error_msg(const char *msg);
void file_ota_get_status(int *state, int *target, size_t *written, size_t *total, size_t *applied);
