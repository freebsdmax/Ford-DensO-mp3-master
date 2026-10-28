#include "player_ble_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <string.h>

static const char *TAG_OTA = "ble_ota";

static const esp_partition_t *s_partition = NULL;
static uint8_t *s_buf = NULL;
static bool s_active = false;
static size_t s_total = 0;
static size_t s_written = 0;

bool ble_ota_active(void) { return s_active; }
size_t ble_ota_written(void) { return s_written; }
size_t ble_ota_total(void) { return s_total; }

esp_err_t ble_ota_begin(size_t total_size)
{
    if (s_active) {
        ble_ota_abort();
    }
    if (total_size < 1024) {
        return ESP_ERR_INVALID_ARG;
    }
    s_partition = esp_ota_get_next_update_partition(NULL);
    if (!s_partition) {
        ESP_LOGE(TAG_OTA, "No OTA partition");
        return ESP_ERR_NOT_FOUND;
    }
    if (total_size > s_partition->size) {
        ESP_LOGE(TAG_OTA, "Image too large: %u > %u", (unsigned)total_size, (unsigned)s_partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    // Buffer the whole image in PSRAM first (no flash writes during BLE transfer)
    s_buf = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        ESP_LOGW(TAG_OTA, "PSRAM alloc failed, trying internal RAM");
        s_buf = heap_caps_malloc(total_size, MALLOC_CAP_8BIT);
    }
    if (!s_buf) {
        ESP_LOGE(TAG_OTA, "Cannot allocate %u bytes for OTA buffer", (unsigned)total_size);
        return ESP_ERR_NO_MEM;
    }

    s_active = true;
    s_total = total_size;
    s_written = 0;
    ESP_LOGI(TAG_OTA, "OTA begin (buffered): %u bytes, free PSRAM=%u", (unsigned)total_size, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

esp_err_t ble_ota_write_at(size_t offset, const uint8_t *data, size_t len)
{
    if (!s_active || !s_buf || !data || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (offset > s_total || len > s_total - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (offset > s_written) {
        // Gap: wait for client to resend from s_written
        return ESP_ERR_INVALID_STATE;
    }
    if (offset + len <= s_written) {
        // Duplicate chunk, already have it
        return ESP_OK;
    }
    size_t start = s_written;                 // offset <= s_written
    size_t skip = start - offset;
    size_t copy_len = offset + len - start;
    memcpy(s_buf + start, data + skip, copy_len);
    s_written += copy_len;
    return ESP_OK;
}

esp_err_t ble_ota_end(void)
{
    if (!s_active || !s_buf) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_written != s_total) {
        ESP_LOGE(TAG_OTA, "Incomplete image: %u/%u", (unsigned)s_written, (unsigned)s_total);
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_buf[0] != 0xE9) {
        ESP_LOGE(TAG_OTA, "Bad magic 0x%02X", s_buf[0]);
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(s_partition, s_total, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_OTA, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t off = 0;
    const size_t block = 4096;
    while (off < s_total) {
        size_t n = (s_total - off) < block ? (s_total - off) : block;
        err = esp_ota_write(handle, s_buf + off, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_OTA, "esp_ota_write failed at %u: %s", (unsigned)off, esp_err_to_name(err));
            esp_ota_abort(handle);
            return err;
        }
        off += n;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_OTA, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ota_set_boot_partition(s_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_OTA, "set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    heap_caps_free(s_buf);
    s_buf = NULL;
    s_active = false;
    ESP_LOGI(TAG_OTA, "OTA complete, restarting...");
    return ESP_OK;
}

void ble_ota_abort(void)
{
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    s_active = false;
    s_total = 0;
    s_written = 0;
    ESP_LOGW(TAG_OTA, "OTA aborted");
}
