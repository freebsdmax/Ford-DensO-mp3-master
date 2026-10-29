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
/** Store the received image in /storage/<name> without applying it. */
#define FILE_OTA_TARGET_STORAGE 3

esp_err_t file_ota_begin(size_t total_size, int target);
esp_err_t file_ota_write_at(size_t offset, const uint8_t *data, size_t len);
esp_err_t file_ota_finish(void);

/** File name used when target == FILE_OTA_TARGET_STORAGE. */
esp_err_t file_ota_set_name(const char *name);

/**
 * Flashes a .bin kept in /storage/<name> and deletes it after success.
 * target: FILE_OTA_TARGET_MASTER or FILE_OTA_TARGET_SLAVE
 */
esp_err_t file_ota_flash_stored(const char *name, int target);

/** Size of a stored image, 0 when it does not exist. */
size_t file_ota_stored_size(const char *name);
void file_ota_abort(void);
bool file_ota_busy(void);
void file_ota_get_error(char *buf, size_t len);
void file_ota_set_error_msg(const char *msg);
void file_ota_get_status(int *state, int *target, size_t *written, size_t *total, size_t *applied);
