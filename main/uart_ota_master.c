#include "uart_ota_protocol.h"
#include "player_link.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "UART_OTA_MASTER";

// UART порт (визначений в play_sdcard_music_example.c)
#define MASTER_UART_PORT UART_NUM_1

/**
 * Зберегти версію UART OTA в NVS
 */
void uart_ota_save_version(void)
{
    //static bool version_saved = false;
    //if (version_saved) return;
    
    // Ініціалізувати NVS якщо потрібно
    static bool nvs_initialized = false;
    if (!nvs_initialized) {
        ESP_LOGI(TAG, "UART: Initializing NVS");
        esp_err_t err_init = nvs_flash_init();
        if (err_init == ESP_ERR_NVS_NO_FREE_PAGES || err_init == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGI(TAG, "UART: Erasing NVS");
            err_init = nvs_flash_erase();
            if (err_init != ESP_OK) {
                ESP_LOGW(TAG, "UART: NVS erase failed: %s", esp_err_to_name(err_init));
            }
            err_init = nvs_flash_init();
        }
        if (err_init != ESP_OK) {
            ESP_LOGE(TAG, "UART: Failed to init NVS: %s", esp_err_to_name(err_init));
            return;
        }
        nvs_initialized = true;
        ESP_LOGI(TAG, "UART: NVS initialized");
        }
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("uart_ota", NVS_READWRITE, &nvs_handle);
    ESP_LOGI(TAG, "UART: Opening uart_ota NVS");
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "UART: Setting date %s", __DATE__);
        nvs_set_str(nvs_handle, "uart_date", __DATE__);
        ESP_LOGI(TAG, "UART: Setting time %s", __TIME__);
        nvs_set_str(nvs_handle, "uart_time", __TIME__);
        ESP_LOGI(TAG, "UART: Committing");
        nvs_commit(nvs_handle);
        
        // Перевірити збережене значення
        ESP_LOGI(TAG, "UART: Reading back saved values");
        char saved_date[12] = "";
        char saved_time[9] = "";
        size_t len = sizeof(saved_date);
        esp_err_t read_err = nvs_get_str(nvs_handle, "uart_date", saved_date, &len);
        if (read_err == ESP_OK) {
            len = sizeof(saved_time);
            read_err = nvs_get_str(nvs_handle, "uart_time", saved_time, &len);
            if (read_err == ESP_OK) {
                ESP_LOGI(TAG, "UART NVS READ: DATE: %s TIME: %s", saved_date, saved_time);
            } else {
                ESP_LOGE(TAG, "UART: Failed to read time from NVS: %s", esp_err_to_name(read_err));
            }
        } else {
            ESP_LOGE(TAG, "UART: Failed to read date from NVS: %s", esp_err_to_name(read_err));
        }
        
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "💾 UART OTA version saved to NVS");
        //ESP_LOGI(TAG, "UART DATE: %s TIME: %s", __DATE__, __TIME__);
        
        //version_saved = true;
    } else {
        ESP_LOGE(TAG, "❌ Failed to open uart_ota NVS: %s", esp_err_to_name(err));
    }
}



/**
 * Обчислити CRC16 для даних (MODBUS)
 */
uint16_t uart_ota_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/**
 * Обчислити CRC32 для даних
 */
uint32_t uart_ota_crc32(const uint8_t *data, size_t len)
{
    return esp_crc32_le(0, data, len);
}

/**
 * Відправити пакет по UART
 */
static esp_err_t send_packet(const void *data, size_t len)
{
    int written = uart_write_bytes(MASTER_UART_PORT, data, len);
    if (written != len) {
        ESP_LOGE(TAG, "UART write error: %d/%d bytes", written, (int)len);
        return ESP_FAIL;
    }
    uart_wait_tx_done(MASTER_UART_PORT, pdMS_TO_TICKS(100));
    return ESP_OK;
}

/**
 * Отримати відповідь від slave
 */
static esp_err_t receive_response(ota_response_t *resp, uint32_t timeout_ms)
{
    memset(resp, 0, sizeof(ota_response_t));
    
    ESP_LOGD(TAG, "🔍 Очікування відповіді (%u мс)...", timeout_ms);
    
    int len = uart_read_bytes(MASTER_UART_PORT, (uint8_t *)resp, 
                              sizeof(ota_response_t), pdMS_TO_TICKS(timeout_ms));
    
    if (len != sizeof(ota_response_t)) {
        ESP_LOGE(TAG, "Response timeout or incomplete: %d bytes (expected %d)", 
                 len, sizeof(ota_response_t));
        
        // Виведемо що отримали
        if (len > 0) {
            ESP_LOGE(TAG, "Received data: 0x%02X 0x%02X 0x%02X 0x%02X", 
                     ((uint8_t*)resp)[0], ((uint8_t*)resp)[1], 
                     ((uint8_t*)resp)[2], ((uint8_t*)resp)[3]);
        }
        return ESP_ERR_TIMEOUT;
    }
    
    if (resp->prefix != UART_CMD_PREFIX_OTA) {
        ESP_LOGE(TAG, "Invalid response prefix: 0x%02X (expected 0x55)", resp->prefix);
        return ESP_ERR_INVALID_RESPONSE;
    }
    
    ESP_LOGD(TAG, "✅ Отримано валідну відповідь: cmd=0x%02X, block=%d", 
             resp->response, resp->block_num);
    
    return ESP_OK;
}

/**
 * Відправити START команду
 */
static esp_err_t send_start_command(uint32_t total_size, uint32_t crc32)
{
    ota_start_packet_t pkt = {
        .prefix = UART_CMD_PREFIX_OTA,
        .cmd = OTA_CMD_START,
        .total_size = total_size,
        .crc32 = crc32
    };
    
    ESP_LOGI(TAG, "📤 Відправка START: size=%u, crc=0x%08X (packet size=%d)", 
             total_size, crc32, sizeof(pkt));
    
    esp_err_t ret = send_packet(&pkt, sizeof(pkt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Помилка відправки START пакету");
        return ret;
    }
    
    ESP_LOGI(TAG, "✅ START пакет відправлено, очікування READY...");
    vTaskDelay(pdMS_TO_TICKS(50));  // Зменшено з 100мс
    
    // Очікуємо READY (даємо більше часу для ініціалізації OTA)
    ota_response_t resp;
    ret = receive_response(&resp, UART_OTA_TIMEOUT_MS * 3);  // Зменшено до 3 секунд
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Timeout очікування READY");
        ESP_LOGI(TAG, "🔍 Можливі причини: slave не ініціалізований, UART проблеми, або slave зайнятий");
        return ret;
    }
    
    ESP_LOGI(TAG, "📥 Отримано відповідь: 0x%02X (block_num=%d)", 
             resp.response, resp.block_num);
    
    if (resp.response != OTA_RESP_READY) {
        ESP_LOGE(TAG, "❌ Slave не готовий: response=0x%02X", resp.response);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ Slave готовий до OTA");
    return ESP_OK;
}

/**
 * Відправити блок даних з повторними спробами
 */
static esp_err_t send_data_block(uint16_t block_num, const uint8_t *data, uint8_t data_len)
{
    // Формуємо пакет вручну щоб не відправляти весь 240-байтний масив
    uint8_t packet_buf[260];  // prefix + cmd + block_num + data_len + data + crc16
    size_t offset = 0;
    
    packet_buf[offset++] = UART_CMD_PREFIX_OTA;
    packet_buf[offset++] = OTA_CMD_DATA_BLOCK;
    
    // block_num (little-endian)
    packet_buf[offset++] = block_num & 0xFF;
    packet_buf[offset++] = (block_num >> 8) & 0xFF;
    
    packet_buf[offset++] = data_len;
    
    memcpy(&packet_buf[offset], data, data_len);
    offset += data_len;
    
    uint16_t crc16 = uart_ota_crc16(data, data_len);
    packet_buf[offset++] = crc16 & 0xFF;
    packet_buf[offset++] = (crc16 >> 8) & 0xFF;
    
    uint16_t current_block = block_num;
    for (int retry = 0; retry < UART_OTA_MAX_RETRIES; retry++) {
        if (retry > 0) {
            ESP_LOGW(TAG, "🔄 Повторна спроба #%d для блоку %d", retry, current_block);
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        // Update block_num in packet for retries (in case NACK requested a different block)
        packet_buf[2] = current_block & 0xFF;
        packet_buf[3] = (current_block >> 8) & 0xFF;

        esp_err_t ret = send_packet(packet_buf, offset);
        if (ret != ESP_OK) {
            continue;
        }

        ota_response_t resp;
        ret = receive_response(&resp, UART_OTA_TIMEOUT_MS);
        if (ret != ESP_OK) {
            continue;
        }

        if (resp.response == OTA_RESP_ACK && resp.block_num == current_block) {
            return ESP_OK;
        }

        if (resp.response == OTA_RESP_NACK) {
            ESP_LOGW(TAG, "⚠️ NACK для блоку %d (очікувався %d), повторюємо саме цей блок", resp.block_num, current_block);
            current_block = resp.block_num; // resend the block requested by slave
            continue;
        }
    }

    ESP_LOGE(TAG, "❌ Не вдалося відправити блок %d після %d спроб", current_block, UART_OTA_MAX_RETRIES);
    return ESP_FAIL;
}

/**
 * Відправити END команду
 */
static esp_err_t send_end_command(uint32_t final_crc32)
{
    ota_end_packet_t pkt = {
        .prefix = UART_CMD_PREFIX_OTA,
        .cmd = OTA_CMD_END,
        .final_crc32 = final_crc32
    };
    
    ESP_LOGI(TAG, "📤 Відправка END: crc=0x%08X", final_crc32);
    
    esp_err_t ret = send_packet(&pkt, sizeof(pkt));
    if (ret != ESP_OK) {
        return ret;
    }
    
    // Очікуємо COMPLETE
    ota_response_t resp;
    ret = receive_response(&resp, UART_OTA_TIMEOUT_MS * 5);  // Більше часу на фіналізацію
    if (ret != ESP_OK) {
        return ret;
    }
    
    if (resp.response != OTA_RESP_COMPLETE) {
        ESP_LOGE(TAG, "OTA не завершено: response=0x%02X", resp.response);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ OTA успішно завершено на slave");
    return ESP_OK;
}

/**
 * Основна функція для відправки firmware на slave через UART
 */
static esp_err_t uart_ota_send_firmware_locked(const uint8_t *firmware_data, size_t firmware_size,
                                               void (*progress_callback)(size_t sent, size_t total))
{
    if (!firmware_data || firmware_size == 0 || firmware_size > UART_OTA_MAX_SIZE) {
        ESP_LOGE(TAG, "Невірний розмір firmware: %d", (int)firmware_size);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "🚀 Початок UART OTA: %d байт", (int)firmware_size);
    
    // Очистити буфери UART на обох сторонах
    ESP_LOGI(TAG, "🧹 Очищення UART буферів...");
    uart_flush(MASTER_UART_PORT);
    vTaskDelay(pdMS_TO_TICKS(200));  // Дати час на очищення
    
    // Відправити кілька "пробних" байтів щоб "розбудити" slave
    uint8_t wake_bytes[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uart_write_bytes(MASTER_UART_PORT, wake_bytes, sizeof(wake_bytes));
    vTaskDelay(pdMS_TO_TICKS(100));
    uart_flush(MASTER_UART_PORT);
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGI(TAG, "📤 Відправка пробних байтів для ініціалізації slave...");
    
    // Додаткова затримка для ініціалізації UART task на slave
    ESP_LOGI(TAG, "⏳ Waiting 1 second for slave UART task initialization...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    // Обчислити CRC32 всього firmware
    uint32_t total_crc32 = uart_ota_crc32(firmware_data, firmware_size);
    ESP_LOGI(TAG, "📊 CRC32: 0x%08X", total_crc32);
    
    // Крок 1: Відправити START
    esp_err_t ret = send_start_command(firmware_size, total_crc32);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ START команда не вдалася");
        return ret;
    }
    
    // Крок 2: Відправити блоки даних
    size_t bytes_sent = 0;
    uint16_t block_num = 0;
    
    while (bytes_sent < firmware_size) {
        size_t remaining = firmware_size - bytes_sent;
        uint8_t block_size = (remaining > UART_OTA_BLOCK_SIZE) ? UART_OTA_BLOCK_SIZE : remaining;

        ret = send_data_block(block_num, firmware_data + bytes_sent, block_size);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ Помилка відправки блоку %d", block_num);

            // Спроба відміни OTA на slave
            uint8_t abort_cmd[2] = {UART_CMD_PREFIX_OTA, OTA_CMD_ABORT};
            send_packet(abort_cmd, 2);

            return ret;
        }

        bytes_sent += block_size;
        block_num++;

        // Callback для прогресу
        if (progress_callback) {
            progress_callback(bytes_sent, firmware_size);
        }

        // Логування кожні 128 блоків, на першому блоці або на 100%
        if ((block_num % 128 == 0) || (block_num == 1) || (bytes_sent == firmware_size)) {
            int percent = (bytes_sent * 100) / firmware_size;
            int last_block = (block_num > 0) ? (block_num - 1) : 0;
            ESP_LOGI(TAG, "📊 OTA: %3d%% (%d/%d bytes, block %d)",
                     percent, (int)bytes_sent, (int)firmware_size, last_block);
        }

        vTaskDelay(1);  // ACK already paces the transfer
    }

    ESP_LOGI(TAG, "✅ Всі блоки відправлено: %d блоків, %d байт", block_num, (int)bytes_sent);
    
    // Крок 3: Відправити END
    ret = send_end_command(total_crc32);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ END команда не вдалася");
        return ret;
    }
    
    ESP_LOGI(TAG, "🎉 UART OTA завершено успішно!");
    return ESP_OK;
}

esp_err_t uart_ota_send_firmware(const uint8_t *firmware_data, size_t firmware_size,
                                 void (*progress_callback)(size_t sent, size_t total))
{
    /* The whole transfer owns the UART: no version polling, no play commands. */
    if (!uart_link_lock(3000)) {
        ESP_LOGE(TAG, "UART busy; cannot start OTA");
        return ESP_ERR_TIMEOUT;
    }
    uart_link_set_ota_active(true);
    esp_err_t err = uart_ota_send_firmware_locked(firmware_data, firmware_size, progress_callback);
    uart_link_set_ota_active(false);
    uart_link_unlock();
    return err;
}
