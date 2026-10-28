#include "player_sleep.h"
#include "player_pipeline.h"
#include "player_can.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG_SLEEP = "Player";
#ifndef SLEEP_IND_PIN
#define SLEEP_IND_PIN 21
#endif
extern uint32_t g_sleep_start_time;

sleep_settings_t g_sleep_settings = {
    .sleep_mode = 0,
    .idle_timeout_sec = 30
};
uint32_t g_last_system_activity_time = 0;

void enter_deep_sleep(void);

esp_err_t get_sleep_settings(int *sleep_mode, int *idle_timeout_sec)
{
    if (!sleep_mode || !idle_timeout_sec) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *sleep_mode = g_sleep_settings.sleep_mode;
    *idle_timeout_sec = g_sleep_settings.idle_timeout_sec;
    return ESP_OK;
}

esp_err_t set_sleep_settings(int sleep_mode, int idle_timeout_sec)
{
    if (sleep_mode < 0 || sleep_mode > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    if (idle_timeout_sec < 10 || idle_timeout_sec > 28800) { // 10 сек - 8 годин
        return ESP_ERR_INVALID_ARG;
    }
    
    sleep_settings_t new_settings = {
        .sleep_mode = sleep_mode,
        .idle_timeout_sec = idle_timeout_sec
    };
    
    esp_err_t err = save_sleep_settings(&new_settings);
    if (err != ESP_OK) {
        return err;
    }
    
    // Оновити глобальні налаштування
    g_sleep_settings = new_settings;
    
    ESP_LOGI(TAG_SLEEP, "Sleep settings updated: mode=%d, timeout=%d sec", 
             sleep_mode, idle_timeout_sec);
    
    return ESP_OK;
}

esp_err_t load_sleep_settings(sleep_settings_t *settings)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("sleep", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_SLEEP, "NVS sleep settings not found, using defaults");
        return ESP_ERR_NVS_NOT_FOUND;
    }
    
    // Load sleep mode
    int32_t sleep_mode = 0;
    err = nvs_get_i32(nvs_handle, "sleep_mode", &sleep_mode);
    if (err == ESP_OK) {
        settings->sleep_mode = sleep_mode;
    }
    
    // Load idle timeout (try new key first, then old key for migration)
    int32_t idle_timeout = 30;
    err = nvs_get_i32(nvs_handle, "idle_timeout_s", &idle_timeout);
    if (err != ESP_OK) {
        // Try old key for migration
        err = nvs_get_i32(nvs_handle, "idle_timeout", &idle_timeout);
        if (err == ESP_OK) {
            // Convert from minutes to seconds (minimum 10 seconds)
            idle_timeout = idle_timeout * 60;
            if (idle_timeout < 10) idle_timeout = 10;
            if (idle_timeout > 28800) idle_timeout = 28800;
            ESP_LOGI(TAG_SLEEP, "Migrated idle_timeout from %d min to %d sec", idle_timeout / 60, idle_timeout);
            // Save with new key
            nvs_set_i32(nvs_handle, "idle_timeout_s", idle_timeout);
            nvs_erase_key(nvs_handle, "idle_timeout");
            nvs_commit(nvs_handle);
        }
    }
    settings->idle_timeout_sec = idle_timeout;
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG_SLEEP, "Loaded sleep settings: mode=%d, timeout=%d sec", 
             settings->sleep_mode, settings->idle_timeout_sec);
    return ESP_OK;
}

esp_err_t save_sleep_settings(const sleep_settings_t *settings)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("sleep", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SLEEP, "Failed to open NVS for sleep settings: %s (%d)", esp_err_to_name(err), err);
        return err;
    }
    
    // Erase old key if it exists (migration from minutes to seconds)
    nvs_erase_key(nvs_handle, "idle_timeout");
    
    // Save sleep mode
    err = nvs_set_i32(nvs_handle, "sleep_mode", settings->sleep_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SLEEP, "Failed to save sleep_mode: %s (%d)", esp_err_to_name(err), err);
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save idle timeout
    err = nvs_set_i32(nvs_handle, "idle_timeout_s", settings->idle_timeout_sec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SLEEP, "Failed to save idle_timeout_s: %s (%d)", esp_err_to_name(err), err);
        // Try to erase all keys and retry
        nvs_erase_all(nvs_handle);
        // Re-save sleep mode after erase
        err = nvs_set_i32(nvs_handle, "sleep_mode", settings->sleep_mode);
        if (err == ESP_OK) {
            err = nvs_set_i32(nvs_handle, "idle_timeout_s", settings->idle_timeout_sec);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG_SLEEP, "Still failed to save idle_timeout_s after erase: %s (%d)", esp_err_to_name(err), err);
            nvs_close(nvs_handle);
            return err;
        }
        ESP_LOGW(TAG_SLEEP, "NVS namespace was full, erased and retried successfully");
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_SLEEP, "Failed to commit NVS changes: %s (%d)", esp_err_to_name(err), err);
        nvs_close(nvs_handle);
        return err;
    }
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG_SLEEP, "Sleep settings saved: mode=%d, timeout=%d sec", 
                 settings->sleep_mode, settings->idle_timeout_sec);
    }
    return err;
}

bool should_enter_sleep(void)
{
    if (g_sleep_settings.sleep_mode == 0 || g_sleep_settings.idle_timeout_sec == 0) {
        return false; // sleep disabled
    }
    
    uint32_t idle_time_ms = pdTICKS_TO_MS(xTaskGetTickCount() - g_last_can_activity_time);
    uint32_t idle_timeout_ms = g_sleep_settings.idle_timeout_sec * 1000;
    
    ESP_LOGD(TAG_SLEEP, "Sleep check: mode=%d, timeout=%d, idle_time=%d ms, timeout_ms=%d", 
             g_sleep_settings.sleep_mode, g_sleep_settings.idle_timeout_sec, idle_time_ms, idle_timeout_ms);
    
    return idle_time_ms >= idle_timeout_ms;
}

void enter_configured_sleep(void)
{
    ESP_LOGI(TAG_SLEEP, "Entering sleep mode: %d", g_sleep_settings.sleep_mode);
    
    // 🎵 Стан плеєра вже збережено в can_sleep_monitor_task
    
    switch (g_sleep_settings.sleep_mode) {
        case 1: // Light sleep - ЗАМІНИЛИ НА DEEP SLEEP
        case 2: // Modem sleep - ЗАМІНИЛИ НА DEEP SLEEP  
        case 3: // Deep sleep
        default:
            ESP_LOGI(TAG_SLEEP, "Deep sleep - full power off");
            enter_deep_sleep();
            break;
    }
}

void update_system_activity(void)
{
    g_last_system_activity_time = xTaskGetTickCount();
}


void enter_deep_sleep(void)
{
    ESP_LOGI(TAG_SLEEP, "😴 Перехід в глибокий сон...");

    // 🏷️ Встановлюємо прапорець сну
    g_can_sleep_mode = true;
    g_sleep_start_time = xTaskGetTickCount();

    ESP_LOGI(TAG_SLEEP, "⏰ Встановлення будильника на GPIO%d (рівень LOW)", CAN_RX_GPIO);

    // ⏰ Налаштовуємо пробудження по CAN RX
    // Зупиняємо CAN контролер
    if (twai_stop() != ESP_OK) {
        ESP_LOGE(TAG_SLEEP, "Помилка зупинки TWAI драйвера перед сном.");
        // Можливо, варто не переходити в сон, якщо не можемо зупинити драйвер
        return;
    }
    ESP_LOGI(TAG_SLEEP, "Драйвер TWAI зупинено.");

    // Налаштовуємо пін CAN RX як вхід з підтяжкою вгору
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(CAN_RX_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG_SLEEP, "GPIO%d налаштовано як вхід з pull-up.", CAN_RX_GPIO);

    // Вмикаємо пробудження від зовнішнього джерела 0 (ext0)
    // Пробудження відбудеться, коли на піні CAN_RX_GPIO буде низький рівень (0)
    if (esp_sleep_enable_ext0_wakeup(CAN_RX_GPIO, 0) != ESP_OK) {
        ESP_LOGE(TAG_SLEEP, "Помилка налаштування пробудження ext0.");
        return;
    }
    ESP_LOGI(TAG_SLEEP, "Пробудження ext0 налаштовано для GPIO%d.", CAN_RX_GPIO);

    ESP_LOGI(TAG_SLEEP, "💤 Перехід в глибокий сон через 2 секунди...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    gpio_set_level((gpio_num_t)SLEEP_IND_PIN, 0);

    // 😴 Переходимо в сон
    esp_deep_sleep_start();
}
