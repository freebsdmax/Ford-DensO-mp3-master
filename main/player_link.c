#include "player_link.h"
#include "app_messages.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG_LINK = "Player";

static SemaphoreHandle_t s_link_lock = NULL;
static volatile bool s_ota_active = false;

bool uart_link_lock(uint32_t timeout_ms)
{
    if (!s_link_lock) {
        s_link_lock = xSemaphoreCreateMutex();
        if (!s_link_lock) return false;
    }
    return xSemaphoreTake(s_link_lock, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void uart_link_unlock(void)
{
    if (s_link_lock) xSemaphoreGive(s_link_lock);
}

bool uart_link_ota_active(void)
{
    return s_ota_active;
}

void uart_link_set_ota_active(bool active)
{
    s_ota_active = active;
}

void gpio_cb(void *arg)
{
    BaseType_t xTaskWoken = pdFALSE;
    app_message_t message = {
        .id = APP_NEXT_TRACK,
    };

    if (app_queue) {
        xQueueSendFromISR(app_queue, &message, &xTaskWoken);
    }

    if (xTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t send_i2c_command(uint8_t cmd)
{
    // Never inject a command while an OTA owns the link.
    if (s_ota_active) {
        ESP_LOGW(TAG_LINK, "UART cmd 0x%02X dropped: OTA owns the link", cmd);
        return ESP_ERR_INVALID_STATE;
    }
    if (!uart_link_lock(100)) {
        ESP_LOGW(TAG_LINK, "UART cmd 0x%02X dropped: link busy", cmd);
        return ESP_ERR_TIMEOUT;
    }
    // Відправляємо з префіксом UART_CMD_PREFIX_NORMAL
    uint8_t packet[2] = {UART_CMD_PREFIX_NORMAL, cmd};
    int w = uart_write_bytes(MASTER_UART_PORT, (const char *)packet, 2);
    uart_link_unlock();
    if (w != 2) {
        ESP_LOGW(TAG_LINK, "UART cmd 0x%02X not sent (%d/2 bytes)", cmd, w);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG_LINK, "UART cmd 0x%02X sent to slave", cmd);
    return ESP_OK;
}

esp_err_t init_i2c_master(void)
{
    if (!s_link_lock) {
        s_link_lock = xSemaphoreCreateMutex();
    }
    const uart_config_t cfg = {
        .baud_rate = MASTER_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,0,0)
        .source_clk = UART_SCLK_DEFAULT,
#endif
    };
    esp_err_t err = uart_param_config(MASTER_UART_PORT, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(MASTER_UART_PORT, MASTER_UART_TX_PIN, MASTER_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    err = uart_driver_install(MASTER_UART_PORT, MASTER_UART_RX_BUF, MASTER_UART_TX_BUF, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_LINK, "❌ Помилка встановлення UART драйвера: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG_LINK, "✅ UART master ініціалізовано (TX=%d RX=%d, %d bps)", MASTER_UART_TX_PIN, MASTER_UART_RX_PIN, MASTER_UART_BAUD);
    }
    // Clean any pending RX
    uart_flush(MASTER_UART_PORT);
    return err;
}

