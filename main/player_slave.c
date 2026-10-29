#include "player_slave.h"
#include "player_pipeline.h"
#include "player_slave_ota.h"
#include "player_link.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "Player";

static char s_main_date[12];
static char s_main_time[9];
static char s_crc_date[12];
static char s_crc_time[9];
static char s_uart_date[12];
static char s_uart_time[9];
static char s_fw[24] = "unknown";
static SemaphoreHandle_t s_lock = NULL;
static volatile bool s_refresh = false;

static void copy_out(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

void slave_version_get(char *main_date, size_t main_date_len,
                       char *main_time, size_t main_time_len,
                       char *crc_date, size_t crc_date_len,
                       char *crc_time, size_t crc_time_len,
                       char *uart_date, size_t uart_date_len,
                       char *uart_time, size_t uart_time_len)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_out(main_date, main_date_len, s_main_date);
    copy_out(main_time, main_time_len, s_main_time);
    copy_out(crc_date, crc_date_len, s_crc_date);
    copy_out(crc_time, crc_time_len, s_crc_time);
    copy_out(uart_date, uart_date_len, s_uart_date);
    copy_out(uart_time, uart_time_len, s_uart_time);
    if (s_lock) xSemaphoreGive(s_lock);
}

void slave_fw_version_get(char *buf, size_t len)
{
    if (!buf || len == 0) return;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_out(buf, len, s_fw);
    if (s_lock) xSemaphoreGive(s_lock);
}

void slave_version_refresh_now(void)
{
    s_refresh = true;
}

static void slave_version_task(void *arg)
{
    (void)arg;
    uint32_t last_query = 0;
    char last_logged[24] = {0};
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (true) {
        if (slave_ota_busy() || uart_link_ota_active()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        bool due = (last_query == 0) || (now - last_query >= 30000);
        // On-demand refresh, but never more often than once per 10 seconds
        if (s_refresh && (last_query == 0 || now - last_query >= 10000)) {
            due = true;
            s_refresh = false;
        }

        if (due) {
            char md[12], mt[9], cd[12], ct[9], ud[12], ut[9];
            esp_err_t err = get_slave_version_extended(md, sizeof(md), mt, sizeof(mt),
                                                       cd, sizeof(cd), ct, sizeof(ct),
                                                       ud, sizeof(ud), ut, sizeof(ut));
            last_query = now;
            if (err == ESP_OK) {
                if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
                memcpy(s_main_date, md, sizeof(s_main_date));
                memcpy(s_main_time, mt, sizeof(s_main_time));
                memcpy(s_crc_date, cd, sizeof(s_crc_date));
                memcpy(s_crc_time, ct, sizeof(s_crc_time));
                memcpy(s_uart_date, ud, sizeof(s_uart_date));
                memcpy(s_uart_time, ut, sizeof(s_uart_time));
                if (s_lock) xSemaphoreGive(s_lock);

                char sig[24];
                snprintf(sig, sizeof(sig), "%s %s", md, mt);
                if (strcmp(sig, last_logged) != 0) {
                    strlcpy(last_logged, sig, sizeof(last_logged));
                    ESP_LOGI(TAG, "Slave version: %s", sig);
                }

                /* Firmware version (UART cmd 0x09) - slave >= v1.0.3 only */
                char fw[24] = {0};
                char fw_sig[26] = {0};
                if (get_slave_fw_version(fw, sizeof(fw)) == ESP_OK && fw[0] != '\0') {
                    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
                    strlcpy(s_fw, fw, sizeof(s_fw));
                    strlcpy(fw_sig, s_fw, sizeof(fw_sig));
                    if (s_lock) xSemaphoreGive(s_lock);
                    static char fw_logged[26] = {0};
                    if (strcmp(fw_sig, fw_logged) != 0) {
                        strlcpy(fw_logged, fw_sig, sizeof(fw_logged));
                        ESP_LOGI(TAG, "Slave firmware: %s", fw_sig);
                    }
                }
            } else {
                ESP_LOGW(TAG, "Slave version query failed");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void slave_version_start(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock && xTaskCreate(slave_version_task, "slave_ver", 3072, NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start slave version task");
    }
}
