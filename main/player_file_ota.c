#include "player_file_ota.h"
#include "player_storage.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>

extern esp_err_t uart_ota_send_firmware(const uint8_t *firmware_data, size_t firmware_size,
                                        void (*progress_callback)(size_t sent, size_t total));

static const char *TAG = "Player";

static uint8_t *s_buf = NULL;
static size_t s_total = 0;
static size_t s_written = 0;
static volatile size_t s_applied = 0;
static volatile int s_state = FILE_OTA_IDLE;
static volatile int s_target = FILE_OTA_TARGET_NONE;
static const esp_partition_t *s_part = NULL;
static char s_error[48] = "";
static char s_store_name[32] = "";
static bool s_delete_after = false;
static int s_reject_logs = 0;

static void set_error(const char *msg)
{
    strlcpy(s_error, msg ? msg : "", sizeof(s_error));
}

static void slave_progress(size_t sent, size_t total)
{
    (void)total;
    s_applied = sent;
}

bool file_ota_busy(void)
{
    return s_state == FILE_OTA_APPLYING;
}

/** Writes the received image into /storage/<name> without blocking the RPC. */
static void store_task(void *arg)
{
    (void)arg;
    s_state = FILE_OTA_APPLYING;
    s_applied = 0;

    esp_err_t err = player_storage_write_file(s_store_name, s_buf, s_total);
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }

    if (err != ESP_OK) {
        set_error("cannot write to storage");
        ESP_LOGE(TAG, "Storing /storage/%s failed: %s", s_store_name, esp_err_to_name(err));
        s_state = FILE_OTA_ERROR;
    } else {
        s_applied = s_total;
        s_state = FILE_OTA_DONE;
        ESP_LOGI(TAG, "Firmware stored as /storage/%s (%u bytes)", s_store_name, (unsigned)s_total);
    }
    vTaskDelete(NULL);
}

static void apply_task(void *arg)
{
    (void)arg;
    s_state = FILE_OTA_APPLYING;
    s_applied = 0;
    esp_err_t err = ESP_OK;

    // 1) Persist the received image into the staging partition
    if (s_part) {
        size_t aligned = (s_total + 4095) & ~((size_t)4095);
        if (aligned > s_part->size) aligned = s_total;
        err = esp_partition_erase_range(s_part, 0, aligned);
        if (err == ESP_OK) {
            uint8_t *block = heap_caps_malloc(4096, MALLOC_CAP_8BIT);
            if (!block) {
                err = ESP_ERR_NO_MEM;
            } else {
                size_t off = 0;
                while (off < s_total) {
                    size_t n = (s_total - off) < 4096 ? (s_total - off) : 4096;
                    memset(block, 0, 4096);
                    memcpy(block, s_buf + off, n);
                    err = esp_partition_write(s_part, off, block, 4096);
                    if (err != ESP_OK) break;
                    off += n;
                    s_applied = off;
                }
                heap_caps_free(block);
            }
        }
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Firmware saved to staging: %u bytes", (unsigned)s_total);
        }
    }

    // 2) Apply
    if (err == ESP_OK) {
        if (s_target == FILE_OTA_TARGET_MASTER) {
            const esp_partition_t *upd = esp_ota_get_next_update_partition(NULL);
            esp_ota_handle_t h = 0;
            err = upd ? esp_ota_begin(upd, s_total, &h) : ESP_ERR_NOT_FOUND;
            if (err == ESP_OK) {
                size_t off = 0;
                while (off < s_total) {
                    size_t n = (s_total - off) < 4096 ? (s_total - off) : 4096;
                    err = esp_ota_write(h, s_buf + off, n);
                    if (err != ESP_OK) break;
                    off += n;
                    s_applied = off;
                }
                if (err == ESP_OK) err = esp_ota_end(h);
                if (err == ESP_OK) err = esp_ota_set_boot_partition(upd);
            }
            if (err == ESP_OK) {
                if (s_delete_after && s_store_name[0]) {
                    player_storage_delete_file(s_store_name);
                }
                ESP_LOGI(TAG, "Master OTA applied, restarting");
                s_state = FILE_OTA_DONE;
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        } else if (s_target == FILE_OTA_TARGET_SLAVE) {
            err = uart_ota_send_firmware(s_buf, s_total, slave_progress);
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    }

    if (err != ESP_OK) {
        set_error(esp_err_to_name(err));
        ESP_LOGE(TAG, "File OTA apply failed: %s", esp_err_to_name(err));
        s_state = FILE_OTA_ERROR;
    } else {
        if (s_delete_after && s_store_name[0]) {
            player_storage_delete_file(s_store_name);
        }
        if (s_state != FILE_OTA_DONE) s_state = FILE_OTA_DONE;
        ESP_LOGI(TAG, "File OTA finished (target=%d)", s_target);
    }

    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    vTaskDelete(NULL);
}

esp_err_t file_ota_begin(size_t total_size, int target)
{
    if (s_state == FILE_OTA_APPLYING) {
        set_error("device is applying previous update");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    if (total_size < 1024 || total_size > (2 * 1024 * 1024)) {
        set_error("invalid firmware size");
        return ESP_ERR_INVALID_SIZE;
    }
    if (target != FILE_OTA_TARGET_MASTER && target != FILE_OTA_TARGET_SLAVE &&
        target != FILE_OTA_TARGET_STORAGE) {
        set_error("invalid target");
        return ESP_ERR_INVALID_ARG;
    }
    s_delete_after = false;

    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "staging");
    if (!s_part) {
        ESP_LOGE(TAG, "No 'staging' partition. Flash new partition table.");
    }

    s_buf = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) s_buf = heap_caps_malloc(total_size, MALLOC_CAP_8BIT);
    if (!s_buf) { set_error("out of memory"); return ESP_ERR_NO_MEM; }

    s_total = total_size;
    s_written = 0;
    s_applied = 0;
    s_target = target;
    s_error[0] = '\0';
    s_reject_logs = 0;
    s_state = FILE_OTA_RECEIVING;
    const char *tname = target == FILE_OTA_TARGET_MASTER ? "master" :
                        target == FILE_OTA_TARGET_SLAVE ? "slave" : "storage";
    ESP_LOGI(TAG, "File OTA begin: %u bytes, target=%s, name=%s, staging=%s",
             (unsigned)total_size, tname, s_store_name, s_part ? "yes" : "no");
    return ESP_OK;
}

esp_err_t file_ota_write_at(size_t offset, const uint8_t *data, size_t len)
{
    if (s_state != FILE_OTA_RECEIVING || !s_buf || !data || len == 0) {
        if (s_reject_logs++ < 10) {
            ESP_LOGW(TAG, "file_ota write reject: state=%d buf=%p len=%u",
                     s_state, s_buf, (unsigned)len);
        }
        set_error("receiver is not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if (offset > s_total || len > s_total - offset) {
        if (s_reject_logs++ < 10) {
            ESP_LOGW(TAG, "file_ota write reject: range off=%u len=%u total=%u",
                     (unsigned)offset, (unsigned)len, (unsigned)s_total);
        }
        set_error("chunk outside image size");
        return ESP_ERR_INVALID_SIZE;
    }
    /* The image is fully buffered in RAM, so a chunk may arrive in any order
       (duplicate, retry after a lost BLE response, or slightly ahead of the
       contiguous prefix). Just store it at its offset. */
    memcpy(s_buf + offset, data, len);
    if (offset + len > s_written) {
        s_written = offset + len;
    }
    return ESP_OK;
}

esp_err_t file_ota_finish(void)
{
    if (s_state != FILE_OTA_RECEIVING || !s_buf) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_written != s_total) {
        set_error("incomplete upload");
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_buf[0] != 0xE9) {
        set_error("not a firmware .bin (bad magic)");
        ESP_LOGE(TAG, "Not a firmware image (magic 0x%02X)", s_buf[0]);
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    if (s_target == FILE_OTA_TARGET_STORAGE) {
        /* Keep the image on the device; the app triggers the flash separately.
           The FAT write takes seconds, so it must not block the BLE RPC: the
           caller polls file.status until it reaches DONE (3) / ERROR (4). */
        s_state = FILE_OTA_APPLYING;
        if (xTaskCreate(store_task, "file_store", 4096, NULL, 4, NULL) != pdPASS) {
            set_error("out of memory");
            s_state = FILE_OTA_ERROR;
            return ESP_ERR_NO_MEM;
        }
        return ESP_OK;
    }

    s_state = FILE_OTA_APPLYING;
    if (xTaskCreate(apply_task, "file_ota", 8192, NULL, 4, NULL) != pdPASS) {
        s_state = FILE_OTA_ERROR;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void file_ota_abort(void)
{
    if (s_state == FILE_OTA_APPLYING) {
        return;
    }
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    s_total = 0;
    s_written = 0;
    s_applied = 0;
    s_state = FILE_OTA_IDLE;
    s_target = FILE_OTA_TARGET_NONE;
}

void file_ota_set_error_msg(const char *msg)
{
    set_error(msg);
}

void file_ota_get_error(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    strlcpy(buf, s_error, len);
}

void file_ota_get_status(int *state, int *target, size_t *written, size_t *total, size_t *applied)
{
    if (state) *state = s_state;
    if (target) *target = s_target;
    if (written) *written = s_written;
    if (total) *total = s_total;
    if (applied) *applied = s_applied;
}

esp_err_t file_ota_set_name(const char *name)
{
    char safe[32];
    if (!player_storage_sanitize_name(name, safe, sizeof(safe))) {
        set_error("invalid file name");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_store_name, safe, sizeof(s_store_name));
    return ESP_OK;
}

size_t file_ota_stored_size(const char *name)
{
    char safe[32];
    size_t size = 0;
    if (!player_storage_sanitize_name(name, safe, sizeof(safe))) return 0;
    player_storage_init();
    if (!player_storage_file_size(safe, &size)) return 0;
    return size;
}

esp_err_t file_ota_flash_stored(const char *name, int target)
{
    if (s_state == FILE_OTA_APPLYING) {
        set_error("device is applying previous update");
        return ESP_ERR_INVALID_STATE;
    }
    if (target != FILE_OTA_TARGET_MASTER && target != FILE_OTA_TARGET_SLAVE) {
        set_error("invalid target");
        return ESP_ERR_INVALID_ARG;
    }
    if (player_storage_init() != ESP_OK) {
        set_error("storage is not mounted");
        return ESP_FAIL;
    }

    char safe[32];
    if (!player_storage_sanitize_name(name, safe, sizeof(safe))) {
        set_error("invalid file name");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *buf = NULL;
    size_t len = 0;
    esp_err_t err = player_storage_read_file(safe, &buf, &len);
    if (err != ESP_OK || !buf || len < 1024) {
        set_error("stored image not found");
        return ESP_ERR_NOT_FOUND;
    }
    if (buf[0] != 0xE9) {
        heap_caps_free(buf);
        set_error("not a firmware .bin (bad magic)");
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }

    if (s_buf) heap_caps_free(s_buf);
    s_buf = buf;
    s_total = len;
    s_written = len;
    s_applied = 0;
    s_target = target;
    s_error[0] = '\0';
    strlcpy(s_store_name, safe, sizeof(s_store_name));
    s_delete_after = true;
    s_state = FILE_OTA_APPLYING;

    ESP_LOGI(TAG, "Flashing /storage/%s (%u bytes) -> %s",
             s_store_name, (unsigned)len,
             target == FILE_OTA_TARGET_MASTER ? "master" : "slave");

    if (xTaskCreate(apply_task, "file_ota", 8192, NULL, 4, NULL) != pdPASS) {
        s_state = FILE_OTA_ERROR;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
