#include "player_console.h"
#include "app_messages.h"
#include "player_pipeline.h"
#include "player_sleep.h"
#include "player_usb.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include <stdio.h>

static const char *TAG_CON = "Player";

extern bool g_verbose_logs;

QueueHandle_t app_queue = NULL;

static void print_task_stats(void)
{
    UBaseType_t count = uxTaskGetNumberOfTasks();
    TaskStatus_t *arr = pvPortMalloc(count * sizeof(TaskStatus_t));
    if (!arr) {
        printf("Not enough memory for task stats\r\n");
        return;
    }
    count = uxTaskGetSystemState(arr, count, NULL);
    printf("\r\n%-16s %5s %5s %10s %5s\r\n", "Task", "State", "Prio", "StackFree", "Num");
    for (UBaseType_t i = 0; i < count; i++) {
        printf("%-16s %5d %5u %10u %5u\r\n",
               arr[i].pcTaskName,
               (int)arr[i].eCurrentState,
               (unsigned)arr[i].uxCurrentPriority,
               (unsigned)arr[i].usStackHighWaterMark,
               (unsigned)arr[i].xTaskNumber);
    }
    vPortFree(arr);
    printf("Free heap: %u, min free: %u\r\n\r\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size());
}

void terminal_commands_task(void *arg)
{
    if (g_verbose_logs) {
        ESP_LOGI(TAG_CON, "⌨️ Terminal commands task started");
        ESP_LOGI(TAG_CON, "   n - next track");
        ESP_LOGI(TAG_CON, "   p - previous track");
        ESP_LOGI(TAG_CON, "   m - mode switch");
        ESP_LOGI(TAG_CON, "   c - clear external I2S RX buffer");
        ESP_LOGI(TAG_CON, "   s - disable sleep");
        ESP_LOGI(TAG_CON, "   w - switch Wi-Fi to STA (DHCP)");
        ESP_LOGI(TAG_CON, "   t - tasks/stack stats");
        ESP_LOGI(TAG_CON, "   h - free heap");
    }
    
    char ch;
    app_message_t msg;
    
    while (1) {
        // Читаємо один символ з stdin (UART)
        ch = getchar();
        
        if (ch == 'n' || ch == 'N') {
            //ESP_LOGI(TAG_CON, "⏭️ Terminal: Next track");
            msg.id = APP_NEXT_TRACK;
            xQueueSend(app_queue, &msg, 0);
        }
        else if (ch == 'p' || ch == 'P') {
            //ESP_LOGI(TAG_CON, "⏮️ Terminal: Previous track");
            msg.id = APP_PREV_TRACK;
            xQueueSend(app_queue, &msg, 0);
        }
        else if (ch == 'm' || ch == 'M') {
            // Звичайне коротке натискання — перемикання режиму відтворення
            msg.id = APP_MODE_SWITCH;
            xQueueSend(app_queue, &msg, 0);
        }
        else if (ch == 's' || ch == 'S') {
            // Disable sleep (set sleep_mode = 0), keep current idle timeout
            int timeout = g_sleep_settings.idle_timeout_sec;
            if (timeout < 10) timeout = 30;
            esp_err_t err = set_sleep_settings(0, timeout);
            if (err == ESP_OK) {
                printf("Sleep disabled (mode=0)\r\n");
                if (g_verbose_logs) ESP_LOGI(TAG_CON, "Sleep disabled from console");
            } else {
                printf("Failed to disable sleep: %s\r\n", esp_err_to_name(err));
            }
        }
        else if (ch == 'w' || ch == 'W') {
            // Switch Wi-Fi to STA (DHCP)
            if (g_verbose_logs) ESP_LOGI(TAG_CON, "Wi-Fi switch to STA (DHCP) requested");
            msg.id = APP_WIFI_SWITCH_STA;
            xQueueSend(app_queue, &msg, 0);
        }
        else if (ch == 'u' || ch == 'U') {
            g_usb_reinit_request = true;
            printf("USB re-init requested\r\n");
        }
        else if (ch == 't' || ch == 'T') {
            print_task_stats();
        }
        else if (ch == 'h' || ch == 'H') {
            printf("Free heap: %u, min free: %u\r\n",
                   (unsigned)esp_get_free_heap_size(),
                   (unsigned)esp_get_minimum_free_heap_size());
        }
        else if (ch == 'c' || ch == 'C') {
            if (g_mode_external && g_i2s_passthrough_task_handle) {
                g_external_i2s_flush_requested = true;
                printf("I2S RX buffer flush requested\r\n");
            } else {
                printf("I2S RX buffer flush is available only in external mode\r\n");
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

