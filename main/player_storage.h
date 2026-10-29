#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * Internal FAT partition ("storage", ~4.9 MB) used to keep a downloaded
 * firmware image on the device until it is applied.
 *
 * The app streams the .bin into /storage/<name>, and only then triggers the
 * flash - so a dropped BLE link does not force a full re-upload, and the file
 * stays on the device until the update actually succeeds.
 */
esp_err_t player_storage_init(void);
bool player_storage_ready(void);

esp_err_t player_storage_write_file(const char *name, const uint8_t *data, size_t len);
esp_err_t player_storage_read_file(const char *name, uint8_t **out, size_t *out_len);
esp_err_t player_storage_delete_file(const char *name);
bool player_storage_file_size(const char *name, size_t *size);

/** Copies [name] into [dst]; returns false when it is not a safe file name. */
bool player_storage_sanitize_name(const char *name, char *dst, size_t cap);
