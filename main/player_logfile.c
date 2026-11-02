#include "player_logfile.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "player_storage.h"

static const char *TAG = "LogFile";
static const char *PATH = "/storage/log.txt";
static const char *PATH_OLD = "/storage/log.old";
static const size_t MAX_FILE = 256 * 1024;
static const uint32_t FLUSH_PERIOD_MS = 300000;

extern size_t web_ota_logs_since(uint32_t *since, char *dst, size_t cap);

static volatile bool s_enabled = true;
static uint32_t s_since = 0;

bool player_logfile_enabled(void)
{
    return s_enabled;
}

void player_logfile_set_enabled(bool enabled)
{
    s_enabled = enabled;
    nvs_handle_t h;
    if (nvs_open("audio_player", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "log_persist", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "persist %s", enabled ? "enabled" : "disabled");
}

static void logfile_load(void)
{
    nvs_handle_t h;
    if (nvs_open("audio_player", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 1;
        if (nvs_get_u8(h, "log_persist", &v) == ESP_OK) {
            s_enabled = (v != 0);
        }
        nvs_close(h);
    }
}

static void rotate_if_needed(void)
{
    struct stat st;
    if (stat(PATH, &st) == 0 && (size_t)st.st_size < MAX_FILE) return;
    if (stat(PATH, &st) != 0) return;

    remove(PATH_OLD);
    if (rename(PATH, PATH_OLD) != 0) {
        remove(PATH);       /* не вдалося перейменувати - просто почати заново */
    }
    ESP_LOGI(TAG, "log rotated");
}

static void logfile_task(void *arg)
{
    (void)arg;
    char *buf = malloc(4096);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for log file task");
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(10000));
    logfile_load();
    ESP_LOGI(TAG, "log persistence %s", s_enabled ? "enabled" : "disabled");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(FLUSH_PERIOD_MS));
        if (!s_enabled) continue;
        if (player_storage_init() != ESP_OK) continue;

        size_t n = web_ota_logs_since(&s_since, buf, 4096);
        if (n == 0) continue;

        rotate_if_needed();

        FILE *f = fopen(PATH, "ab");
        if (!f) {
            ESP_LOGW(TAG, "cannot open %s", PATH);
            continue;
        }
        size_t written = fwrite(buf, 1, n, f);
        fclose(f);
        if (written != n) {
            ESP_LOGW(TAG, "short write %u/%u", (unsigned)written, (unsigned)n);
        }
    }
}

void player_logfile_start(void)
{
    xTaskCreate(logfile_task, "logfile", 4096, NULL, 2, NULL);
}
