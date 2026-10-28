#include "player_slave_ota.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

extern esp_err_t uart_ota_send_firmware(const uint8_t *firmware_data, size_t firmware_size,
                                        void (*progress_callback)(size_t sent, size_t total));

static const char *TAG = "Player";

static uint8_t *s_buf = NULL;
static size_t s_total = 0;
static size_t s_written = 0;
static volatile size_t s_sent = 0;
static volatile int s_state = SLAVE_OTA_IDLE;

static void progress_cb(size_t sent, size_t total)
{
    s_sent = sent;
    (void)total;
}

static void slave_ota_task(void *arg)
{
    (void)arg;
    s_state = SLAVE_OTA_SENDING;
    s_sent = 0;
    esp_err_t err = uart_ota_send_firmware(s_buf, s_total, progress_cb);
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    s_state = (err == ESP_OK) ? SLAVE_OTA_DONE : SLAVE_OTA_ERROR;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Slave OTA finished");
    } else {
        ESP_LOGE(TAG, "Slave OTA failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

esp_err_t slave_ota_begin(size_t total_size)
{
    if (s_state == SLAVE_OTA_SENDING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    if (total_size < 1024 || total_size > (2 * 1024 * 1024)) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_buf = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        s_buf = heap_caps_malloc(total_size, MALLOC_CAP_8BIT);
    }
    if (!s_buf) {
        return ESP_ERR_NO_MEM;
    }
    s_total = total_size;
    s_written = 0;
    s_sent = 0;
    s_state = SLAVE_OTA_RECEIVING;
    ESP_LOGI(TAG, "Slave OTA begin: %u bytes", (unsigned)total_size);
    return ESP_OK;
}

esp_err_t slave_ota_write_at(size_t offset, const uint8_t *data, size_t len)
{
    if (s_state != SLAVE_OTA_RECEIVING || !s_buf || !data || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (offset > s_total || len > s_total - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (offset > s_written) {
        static int rejects = 0;
        if (rejects++ < 5) {
            ESP_LOGW(TAG, "slave_ota gap: off=%u written=%u len=%u state=%d",
                     (unsigned)offset, (unsigned)s_written, (unsigned)len, s_state);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (offset + len <= s_written) {
        return ESP_OK;
    }
    size_t start = s_written;
    size_t skip = start - offset;
    size_t copy_len = offset + len - start;
    memcpy(s_buf + start, data + skip, copy_len);
    s_written += copy_len;
    return ESP_OK;
}

esp_err_t slave_ota_start(void)
{
    if (s_state != SLAVE_OTA_RECEIVING || !s_buf) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_written != s_total) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_buf[0] != 0xE9) {
        ESP_LOGE(TAG, "Slave image bad magic 0x%02X", s_buf[0]);
        return ESP_ERR_OTA_VALIDATE_FAILED;
    }
    ESP_LOGI(TAG, "Slave OTA start: %u bytes", (unsigned)s_total);
    s_state = SLAVE_OTA_SENDING;
    if (xTaskCreate(slave_ota_task, "slave_ota", 6144, NULL, 4, NULL) != pdPASS) {
        s_state = SLAVE_OTA_ERROR;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void slave_ota_abort(void)
{
    if (s_state == SLAVE_OTA_SENDING) {
        return; // let it finish
    }
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    s_total = 0;
    s_written = 0;
    s_sent = 0;
    s_state = SLAVE_OTA_IDLE;
}

bool slave_ota_busy(void)
{
    return s_state == SLAVE_OTA_SENDING;
}

void slave_ota_get_status(int *state, size_t *written, size_t *total, size_t *sent)
{
    if (state) *state = s_state;
    if (written) *written = s_written;
    if (total) *total = s_total;
    if (sent) *sent = s_sent;
}
