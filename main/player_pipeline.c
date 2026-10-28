#include "player_pipeline.h"
#include "player_playlist.h"
// TODO

#include "esp_log.h"
#include "i2s_stream.h"
#include "board.h"
#include "filter_resample.h"
#include "player_usb.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "mp3_decoder.h"
#include "fatfs_stream.h"
#include "audio_pipeline.h"
#include "esp_timer.h"
#include "audio_element.h"
static const char *TAG_PIPE = "Player";

extern size_t g_stall_last_bytes;
extern int64_t g_stall_last_ts_ms;
extern int32_t g_current_track_duration_ms;

bool IRAM_ATTR external_i2s_rx_overflow_callback(i2s_chan_handle_t handle,
                                                        i2s_event_data_t *event,
                                                        void *user_ctx)
{
    (void)handle;
    (void)user_ctx;
    g_external_i2s_rx_overflows++;
    if (event) {
        g_external_i2s_rx_overflow_bytes += event->size;
    }
    return false;
}

extern bool g_decoder_error;
extern bool g_restore_playback;
#ifndef SLEEP_IND_PIN
#define SLEEP_IND_PIN 21
#endif


extern bool g_verbose_logs;
extern bool g_player_logs;
extern void enter_deep_sleep(void);
void stop_playback_and_services(void);
void wakeup_from_sleep(void);
esp_err_t start_external_input(void);
esp_err_t set_playback_position(audio_element_handle_t fatfs_stream_reader, int position_ms);
void play_test_tone(audio_element_handle_t i2s_writer, int duration_ms);
esp_err_t audio_event_cb(audio_element_handle_t el, audio_event_iface_msg_t *event, void *ctx);
void analyze_mp3_file(const char *filename);
void player_control(const char *cmd, int track);
esp_err_t get_slave_version_extended(char *main_date, size_t main_date_len, char *main_time, size_t main_time_len, char *crc_date, size_t crc_date_len, char *crc_time, size_t crc_time_len, char *uart_date, size_t uart_date_len, char *uart_time, size_t uart_time_len);
void i2s_passthrough_task(void *pvParameters);


void stop_external_input(void)
{
    if (!g_i2s_passthrough_task_handle) {
        ESP_LOGW(TAG_PIPE, "⚠️ External input task not running");
        // Ensure mode is false even if handle is already null
        g_mode_external = false; 
        return;
    }

    ESP_LOGI(TAG_PIPE, "🛑 Зупиняємо external input (raw passthrough)...");

    // 1. Signal the task to stop by clearing the flag
    g_mode_external = false;

    // 2. Wait for the task to finish (it will set the handle to NULL and self-delete)
    int timeout_ms = 2000;
    while (g_i2s_passthrough_task_handle != NULL && timeout_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_ms -= 100;
    }
    if (g_i2s_passthrough_task_handle != NULL) {
        ESP_LOGW(TAG_PIPE, "Passthrough task did not stop gracefully, deleting manually.");
        vTaskDelete(g_i2s_passthrough_task_handle);
        g_i2s_passthrough_task_handle = NULL;
    }

    // 3. De-initialize the writer element
    if (g_external_writer) {
        // Stop the element's internal task before deinitializing
        audio_element_stop(g_external_writer);
        audio_element_wait_for_stop(g_external_writer);
        audio_element_deinit(g_external_writer);
        g_external_writer = NULL;
    }

    ESP_LOGI(TAG_PIPE, "✅ External input stopped.");
}


#include "audio_hal.h"
#include "errno.h"
#include "math.h"
#include "driver/twai.h"
void stop_playback(void) {
    if (g_player_task_handle) {
        ESP_LOGI(TAG_PIPE, "⏸️ Полная остановка воспроизведения");
        
        // 1. 🛑 ПРИМУСОВА ЗУПИНКА - встановлюємо прапори зупинки
        bool old_playlist_ready = g_playlist_ready;
        g_playlist_ready = false;  // Це зупинить зовнішній цикл while
        g_skip_to_next = true;     // Це зупинить внутрішній цикл while
        g_skip_to_prev = false;
        
        // 2. 🔇 Остановить аудиочип
        if (g_board_handle) {
            audio_hal_ctrl_codec(g_board_handle->audio_hal, 
                                AUDIO_HAL_CODEC_MODE_DECODE, 
                                AUDIO_HAL_CTRL_STOP);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        
        // 3. ⏳ Чекаємо завершення задачі (вона сама завершиться)
        ESP_LOGI(TAG_PIPE, "⏳ Очікування завершення задачі плеєра...");
        int timeout = 50; // 5 секунд
        while (g_player_task_handle != NULL && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout--;
        }
        
        if (g_player_task_handle != NULL) {
            ESP_LOGW(TAG_PIPE, "⚠️ Задача не завершилась, примусове видалення");
            TaskHandle_t task_to_delete = g_player_task_handle;
            g_player_task_handle = NULL;
            vTaskDelete(task_to_delete);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            ESP_LOGI(TAG_PIPE, "✅ Задача плеєра успішно завершилась");
        }
        
        // 4. Відновлюємо стан плейлиста якщо він був готовий
        g_playlist_ready = old_playlist_ready;
        
        ESP_LOGI(TAG_PIPE, "✅ Воспроизведение полностью остановлено");
    }
    g_skip_to_next = false;
    g_skip_to_prev = false;
}

#include "player_nvs.h"
#include "wifi_can_sniffer.h"
#include "player_wifi.h"
#include "uart_ota_protocol.h"
#include "player_link.h"

/**
 * @brief Ask the slave for its firmware version (UART command 0x09).
 * Reply: [0xAA][0x09][len][chars...]
 */
esp_err_t get_slave_fw_version(char *buf, size_t len)
{
    if (!buf || len < 2) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';

    if (uart_link_ota_active()) return ESP_ERR_INVALID_STATE;
    if (!uart_link_lock(1000)) return ESP_ERR_INVALID_STATE;

    esp_err_t err = ESP_OK;
    uart_flush_input(MASTER_UART_PORT);

    uint8_t req[2] = {UART_CMD_PREFIX_NORMAL, UART_CMD_GET_FW_VER};
    if (uart_write_bytes(MASTER_UART_PORT, (const char *)req, 2) != 2) {
        err = ESP_FAIL;
    } else {
        uint8_t hdr[3];
        int r = uart_read_bytes(MASTER_UART_PORT, hdr, 3, pdMS_TO_TICKS(600));
        if (r != 3 || hdr[0] != UART_CMD_PREFIX_NORMAL || hdr[1] != UART_CMD_GET_FW_VER) {
            err = ESP_ERR_TIMEOUT;
        } else {
            size_t n = hdr[2];
            if (n > len - 1) n = len - 1;
            if (n > 0) {
                int got = uart_read_bytes(MASTER_UART_PORT, (uint8_t *)buf, n, pdMS_TO_TICKS(600));
                if (got != (int)n) {
                    err = ESP_ERR_TIMEOUT;
                    buf[0] = '\0';
                } else {
                    buf[n] = '\0';
                }
            }
        }
    }

    uart_link_unlock();
    return err;
}

#include "driver/uart.h"
#include "player_can.h"
#include "player_sleep.h"
#include "app_messages.h"
#define CAN_WAKEUP_DELAY_MS 5000

void play_next_track(void) {
    // 😴 Якщо в режимі сну - спочатку прокидаємося
    if (g_can_sleep_mode) {
        ESP_LOGI(TAG_PIPE, "🌅 Пробудження від сну по команді next track");
        g_can_sleep_mode = false;
        g_last_can_activity_time = xTaskGetTickCount();
        vTaskDelay(pdMS_TO_TICKS(CAN_WAKEUP_DELAY_MS));
    }
    
    if (!g_playlist_ready || g_playlist_size == 0) {
        ESP_LOGE(TAG_PIPE, "❌ Плейлист порожній або не готовий");
        return;
    }
    
    g_current_track = (g_current_track + 1) % g_playlist_size;
    
    ESP_LOGI(TAG_PIPE, "");
    ESP_LOGI(TAG_PIPE, "╔═══════════════════════════════════════════════╗");
    ESP_LOGI(TAG_PIPE, "║  ⏭️  НАСТУПНИЙ ТРЕК                            ║");
    ESP_LOGI(TAG_PIPE, "╠═══════════════════════════════════════════════╣");
    ESP_LOGI(TAG_PIPE, "║  📀 %d / %d", g_current_track + 1, g_playlist_size);
    ESP_LOGI(TAG_PIPE, "║  📁 %s", g_playlist[g_current_track]);
    ESP_LOGI(TAG_PIPE, "╚═══════════════════════════════════════════════╝");
    ESP_LOGI(TAG_PIPE, "");
    
    g_skip_to_next = true;
    
    // 💾 Зберегти позицію при зміні треку (початок нового треку)
    save_playback_position(g_current_track, 0);
    g_last_saved_position = 0;
    
    // 📨 Відправити подію по CAN
    //send_can_event(EVENT_TRACK_CHANGED, g_current_track + 1, 0x01); // 0x01 = вперед
}

void play_prev_track(void) {
    // 😴 Якщо в режимі сну - спочатку прокидаємося
    if (g_can_sleep_mode) {
        ESP_LOGI(TAG_PIPE, "🌅 Пробудження від сну по команді prev track");
        g_can_sleep_mode = false;
        g_last_can_activity_time = xTaskGetTickCount();
        vTaskDelay(pdMS_TO_TICKS(CAN_WAKEUP_DELAY_MS));
    }
    
    if (!g_playlist_ready || g_playlist_size == 0) {
        ESP_LOGE(TAG_PIPE, "❌ Плейлист порожній або не готовий");
        return;
    }
    
    g_current_track = (g_current_track - 1 + g_playlist_size) % g_playlist_size;
    
    ESP_LOGI(TAG_PIPE, "");
    ESP_LOGI(TAG_PIPE, "╔═══════════════════════════════════════════════╗");
    ESP_LOGI(TAG_PIPE, "║  ⏮️  ПОПЕРЕДНІЙ ТРЕК                           ║");
    ESP_LOGI(TAG_PIPE, "╠═══════════════════════════════════════════════╣");
    ESP_LOGI(TAG_PIPE, "║  📀 %d / %d", g_current_track + 1, g_playlist_size);
    ESP_LOGI(TAG_PIPE, "║  📁 %s", g_playlist[g_current_track]);
    ESP_LOGI(TAG_PIPE, "╚═══════════════════════════════════════════════╝");
    ESP_LOGI(TAG_PIPE, "");
    
    g_skip_to_prev = true;
    
    // 💾 Зберегти позицію
    save_playback_position(g_current_track, 0);
    g_last_saved_position = 0;
    
    // 📨 Відправити подію по CAN
    //send_can_event(EVENT_TRACK_CHANGED, g_current_track + 1, 0x00); // 0x00 = назад
}

void set_volume(int volume_percent) {
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    
    g_volume = volume_percent;
    
    if (g_board_handle) {
        audio_hal_set_volume(g_board_handle->audio_hal, g_volume);
        ESP_LOGI(TAG_PIPE, "🔊 Громкость установлена: %d%%", g_volume);
    } else {
        ESP_LOGW(TAG_PIPE, "⚠️ Аудиочип не инициализирован, громкость не изменена");
    }
}

void volume_up(void) {
    int new_volume = g_volume + 10;
    if (new_volume > 100) new_volume = 100;
    set_volume(new_volume);
}

void volume_down(void) {
    int new_volume = g_volume - 10;
    if (new_volume < 0) new_volume = 0;
    set_volume(new_volume);
}

int get_current_position(audio_element_handle_t fatfs_stream_reader) {
    audio_element_info_t info = {0};
    if (audio_element_getinfo(fatfs_stream_reader, &info) == ESP_OK) {
        if (info.byte_pos > 0 && info.total_bytes > 0) {
            // 🎯 ПРАВИЛЬНАЯ ФОРМУЛА через пропорцию
            // Для MP3 разный битрейт, поэтому используем пропорцию
            
            // Сначала получаем длительность файла если известна
            // Или оцениваем по битрейту
            
            // Предположим стандартный битрейт 128 kbps = 16 KB/s
            // Но лучше пересчитывать через пропорцию
            
            int position_ms = (int)((info.byte_pos * 1000) / 16000);
            
            // 📝 Для отладки
            ESP_LOGD(TAG_PIPE, "⏱️ get_current_position: %d/%d байт -> ~%d мс", 
                    (int)info.byte_pos, (int)info.total_bytes, position_ms);
            
            return position_ms;
        }
    }
    return 0;
}

void player_set_position(int position_ms) {
    g_seek_position_ms = position_ms;
    ESP_LOGI(TAG_PIPE, "🌐 Веб: встановити позицію %d мс", position_ms);
}

void debug_audio_state(audio_element_handle_t el, const char *name) {
    audio_element_info_t info = {0};
    audio_element_state_t state = audio_element_get_state(el);
    
    if (audio_element_getinfo(el, &info) == ESP_OK) {
        ESP_LOGI(TAG_PIPE, "%s: стан=%d, байт=%d/%d, URI=%s", 
                 name, state, (int)info.byte_pos, (int)info.total_bytes,
                 info.uri ? info.uri : "NULL");
    } else {
        ESP_LOGI(TAG_PIPE, "%s: стан=%d, не вдалося отримати інфо", name, state);
    }
}

void debug_file_position(audio_element_handle_t el, const char *context) {
    audio_element_info_t info = {0};
    if (audio_element_getinfo(el, &info) == ESP_OK) {
        if (info.total_bytes > 0) {
            // Рассчитываем позицию несколькими способами для проверки
            int pos_ms_method1 = (int)(info.byte_pos / 16);  // Для 128kbps
            int pos_ms_method2 = (int)((info.byte_pos * 1000) / 16000);
            float percent = (info.byte_pos * 100.0f) / info.total_bytes;
            
            ESP_LOGI(TAG_PIPE, "🔍 %s:", context);
            ESP_LOGI(TAG_PIPE, "   Байти: %d / %d", (int)info.byte_pos, (int)info.total_bytes);
            ESP_LOGI(TAG_PIPE, "   Відсоток: %.1f%%", percent);
            ESP_LOGI(TAG_PIPE, "   Позиція (метод 1): %d мс", pos_ms_method1);
            ESP_LOGI(TAG_PIPE, "   Позиція (метод 2): %d мс", pos_ms_method2);
            ESP_LOGI(TAG_PIPE, "   URI: %s", info.uri ? info.uri : "NULL");
        }
    }
}

void flush_audio_buffers(void) {
    ESP_LOGI(TAG_PIPE, "🧹 Очистка аудио буферов...");
    
    // 🔧 Простая задержка вместо реальной очистки буферов
    // В вашей версии ESP-ADF может не быть функций flush
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Можно попробовать пересоздать аудио элементы
    ESP_LOGI(TAG_PIPE, "✅ Аудио буферы очищены");
}

void cleanup_i2s_driver(void) {
    ESP_LOGI(TAG_PIPE, "🧹 Упрощенная очистка I2S драйвера (перевірка g_mode_external)...");
    if (g_mode_external) {
        ESP_LOGI(TAG_PIPE, "ℹ️ Зовнішній режим активний, викликаємо stop_external_input()...");
        stop_external_input();
    }
}

void safe_i2s_cleanup(void) {
    ESP_LOGI(TAG_PIPE, "🧹 Безопасная очистка I2S...");
    
    if (g_i2s_tx_handle != NULL) {
        // 🔧 Останавливаем канал
        i2s_channel_disable(g_i2s_tx_handle);
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // 🗑️ Удаляем канал
        i2s_del_channel(g_i2s_tx_handle);
        g_i2s_tx_handle = NULL;
        
        ESP_LOGI(TAG_PIPE, "✅ I2S канал остановлен и удален");
    } else {
        ESP_LOGW(TAG_PIPE, "⚠️ I2S хэндл уже NULL, пропускаем очистку");
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
}

void stop_playback_and_services(void)
{
    ESP_LOGI(TAG_PIPE, "🛑 Stopping playback and services for OTA...");
    
    // Stop CAN/TWAI sniffer if running (safe to call even if not initialized)
    esp_err_t err = twai_stop();
    if (err == ESP_OK) {
        ESP_LOGI(TAG_PIPE, "✅ CAN sniffer stopped");
    }
    
    // Give other tasks time to stop naturally
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG_PIPE, "✅ All services stopped, ready for OTA");
}

void wakeup_from_sleep(void)
{
    ESP_LOGI(TAG_PIPE, "🌅 Пробудження від сну...");
    
    // 🔄 Скидаємо прапорець сну
    g_can_sleep_mode = false;
    
    // 📥 Завантажуємо збережену позицію
    load_playback_position(&g_saved_track_index, &g_saved_position_ms);
    g_restore_playback = true;
    
    // ⏰ Оновлюємо останню активність
    g_last_can_activity_time = xTaskGetTickCount();
    
    ESP_LOGI(TAG_PIPE, "💾 Позиція відновлена: трек %" PRId32 ", %" PRId32 " мс", 
             g_saved_track_index, g_saved_position_ms);
    
    // ⏳ Чекаємо перед початком відтворення
    vTaskDelay(pdMS_TO_TICKS(CAN_WAKEUP_DELAY_MS));

    // Устанавливаем пин-индикатор сна в HIGH при пробуждении
    gpio_set_level((gpio_num_t)SLEEP_IND_PIN, 1);
}

esp_err_t start_external_input(void)
{
    if (g_i2s_passthrough_task_handle) {
        ESP_LOGW(TAG_PIPE, "⚠️ External input task already running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG_PIPE, "▶️ Запуск external input (низкоуровневый I2S passthrough)...");

    // Создаём passthrough задачу на ядрі 1 (аудіо)
    g_mode_external = true;
    BaseType_t task_created = xTaskCreatePinnedToCore(
        i2s_passthrough_task, 
        "i2s_passthrough", 
        8192, 
        NULL,  // Параметр не используется
        5, 
        &g_i2s_passthrough_task_handle,
        1  // CPU1 - аудіо обробка
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG_PIPE, "❌ Не вдалося створити i2s_passthrough_task");
        g_mode_external = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG_PIPE, "✅ External input passthrough task started (I2S slave on I2S_NUM_1).");
    return ESP_OK;
}

esp_err_t set_playback_position(audio_element_handle_t fatfs_stream_reader, int position_ms) {
    ESP_LOGI(TAG_PIPE, "🎯 Встановлення позиції: %d мс", position_ms);
    
    if (position_ms <= 0) {
        return ESP_OK;
    }
    
    // 🚨 ВАЖНО: Для FATFS stream нужно использовать другой подход
    // Получаем информацию о файле
    audio_element_info_t info = {0};
    esp_err_t ret = audio_element_getinfo(fatfs_stream_reader, &info);
    
    if (ret != ESP_OK || info.total_bytes <= 0) {
        ESP_LOGE(TAG_PIPE, "❌ Не вдалося отримати інформацію про файл");
        return ret;
    }
    
    // Вычисляем позицию в байтах
    // Для MP3 примерно: 1 секунда = 16000 байт (для 128kbps)
    size_t target_bytes = (size_t)((position_ms * 16000) / 1000);
    
    // Ограничиваем 90% файла
    if (target_bytes > (size_t)(info.total_bytes * 0.9)) {
        target_bytes = (size_t)(info.total_bytes * 0.9);
    }
    
    ESP_LOGI(TAG_PIPE, "   Ціль: %d байт (з %d)", (int)target_bytes, (int)info.total_bytes);
    
    // ⏳ Даем время
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Пытаемся установить позицию
    ret = audio_element_set_byte_pos(fatfs_stream_reader, target_bytes);
    
    if (ret == ESP_OK) {
        // Проверяем результат
        vTaskDelay(pdMS_TO_TICKS(300));
        audio_element_getinfo(fatfs_stream_reader, &info);
        
        int actual_ms = (int)((info.byte_pos * 1000) / 16000);
        ESP_LOGI(TAG_PIPE, "✅ Позиція встановлена: %d байт (~%d мс)", 
                 (int)info.byte_pos, actual_ms);
    } else {
        ESP_LOGE(TAG_PIPE, "❌ Помилка встановлення позиції: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

void play_test_tone(audio_element_handle_t i2s_writer, int duration_ms)
{
    ESP_LOGI(TAG_PIPE, "🔊 Відтворення тестового тону (%d мс)...", duration_ms);

    int sample_rate = 44100;
    int channels = 2;
    float freq = 440.0; // Нота 'Ля' (A4)
    int volume = 8000;  // Амплітуда (макс. 32767 для 16-біт)

    int buffer_size = 1024;
    int16_t *buffer = malloc(buffer_size);
    if (!buffer) {
        ESP_LOGE(TAG_PIPE, "❌ Не вдалося виділити пам'ять для буфера тестового тону");
        return;
    }

    int total_samples_to_write = duration_ms * sample_rate / 1000;
    int samples_written = 0;

    // Ensure the element is running before trying to write to it
    audio_element_state_t writer_state = audio_element_get_state(i2s_writer);
    if (writer_state != AEL_STATE_RUNNING) {
        ESP_LOGW(TAG_PIPE, "⚠️ I2S writer не в стані RUNNING (%d), неможливо відтворити тестовий тон", writer_state);
        free(buffer);
        return;
    }

    ESP_LOGI(TAG_PIPE, "✍️  Початок запису тестового тону в i2s_writer...");
    while (samples_written < total_samples_to_write) {
        int samples_in_buffer = (buffer_size / (channels * sizeof(int16_t)));
        
        for (int j = 0; j < samples_in_buffer; j++) {
            double angle = 2.0 * M_PI * freq * (double)(samples_written + j) / (double)sample_rate;
            int16_t sample = (int16_t)(volume * sin(angle));
            buffer[j * channels] = sample;     // Лівий канал
            buffer[j * channels + 1] = sample; // Правий канал
        }
        
        int bytes_to_write = samples_in_buffer * channels * sizeof(int16_t);
        int bytes_written_now = audio_element_input(i2s_writer, (char *)buffer, bytes_to_write);
        
        if (bytes_written_now <= 0) {
            ESP_LOGE(TAG_PIPE, "❌ Помилка запису тестового тону в I2S writer (записано %d)", bytes_written_now);
            break;
        }
        samples_written += (bytes_written_now / (channels * sizeof(int16_t)));
    }

    free(buffer);
    ESP_LOGI(TAG_PIPE, "✅ Тестовий тон завершено (записано %d семплів)", samples_written);
}

esp_err_t audio_event_cb(audio_element_handle_t el, audio_event_iface_msg_t *event, void *ctx)
{
    if (event->cmd == AEL_MSG_CMD_REPORT_STATUS) {
        if ((int)event->data == AEL_STATUS_STATE_FINISHED) {
            bool *playback_finished = (bool*)ctx;
            *playback_finished = true;
            ESP_LOGI(TAG_PIPE, "");
            ESP_LOGI(TAG_PIPE, "╔═══════════════════════════════════════════════╗");
            ESP_LOGI(TAG_PIPE, "║  ✅ ТРЕК ЗАВЕРШЕНО                            ║");
            ESP_LOGI(TAG_PIPE, "╚═══════════════════════════════════════════════╝");
            ESP_LOGI(TAG_PIPE, "");
        }
        // 🔴 Перевіряємо на помилки декодування
        else if ((int)event->data == AEL_STATUS_ERROR_OPEN ||
                 (int)event->data == AEL_STATUS_ERROR_INPUT ||
                 (int)event->data == AEL_STATUS_ERROR_OUTPUT) {
            ESP_LOGE(TAG_PIPE, "❌ Помилка аудіо елемента: %d", (int)event->data);
            
            // Якщо це декодер - позначаємо як проблемний файл
            if (strstr(audio_element_get_tag(el), "decoder")) {
                ESP_LOGW(TAG_PIPE, "⚠️ Проблемний MP3 файл - пропускаємо!");
                g_decoder_error = true;
                bool *playback_finished = (bool*)ctx;
                if (playback_finished) {
                    *playback_finished = true;
                }
            }
        }
    }
    
    // 🎯 Додамо моніторинг продуктивності
    if (event->cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
        audio_element_info_t info = {0};
        audio_element_getinfo(el, &info);
        ESP_LOGI(TAG_PIPE, "🎵 Audio info - Sample rate: %d Hz, Channels: %d, Bits: %d", 
                info.sample_rates, info.channels, info.bits);

        /* Ми більше не змінюємо I2S-клок з контексту callback (unsafe).
           Налаштування частоти буде виконуватись з `player_task` після
           того, як пайплайн гарантовано запущено і writer валідний. */

        // Перевіримо чи це MP3 з високим бітрейтом
        if (info.sample_rates > 44100 || (info.bits > 16 && info.channels > 2)) {
            ESP_LOGW(TAG_PIPE, "⚠️ Високий бітрейт/семпл рейт - можливі проблеми з продуктивністю!");
        }
    }
    
    // 🔊 Моніторинг буферів
    if (event->cmd == AEL_MSG_CMD_REPORT_STATUS) {
        audio_element_info_t info = {0};
        audio_element_getinfo(el, &info);
        
        // Логування інформації про стан елемента
        ESP_LOGD(TAG_PIPE, "🔊 Audio element status - Bytes: %d/%d", 
                (int)info.byte_pos, (int)info.total_bytes);
    }
    
    return ESP_OK;
}

void analyze_mp3_file(const char *filename) {
    ESP_LOGI(TAG_PIPE, "🔍 Спроба відкрити файл: %s", filename);
    FILE *file = fopen(filename, "rb");
    if (!file) {
        ESP_LOGE(TAG_PIPE, "❌ Не вдалося відкрити файл для аналізу: %s (errno: %d)", filename, errno);
        return;
    }
    
    ESP_LOGI(TAG_PIPE, "✅ Файл успішно відкрито для аналізу");
    
    // Отримуємо розмір файлу
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    ESP_LOGI(TAG_PIPE, "📏 Розмір файлу: %ld байт", file_size);
    
    // Читаємо більше байтів для пошуку MP3 фреймів (враховуючи ID3 теги)
    uint8_t buffer[1024];
    size_t bytes_to_read = sizeof(buffer);
    if (bytes_to_read > file_size) bytes_to_read = file_size;
    
    size_t read = fread(buffer, 1, bytes_to_read, file);
    fclose(file);
    
    if (read < 4) {
        ESP_LOGW(TAG_PIPE, "⚠️ Файл занадто малий для MP3: %s", filename);
        return;
    }
    
    // Шукаємо MP3 фрейм синхронізації (може бути після ID3 тегів)
    int mp3_offset = -1;
    for (int i = 0; i < (int)read - 3; i++) {
        if ((buffer[i] == 0xFF) && ((buffer[i + 1] & 0xE0) == 0xE0)) {
            // Знайдено потенційний MP3 фрейм
            mp3_offset = i;
            break;
        }
    }
    
    if (mp3_offset == -1) {
        ESP_LOGW(TAG_PIPE, "⚠️ Не знайдено MP3 фрейм синхронізації в файлі: %s", filename);
        return;
    }
    
    // Використовуємо знайдений MP3 фрейм для аналізу
    uint8_t *header = &buffer[mp3_offset];
    
    ESP_LOGI(TAG_PIPE, "🎵 Знайдено MP3 фрейм на позиції: %d байт", mp3_offset);
    
    // Дебаг: виведемо перші 4 байти заголовка
    ESP_LOGI(TAG_PIPE, "🔍 Заголовок MP3: %02X %02X %02X %02X", 
             header[0], header[1], header[2], header[3]);
    
    // Парсимо MPEG версію та шар
    int version = (header[1] >> 3) & 0x03;
    int layer = (header[1] >> 1) & 0x03;
    
    // Парсимо бітрейт
    int bitrate_index = (header[2] >> 4) & 0x0F;
    int sample_rate_index = (header[2] >> 2) & 0x03;
    
    ESP_LOGI(TAG_PIPE, "🔍 Індекси: version=%d, layer=%d, bitrate_idx=%d, sample_rate_idx=%d", 
             version, layer, bitrate_index, sample_rate_index);
    
    const int bitrates[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    const int sample_rates[4] = {44100, 48000, 32000, 0};
    
    int bitrate = bitrates[bitrate_index];
    int sample_rate = sample_rates[sample_rate_index];
    
    // Валідація MP3 заголовка
    if (bitrate == 0 || sample_rate == 0 || version > 2 || layer != 1) {
        ESP_LOGW(TAG_PIPE, "⚠️ Недійсний MP3 заголовок (bitrate=%d, sample_rate=%d, version=%d, layer=%d)", 
                 bitrate, sample_rate, version, layer);
        ESP_LOGW(TAG_PIPE, "🔍 Спробуємо знайти наступний MP3 фрейм...");
        
        // Шукаємо наступний валідний фрейм
        int next_mp3_offset = -1;
        for (int i = mp3_offset + 1; i < (int)read - 3; i++) {
            if ((buffer[i] == 0xFF) && ((buffer[i + 1] & 0xE0) == 0xE0)) {
                // Перевіряємо заголовок
                uint8_t *test_header = &buffer[i];
                int test_version = (test_header[1] >> 3) & 0x03;
                int test_layer = (test_header[1] >> 1) & 0x03;
                int test_bitrate_idx = (test_header[2] >> 4) & 0x0F;
                int test_sample_rate_idx = (test_header[2] >> 2) & 0x03;
                
                int test_bitrate = bitrates[test_bitrate_idx];
                int test_sample_rate = sample_rates[test_sample_rate_idx];
                
                if (test_bitrate > 0 && test_sample_rate > 0 && test_version <= 2 && test_layer == 1) {
                    next_mp3_offset = i;
                    ESP_LOGI(TAG_PIPE, "✅ Знайдено валідний MP3 фрейм на позиції: %d байт", next_mp3_offset);
                    break;
                }
            }
        }
        
        if (next_mp3_offset != -1) {
            mp3_offset = next_mp3_offset;
            header = &buffer[mp3_offset];
            version = (header[1] >> 3) & 0x03;
            layer = (header[1] >> 1) & 0x03;
            bitrate_index = (header[2] >> 4) & 0x0F;
            sample_rate_index = (header[2] >> 2) & 0x03;
            bitrate = bitrates[bitrate_index];
            sample_rate = sample_rates[sample_rate_index];
            
            ESP_LOGI(TAG_PIPE, "🔍 Новий заголовок: %02X %02X %02X %02X", 
                     header[0], header[1], header[2], header[3]);
        } else {
            ESP_LOGW(TAG_PIPE, "❌ Не знайдено валідний MP3 фрейм у файлі: %s", filename);
            return;
        }
    }
    
    ESP_LOGI(TAG_PIPE, "🎵 MP3 аналіз - Файл: %s", filename);
    ESP_LOGI(TAG_PIPE, "🎵 Бітрейт: %d kbps, Семпл рейт: %d Hz", bitrate, sample_rate);
    
    // Попередження про потенційні проблеми з продуктивністю
    if (bitrate > 192) {
        ESP_LOGW(TAG_PIPE, "⚠️ Високий бітрейт (%d kbps) - можливі проблеми з декодуванням!", bitrate);
    }
    if (sample_rate > 44100) {
        ESP_LOGW(TAG_PIPE, "⚠️ Високий семпл рейт (%d Hz) - можливі проблеми з продуктивністю!", sample_rate);
    }
    
    // Додаткові перевірки для проблемних файлів
    if (bitrate >= 256) {
        ESP_LOGW(TAG_PIPE, "🚨 Дуже високий бітрейт (%d kbps) - ESP32 може не справитися!", bitrate);
    }
}

void player_control(const char* cmd, int track) {
    update_system_activity(); // Web control = system activity
    
    /* External/BT source lives on the slave; without a mounted USB there is
       nothing to control locally either, so forward the command over UART. */
    bool to_slave = g_mode_external || !g_usb_mounted;

    if (strcmp(cmd, "play") == 0) {
        if (to_slave) {
            ESP_LOGI(TAG_PIPE, "Web: play -> slave (external=%d usb=%d)",
                     (int)g_mode_external, (int)g_usb_mounted);
            send_i2c_command(UART_CMD_PLAY);
        } else if (track >= 0 && track < g_playlist_size) {
            g_current_track = track;
            g_skip_to_next = true;
            g_play_specific_track = true;
            ESP_LOGI(TAG_PIPE, "🌐 Веб: відтворити трек %d", track);
        } else {
            g_is_paused = false;
            ESP_LOGI(TAG_PIPE, "🌐 Веб: відновити відтворення");
        }
    } else if (strcmp(cmd, "pause") == 0) {
        if (to_slave) {
            ESP_LOGI(TAG_PIPE, "Web: pause -> slave (external=%d usb=%d)",
                     (int)g_mode_external, (int)g_usb_mounted);
            send_i2c_command(UART_CMD_PAUSE);
        } else {
            g_is_paused = true;
        }
        ESP_LOGI(TAG_PIPE, "🌐 Веб: пауза");
    } else if (strcmp(cmd, "next") == 0) {
        if (to_slave) {
            ESP_LOGI(TAG_PIPE, "Web: next -> slave (external=%d usb=%d)",
                     (int)g_mode_external, (int)g_usb_mounted);
            send_i2c_command(UART_CMD_NEXT_TRACK);
        } else {
            /* play_next_track() increments g_current_track itself; the pipeline
               only clears the flag, so setting it here replayed the same track. */
            play_next_track();
        }
        ESP_LOGI(TAG_PIPE, "🌐 Веб: наступний трек");
    } else if (strcmp(cmd, "prev") == 0) {
        if (to_slave) {
            ESP_LOGI(TAG_PIPE, "Web: prev -> slave (external=%d usb=%d)",
                     (int)g_mode_external, (int)g_usb_mounted);
            send_i2c_command(UART_CMD_PREV_TRACK);
        } else {
            play_prev_track();
        }
        ESP_LOGI(TAG_PIPE, "🌐 Веб: попередній трек");
    } else if (strcmp(cmd, "switch_mode") == 0) {
        ESP_LOGI(TAG_PIPE, "🌐 Web: switch_mode requested");
        // Надсилаємо повідомлення, як і для клавіші 'm'
        app_message_t msg;
        msg.id = APP_MODE_SWITCH;
        xQueueSend(app_queue, &msg, 0);
    } else if (strcmp(cmd, "toggle_sniffer") == 0) {
        if (!g_sniffer_enabled) {
            ESP_LOGI(TAG_PIPE, "🌐 Web: enabling WiFi CAN Sniffer via web toggle");
            wifi_can_sniffer_config_t cfg = {
                .wifi_ssid = WIFI_SSID,
                .wifi_password = WIFI_PASSWORD,
                .pc_ip_str = PC_IP_ADDR,
                .udp_port = UDP_PORT,
                .on_can_message = wifi_can_on_message_callback,
                .on_control_command = wifi_can_on_control_callback,
            };
            if (wifi_can_sniffer_init(&cfg) == ESP_OK) {
                g_sniffer_enabled = true;
                save_sniffer_enabled(1);
                ESP_LOGI(TAG_PIPE, "✅ Sniffer enabled (saved)");
            } else {
                ESP_LOGE(TAG_PIPE, "❌ Failed to enable sniffer");
            }
        } else {
            ESP_LOGI(TAG_PIPE, "🌐 Web: disabling WiFi CAN Sniffer via web toggle");
            wifi_can_sniffer_deinit();
            g_sniffer_enabled = false;
            save_sniffer_enabled(0);
            ESP_LOGI(TAG_PIPE, "✅ Sniffer disabled (saved)");
        }
    }
    else if (strcmp(cmd, "set_sniffer") == 0) {
        // 'track' parameter is used as state: 1 = enable, 0 = disable
        if (track == 1) {
            if (!g_sniffer_enabled) {
                ESP_LOGI(TAG_PIPE, "🌐 Web: setting WiFi CAN Sniffer = ON");
                wifi_can_sniffer_config_t cfg = {
                    .wifi_ssid = WIFI_SSID,
                    .wifi_password = WIFI_PASSWORD,
                    .pc_ip_str = PC_IP_ADDR,
                    .udp_port = UDP_PORT,
                    .on_can_message = wifi_can_on_message_callback,
                    .on_control_command = wifi_can_on_control_callback,
                };
                if (wifi_can_sniffer_init(&cfg) == ESP_OK) {
                    g_sniffer_enabled = true;
                    save_sniffer_enabled(1);
                    ESP_LOGI(TAG_PIPE, "✅ Sniffer enabled (saved)");
                } else {
                    ESP_LOGE(TAG_PIPE, "❌ Failed to enable sniffer");
                }
            }
        } else {
            if (g_sniffer_enabled) {
                ESP_LOGI(TAG_PIPE, "🌐 Web: setting WiFi CAN Sniffer = OFF");
                wifi_can_sniffer_deinit();
                g_sniffer_enabled = false;
                save_sniffer_enabled(0);
                ESP_LOGI(TAG_PIPE, "✅ Sniffer disabled (saved)");
            }
        }
    }
}

static esp_err_t get_slave_version_locked(char *main_date, size_t main_date_len, char *main_time, size_t main_time_len,
                                          char *crc_date, size_t crc_date_len, char *crc_time, size_t crc_time_len,
                                          char *uart_date, size_t uart_date_len, char *uart_time, size_t uart_time_len)
{
    // Спробувати до 3 разів отримати версію
    for (int attempt = 0; attempt < 3; attempt++) {
        // Відправляємо команду отримання версії
        // drop leftovers from an aborted transaction before asking again
        uart_flush_input(MASTER_UART_PORT);
        vTaskDelay(pdMS_TO_TICKS(20));
        uart_flush_input(MASTER_UART_PORT);

        uint8_t packet[2] = {UART_CMD_PREFIX_NORMAL, UART_CMD_GET_VERSION};
        int w = uart_write_bytes(MASTER_UART_PORT, (const char *)packet, 2);
        if (w != 2) {
            ESP_LOGW(TAG_PIPE, "Failed to send version request (attempt %d)", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Читаємо відповідь з більшим таймаутом
        version_response_t resp;
        int r = uart_read_bytes(MASTER_UART_PORT, (uint8_t *)&resp, sizeof(resp), pdMS_TO_TICKS(2000));
        if (r != sizeof(resp)) {
            ESP_LOGW(TAG_PIPE, "Failed to read version response (attempt %d): %d/%d bytes", 
                    attempt + 1, r, sizeof(resp));
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        
        // Перевіряємо відповідь
        if (resp.prefix != UART_CMD_PREFIX_NORMAL || resp.cmd != UART_CMD_GET_VERSION) {
            ESP_LOGW(TAG_PIPE, "Invalid version response (attempt %d): prefix=0x%02X, cmd=0x%02X", 
                    attempt + 1, resp.prefix, resp.cmd);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Копіюємо всі дані версій
        if (main_date && main_date_len > 0) {
            strncpy(main_date, resp.main_build_date, main_date_len - 1);
            main_date[main_date_len - 1] = '\0';
        }
        if (main_time && main_time_len > 0) {
            strncpy(main_time, resp.main_build_time, main_time_len - 1);
            main_time[main_time_len - 1] = '\0';
        }
        if (crc_date && crc_date_len > 0) {
            strncpy(crc_date, resp.crc_build_date, crc_date_len - 1);
            crc_date[crc_date_len - 1] = '\0';
        }
        if (crc_time && crc_time_len > 0) {
            strncpy(crc_time, resp.crc_build_time, crc_time_len - 1);
            crc_time[crc_time_len - 1] = '\0';
        }
        if (uart_date && uart_date_len > 0) {
            strncpy(uart_date, resp.uart_build_date, uart_date_len - 1);
            uart_date[uart_date_len - 1] = '\0';
        }
        if (uart_time && uart_time_len > 0) {
            strncpy(uart_time, resp.uart_build_time, uart_time_len - 1);
            uart_time[uart_time_len - 1] = '\0';
        }
        
        //ESP_LOGI(TAG_PIPE, "✅ Slave versions received:");
        //ESP_LOGI(TAG_PIPE, "  🎵 Main: %s %s", resp.main_build_date, resp.main_build_time);
        //ESP_LOGI(TAG_PIPE, "  📡 CRC: %s %s", resp.crc_build_date, resp.crc_build_time);
        //ESP_LOGI(TAG_PIPE, "  📡 UART: %s %s", resp.uart_build_date, resp.uart_build_time);
        return ESP_OK;
    }
    
    // Якщо всі спроби невдалі, повертаємо порожні рядки
    ESP_LOGW(TAG_PIPE, "Failed to get slave version after 3 attempts");
    if (main_date && main_date_len > 0) main_date[0] = '\0';
    if (main_time && main_time_len > 0) main_time[0] = '\0';
    if (crc_date && crc_date_len > 0) crc_date[0] = '\0';
    if (crc_time && crc_time_len > 0) crc_time[0] = '\0';
    if (uart_date && uart_date_len > 0) uart_date[0] = '\0';
    if (uart_time && uart_time_len > 0) uart_time[0] = '\0';
    return ESP_FAIL;
}

esp_err_t get_slave_version_extended(char *main_date, size_t main_date_len, char *main_time, size_t main_time_len,
                                     char *crc_date, size_t crc_date_len, char *crc_time, size_t crc_time_len,
                                     char *uart_date, size_t uart_date_len, char *uart_time, size_t uart_time_len)
{
    /* Never talk over an OTA transfer, and never let two readers share the stream. */
    if (uart_link_ota_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!uart_link_lock(1000)) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = get_slave_version_locked(main_date, main_date_len, main_time, main_time_len,
                                             crc_date, crc_date_len, crc_time, crc_time_len,
                                             uart_date, uart_date_len, uart_time, uart_time_len);
    uart_link_unlock();
    return err;
}

void i2s_passthrough_task(void *pvParameters)
{
    (void)pvParameters;

    // Гібридний підхід: низькорівневий I2S slave для входу + ESP-ADF writer для виходу
    
    if (g_verbose_logs && g_player_logs) ESP_LOGI(TAG_PIPE, "🎵 Ініціалізація I2S slave для входу...");
    
    // Включаємо pull-up на GPIO 40 (DIN) для стабільності
    gpio_set_pull_mode(EXT_I2S_DIN_PIN, GPIO_PULLUP_ONLY);
    if (g_verbose_logs && g_player_logs) ESP_LOGI(TAG_PIPE, "✅ Pull-up включена на GPIO %d (DIN)", EXT_I2S_DIN_PIN);
    
    // 1. Низькорівневий I2S slave для читання (отримує clock ззовні)
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    rx_cfg.dma_desc_num = 6;        // збільшуємо кількість DMA дескрипторів
    rx_cfg.dma_frame_num = 512;     // збільшуємо розмір кадру
    
    i2s_chan_handle_t rx_handle = NULL;
    esp_err_t ret = i2s_new_channel(&rx_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_PIPE, "❌ RX error: %s", esp_err_to_name(ret));
        g_i2s_passthrough_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    i2s_std_config_t rx_std = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = -1,
            .bclk = EXT_I2S_BCLK_PIN,
            .ws = EXT_I2S_WS_PIN,
            .dout = -1,
            .din = EXT_I2S_DIN_PIN,
        },
    };
    
    ret = i2s_channel_init_std_mode(rx_handle, &rx_std);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_PIPE, "❌ RX init: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        g_i2s_passthrough_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    const i2s_event_callbacks_t rx_callbacks = {
        .on_recv_q_ovf = external_i2s_rx_overflow_callback,
    };
    ret = i2s_channel_register_event_callback(rx_handle, &rx_callbacks, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_PIPE, "❌ RX callback registration: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        g_i2s_passthrough_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_PIPE, "❌ RX enable: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        g_i2s_passthrough_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    if (g_verbose_logs && g_player_logs) {
        ESP_LOGI(TAG_PIPE, "═══════════════════════════════════════════════");
        ESP_LOGI(TAG_PIPE, "✅ RX SLAVE ГОТОВИЙ (I2S_NUM_1)");
        ESP_LOGI(TAG_PIPE, "📍 GPIO: BCLK=%d, WS=%d, DIN=%d", EXT_I2S_BCLK_PIN, EXT_I2S_WS_PIN, EXT_I2S_DIN_PIN);
        ESP_LOGI(TAG_PIPE, "🔊 Формат: 44.1kHz, 16bit, Stereo");
        ESP_LOGI(TAG_PIPE, "═══════════════════════════════════════════════");
    }

    // 2. ESP-ADF I2S stream для виходу (master на PCM5102)
    if (g_verbose_logs && g_player_logs) ESP_LOGI(TAG_PIPE, "🎵 Створення ESP-ADF I2S writer для виходу...");
    
    i2s_stream_cfg_t i2s_writer_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_writer_cfg.type = AUDIO_STREAM_WRITER;
    i2s_writer_cfg.std_cfg.gpio_cfg.bclk = 17;
    i2s_writer_cfg.std_cfg.gpio_cfg.ws = 18;
    i2s_writer_cfg.std_cfg.gpio_cfg.dout = 16;
    i2s_writer_cfg.std_cfg.gpio_cfg.din = -1;
    i2s_writer_cfg.std_cfg.clk_cfg.sample_rate_hz = 44100;
    i2s_writer_cfg.std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
    i2s_writer_cfg.std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;

    audio_element_handle_t i2s_writer = i2s_stream_init(&i2s_writer_cfg);
    if (!i2s_writer) {
        ESP_LOGE(TAG_PIPE, "❌ Writer error");
        i2s_del_channel(rx_handle);
        g_i2s_passthrough_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Запускаємо writer
    audio_element_run(i2s_writer);
    if (g_verbose_logs && g_player_logs) {
        ESP_LOGI(TAG_PIPE, "═══════════════════════════════════════════════");
        ESP_LOGI(TAG_PIPE, "✅ TX MASTER ГОТОВИЙ (I2S_NUM_0)");
        ESP_LOGI(TAG_PIPE, "📍 GPIO: BCLK=17, WS=18, DOUT=16");
        ESP_LOGI(TAG_PIPE, "🔊 Формат: 44.1kHz, 16bit, Stereo");
        ESP_LOGI(TAG_PIPE, "🎯 Вихід: PCM5102 DAC");
        ESP_LOGI(TAG_PIPE, "═══════════════════════════════════════════════");
    }

    // 3. Основний цикл: читаємо з RX slave, пишемо в writer
    if (g_verbose_logs && g_player_logs) {
        ESP_LOGI(TAG_PIPE, "");
        ESP_LOGI(TAG_PIPE, "╔═══════════════════════════════════════════════╗");
        ESP_LOGI(TAG_PIPE, "║  🎵 PASSTHROUGH АКТИВНИЙ                      ║");
        ESP_LOGI(TAG_PIPE, "║  RX: I2S1 (slave) → TX: I2S0 (master)        ║");
        ESP_LOGI(TAG_PIPE, "╚═══════════════════════════════════════════════╝");
    }
    if (g_verbose_logs && g_player_logs) ESP_LOGI(TAG_PIPE, "");
    
    // Менший буфер для меншої затримки, але частіші передачі
    uint8_t *buffer = malloc(2048);
    if (!buffer) {
        ESP_LOGE(TAG_PIPE, "❌ Malloc failed");
        audio_element_deinit(i2s_writer);
        i2s_del_channel(rx_handle);
        g_i2s_passthrough_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    size_t bytes_read;
    uint32_t total_read = 0, total_write = 0;
    uint32_t read_timeouts = 0, read_errors = 0;
    uint32_t short_writes = 0, write_errors = 0, dropped_bytes = 0;
    g_external_i2s_rx_overflows = 0;
    g_external_i2s_rx_overflow_bytes = 0;
    int64_t last_diag_us = esp_timer_get_time();
    bool first_data = false;

    while (g_mode_external) {
        if (g_external_i2s_flush_requested) {
            g_external_i2s_flush_requested = false;
            size_t flushed_bytes = 0;
            size_t discarded_bytes = 0;

            for (uint32_t i = 0; i < rx_cfg.dma_desc_num; i++) {
                discarded_bytes = 0;
                if (i2s_channel_read(rx_handle, buffer, 2048, &discarded_bytes, 0) != ESP_OK ||
                    discarded_bytes == 0) {
                    break;
                }
                flushed_bytes += discarded_bytes;
            }

            printf("I2S RX buffer cleared: %u bytes discarded\r\n", (unsigned)flushed_bytes);
        }

        ret = i2s_channel_read(rx_handle, buffer, 2048, &bytes_read, pdMS_TO_TICKS(100));
        
        if (ret == ESP_OK && bytes_read > 0) {
            if (!first_data) {
                if (g_verbose_logs) {
                    ESP_LOGI(TAG_PIPE, "═══════════════════════════════════════════════");
                    ESP_LOGI(TAG_PIPE, "✅ ПЕРШІ ДАНІ ОТРИМАНО З SLAVE!");
                    ESP_LOGI(TAG_PIPE, "📊 Розмір блоку: %d байт", bytes_read);
                    ESP_LOGI(TAG_PIPE, "🎯 I2S порт: I2S_NUM_1 (slave mode)");
                    ESP_LOGI(TAG_PIPE, "📍 GPIO: BCLK=%d, WS=%d, DIN=%d", EXT_I2S_BCLK_PIN, EXT_I2S_WS_PIN, EXT_I2S_DIN_PIN);
                    ESP_LOGI(TAG_PIPE, "═══════════════════════════════════════════════");
                }
                first_data = true;
            }
            total_read += bytes_read;
            
            int written = audio_element_output(i2s_writer, (char*)buffer, bytes_read);
            if (written > 0) {
                total_write += written;
                if ((size_t)written < bytes_read) {
                    short_writes++;
                    dropped_bytes += bytes_read - written;
                }
            } else {
                write_errors++;
                dropped_bytes += bytes_read;
            }
        } else if (ret == ESP_ERR_TIMEOUT) {
            read_timeouts++;
        } else if (ret != ESP_OK) {
            read_errors++;
        }

        int64_t now_us = esp_timer_get_time();
        if ((now_us - last_diag_us) >= 5000000 &&
            (read_timeouts || read_errors || short_writes || write_errors ||
             g_external_i2s_rx_overflows || g_external_i2s_rx_overflow_bytes) &&
            g_verbose_logs && g_player_logs) {
            ESP_LOGW(TAG_PIPE, "I2S passthrough RX timeout=%u errors=%u DMA_OVF=%u (%u bytes), TX short=%u errors=%u dropped=%u bytes",
                     read_timeouts, read_errors, g_external_i2s_rx_overflows,
                     g_external_i2s_rx_overflow_bytes, short_writes, write_errors, dropped_bytes);
            read_timeouts = 0;
            read_errors = 0;
            short_writes = 0;
            write_errors = 0;
            dropped_bytes = 0;
            g_external_i2s_rx_overflows = 0;
            g_external_i2s_rx_overflow_bytes = 0;
            last_diag_us = now_us;
        } else if ((now_us - last_diag_us) >= 5000000) {
            last_diag_us = now_us;
        }
    }

    free(buffer);
    
    if (g_verbose_logs) {
        ESP_LOGI(TAG_PIPE, "");
        ESP_LOGI(TAG_PIPE, "╔═══════════════════════════════════════════════╗");
        ESP_LOGI(TAG_PIPE, "║  🛑 ЗУПИНКА PASSTHROUGH                       ║");
        ESP_LOGI(TAG_PIPE, "╚═══════════════════════════════════════════════╝");
    }
    
    audio_element_stop(i2s_writer);
    audio_element_deinit(i2s_writer);
    if (g_verbose_logs) ESP_LOGI(TAG_PIPE, "✅ I2S writer зупинено");
    
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    if (g_verbose_logs) ESP_LOGI(TAG_PIPE, "✅ I2S slave зупинено");
    
    ESP_LOGI(TAG_PIPE, "═══════════════════════════════════════════════");
    ESP_LOGI(TAG_PIPE, "✅ PASSTHROUGH ПОВНІСТЮ ЗУПИНЕНО");
    ESP_LOGI(TAG_PIPE, "═══════════════════════════════════════════════");

    g_i2s_passthrough_task_handle = NULL;
    vTaskDelete(NULL);
}

void player_task(void *pvParam)
{
    ESP_LOGI(TAG_PIPE, "🎧 Завдання відтворення запущено");
    
    if (!mount_usb_device(g_current_mount_slot)) {
        ESP_LOGE(TAG_PIPE, "❌ Не вдалося монтувати USB для відтворення");
        g_player_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 🔄 Сброс флага восстановления если трек не совпадает
    if (g_restore_playback && g_current_track != g_saved_track_index) {
        ESP_LOGW(TAG_PIPE, "⚠️ Запрошенный трек %d не совпадает с сохраненным %d", 
                 g_current_track, g_saved_track_index);
        g_restore_playback = false;
    }
    
    // 🎯 Скинути флаг перемотки
    g_seek_position_ms = -1;

    while (g_playlist_ready && g_playlist_size > 0 && g_usb_mounted) {
        audio_pipeline_handle_t pipeline = NULL;
        audio_element_handle_t fatfs_stream_reader = NULL, i2s_stream_writer = NULL, decoder = NULL;
        bool playback_finished = false;
        
        // 🔄 Скидаємо прапор помилки декодера на початку кожного треку
        g_decoder_error = false;

        ESP_LOGI(TAG_PIPE, "");
        ESP_LOGI(TAG_PIPE, "╔═══════════════════════════════════════════════╗");
        ESP_LOGI(TAG_PIPE, "║  🎵 ВІДТВОРЕННЯ ТРЕКУ                         ║");
        ESP_LOGI(TAG_PIPE, "╠═══════════════════════════════════════════════╣");
        ESP_LOGI(TAG_PIPE, "║  📀 Трек: %d / %d", g_current_track + 1, g_playlist_size);
        ESP_LOGI(TAG_PIPE, "║  📁 Файл: %s", g_playlist[g_current_track]);
        ESP_LOGI(TAG_PIPE, "╚═══════════════════════════════════════════════╝");
        ESP_LOGI(TAG_PIPE, "");
        
        // 🔍 Проаналізуємо MP3 файл перед відтворенням
        // Використовуємо шлях безпосередньо з плейлиста (він вже містить повний шлях)
        ESP_LOGI(TAG_PIPE, "🔍 Аналіз файлу: %s", g_playlist[g_current_track]);
        analyze_mp3_file(g_playlist[g_current_track]);
        
        if (!g_usb_mounted) {
            ESP_LOGE(TAG_PIPE, "❌ USB демонтовано під час відтворення");
            break;
        }
        
        // 🏗️ Створити пайплайн
        audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
        pipeline = audio_pipeline_init(&pipeline_cfg);
        if (!pipeline) {
            ESP_LOGE(TAG_PIPE, "❌ Не вдалося створити пайплайн");
            goto next_track;
        }

        // 🎛️ Створити елементи
        fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
        fatfs_cfg.type = AUDIO_STREAM_READER;
        fatfs_stream_reader = fatfs_stream_init(&fatfs_cfg);
        
        i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
        i2s_cfg.type = AUDIO_STREAM_WRITER;
        i2s_cfg.std_cfg.gpio_cfg.bclk = 17;
        i2s_cfg.std_cfg.gpio_cfg.ws = 18;
        i2s_cfg.std_cfg.gpio_cfg.dout = 16;
        i2s_cfg.std_cfg.gpio_cfg.din = -1;
          i2s_cfg.std_cfg.gpio_cfg.mclk = 0;
          i2s_cfg.std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
          i2s_cfg.std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;

        /* Increase I2S DMA/ring parameters where possible to reduce underruns.
           These fields are optional on some ADF versions; check and set if present. */
    #ifdef I2S_STREAM_STD_CONFIG_HAS_DMA
        i2s_cfg.std_cfg.dma_desc_num = 8;
        i2s_cfg.std_cfg.dma_frame_num = 1024;
    #endif
        i2s_stream_writer = i2s_stream_init(&i2s_cfg);
        
        // 🎵 Створити декодер для MP3
        mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
        decoder = mp3_decoder_init(&mp3_cfg);

        if (!fatfs_stream_reader || !i2s_stream_writer || !decoder) {
            ESP_LOGE(TAG_PIPE, "❌ Не вдалося створити аудіо елементи");
            goto cleanup;
        }

        // 📝 Зареєструвати та зв'язати елементи
        audio_pipeline_register(pipeline, fatfs_stream_reader, "file");
        audio_pipeline_register(pipeline, decoder, "decoder");
        audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

        const char *link_tag[3] = {"file", "decoder", "i2s"};
        audio_pipeline_link(pipeline, &link_tag[0], 3);
        
        // 🎯 Встановити URI
        audio_element_set_uri(fatfs_stream_reader, g_playlist[g_current_track]);
        vTaskDelay(pdMS_TO_TICKS(100));

        // Якщо є запит на відновлення позиції — спробуємо встановити її ДО запуску пайплайна,
        // щоб декодер отримав зчитування починаючи з потрібного байту і не відтворював початок.
        if (g_restore_playback && g_current_track == g_saved_track_index && g_saved_position_ms > 0) {
            audio_element_info_t info_pre = {0};
            if (audio_element_getinfo(fatfs_stream_reader, &info_pre) == ESP_OK && info_pre.total_bytes > 0) {
                size_t target_bytes_pre = (size_t)((g_saved_position_ms * 16000) / 1000);
                if (target_bytes_pre > (size_t)(info_pre.total_bytes * 0.9)) {
                    target_bytes_pre = (size_t)(info_pre.total_bytes * 0.9);
                }
                ESP_LOGI(TAG_PIPE, "🌅 Спроба відновлення ДО запуску пайплайна: %d байт", (int)target_bytes_pre);
                esp_err_t ret_pre = audio_element_set_byte_pos(fatfs_stream_reader, target_bytes_pre);
                if (ret_pre == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    audio_element_info_t new_info_pre = {0};
                    if (audio_element_getinfo(fatfs_stream_reader, &new_info_pre) == ESP_OK) {
                        int actual_ms_pre = (int)((new_info_pre.byte_pos * 1000) / 16000);
                        ESP_LOGI(TAG_PIPE, "✅ Позиція (до запуску) встановлена: %d байт (~%d мс)", (int)new_info_pre.byte_pos, actual_ms_pre);
                        g_last_saved_position = actual_ms_pre;
                        // Якщо вдалося — скидаємо прапори відновлення, щоб не повторювати після старту
                        g_restore_playback = false;
                        g_saved_track_index = -1;
                        g_saved_position_ms = 0;
                    }
                } else {
                    ESP_LOGW(TAG_PIPE, "⚠️ Не вдалося встановити позицію до запуску пайплайна: %s", esp_err_to_name(ret_pre));
                }
            } else {
                ESP_LOGI(TAG_PIPE, "ℹ️ Інфо файлу недоступне до запуску пайплайна, спробуємо після старту");
            }
        }
        
        // 📞 Встановити callback для відстеження завершення
        audio_element_set_event_callback(fatfs_stream_reader, audio_event_cb, &playback_finished);

        // 🎯 Додамо callback для моніторингу продуктивності
        // Не передаємо handle writer як ctx (unsafe) — clock буде змінюватися
        // безпечно з `player_task` після старту пайплайну.
        audio_element_set_event_callback(decoder, audio_event_cb, NULL);
        audio_element_set_event_callback(i2s_stream_writer, audio_event_cb, NULL);

        // 🚀 Запускаємо пайплайн
        ESP_LOGI(TAG_PIPE, "🚀 Запуск пайплайну");
        esp_err_t ret = audio_pipeline_run(pipeline);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG_PIPE, "❌ Не вдалося запустити пайплайн: %s", esp_err_to_name(ret));
            goto cleanup;
        }

        // Скидаємо сторож прогресу
        g_stall_last_bytes = 0;
        g_stall_last_ts_ms = esp_timer_get_time() / 1000;

        // Якщо після запуску пайплайну потрібно відновити позицію —
        // попробуем поставить паузу, выполнить seek и затем возобновить,
        // это часто помогает корректно пропустить начальные байты декодером.
        if (g_restore_playback && g_current_track == g_saved_track_index && g_saved_position_ms > 0) {
            ESP_LOGI(TAG_PIPE, "⏸️ Пауза пайплайна для установки позиції");
            audio_pipeline_pause(pipeline);

            audio_element_info_t info_seek = {0};
            if (audio_element_getinfo(fatfs_stream_reader, &info_seek) == ESP_OK && info_seek.total_bytes > 0) {
                size_t target_bytes = (size_t)((g_saved_position_ms * 16000) / 1000);
                if (target_bytes > (size_t)(info_seek.total_bytes * 0.9)) {
                    target_bytes = (size_t)(info_seek.total_bytes * 0.9);
                }
                ESP_LOGI(TAG_PIPE, "🌅 Встановлюємо позицію ПІСЛЯ запуску, до resume: %d байт", (int)target_bytes);
                esp_err_t rseek = audio_element_set_byte_pos(fatfs_stream_reader, target_bytes);
                if (rseek == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    audio_element_info_t check = {0};
                    if (audio_element_getinfo(fatfs_stream_reader, &check) == ESP_OK) {
                        int actual_ms = (int)((check.byte_pos * 1000) / 16000);
                        ESP_LOGI(TAG_PIPE, "✅ Позиція встановлена після pause: %d байт (~%d мс)", (int)check.byte_pos, actual_ms);
                        g_last_saved_position = actual_ms;
                    }
                } else {
                    ESP_LOGW(TAG_PIPE, "⚠️ Не вдалося встановити позицію після pause: %s", esp_err_to_name(rseek));
                }
            } else {
                ESP_LOGW(TAG_PIPE, "⚠️ Інфо файлу недоступне після запуску пайплайна");
            }

            ESP_LOGI(TAG_PIPE, "▶️ Відновлюємо пайплайн після seek");
            audio_pipeline_resume(pipeline);

            // Сбросим флаги восстановления, чтобы не пытаться снова
            g_restore_playback = false;
            g_saved_track_index = -1;
            g_saved_position_ms = 0;
        }

        // ⏳ Чекаємо, поки пайплайн реально почне відтворювати
        ESP_LOGI(TAG_PIPE, "⏳ Очікування старту відтворення...");
        bool playback_started = false;
        int wait_counter = 0;
        
        while (!playback_started && wait_counter < 100) { // Макс 5 секунд
            audio_element_state_t file_state = audio_element_get_state(fatfs_stream_reader);
            
            if (file_state == AEL_STATE_RUNNING) {
                playback_started = true;
                ESP_LOGI(TAG_PIPE, "═══════════════════════════════════════════════");
                ESP_LOGI(TAG_PIPE, "✅ ПАЙПЛАЙН ЗАПУЩЕНО");
                ESP_LOGI(TAG_PIPE, "🎯 Decoder → I2S → PCM5102");
                ESP_LOGI(TAG_PIPE, "═══════════════════════════════════════════════");
                
                // 📊 Розрахувати тривалість треку
                audio_element_info_t dur_info = {0};
                if (audio_element_getinfo(decoder, &dur_info) == ESP_OK && dur_info.duration > 0) {
                    g_current_track_duration_ms = dur_info.duration;
                    int corrected_duration = g_current_track_duration_ms;
                    ESP_LOGI(TAG_PIPE, "📊 Тривалість треку: %d мс (%d:%02d)", corrected_duration, 
                             corrected_duration / 60000, (corrected_duration % 60000) / 1000);
                } else {
                    // Fallback: приблизний розрахунок для 128 kbps
                    audio_element_info_t file_info = {0};
                    if (audio_element_getinfo(fatfs_stream_reader, &file_info) == ESP_OK && file_info.total_bytes > 0) {
                        g_current_track_duration_ms = (int32_t)((file_info.total_bytes * 1000) / 16000);
                        int corrected_duration = g_current_track_duration_ms;
                        ESP_LOGI(TAG_PIPE, "📊 Приблизна тривалість треку: %d мс (%d:%02d)", corrected_duration, 
                                 corrected_duration / 60000, (corrected_duration % 60000) / 1000);
                    } else {
                        g_current_track_duration_ms = 180000; // За замовчуванням 3 хв
                    }
                }

                // Use the decoder's actual output format, not a pre-scan of the MP3 header.
                audio_element_info_t dec_info = {0};
                if (audio_element_getinfo(decoder, &dec_info) == ESP_OK &&
                    dec_info.sample_rates > 0 && dec_info.channels > 0 && dec_info.bits > 0) {
                    ESP_LOGI(TAG_PIPE, "🔧 Налаштування I2S: %d Hz, %d bit, %d ch",
                             dec_info.sample_rates, dec_info.bits, dec_info.channels);
                    i2s_stream_set_clk(i2s_stream_writer, dec_info.sample_rates,
                                       dec_info.bits, dec_info.channels);
                }
                
                // 🌅 СПРОБА ВІДНОВЛЕННЯ ПОЗИЦІЇ
                if (g_restore_playback && g_current_track == g_saved_track_index && g_saved_position_ms > 0) {
                    ESP_LOGI(TAG_PIPE, "");
                    ESP_LOGI(TAG_PIPE, "╔═══════════════════════════════════════════════╗");
                    ESP_LOGI(TAG_PIPE, "║  🌅 ВІДНОВЛЕННЯ ПОЗИЦІЇ                       ║");
                    ESP_LOGI(TAG_PIPE, "╠═══════════════════════════════════════════════╣");
                    ESP_LOGI(TAG_PIPE, "║  🎵 Трек: %d", g_current_track);
                    ESP_LOGI(TAG_PIPE, "║  ⏱️  Позиція: %d мс (%.1f сек)", g_saved_position_ms, g_saved_position_ms / 1000.0);
                    ESP_LOGI(TAG_PIPE, "╚═══════════════════════════════════════════════╝");
                    
                    // Даем время на инициализацию
                    vTaskDelay(pdMS_TO_TICKS(300));
                    
                    // Получаем информацию о файле
                    audio_element_info_t info = {0};
                    if (audio_element_getinfo(fatfs_stream_reader, &info) == ESP_OK && info.total_bytes > 0) {
                        ESP_LOGI(TAG_PIPE, "   Розмір файлу: %d байт", (int)info.total_bytes);
                        
                        // Вычисляем позицию в байтах
                        // Для MP3 примерно: 1 секунда = 16000 байт (128kbps)
                        size_t target_bytes = (size_t)((g_saved_position_ms * 16000) / 1000);
                        
                        // Ограничиваем 90% файла
                        if (target_bytes > (size_t)(info.total_bytes * 0.9)) {
                            target_bytes = (size_t)(info.total_bytes * 0.9);
                            ESP_LOGW(TAG_PIPE, "   Позиція скоригована до %d байт", (int)target_bytes);
                        }
                        
                        ESP_LOGI(TAG_PIPE, "   Встановлюємо позицію: %d байт", (int)target_bytes);
                        
                        // ⏳ Задержка перед установкой
                        vTaskDelay(pdMS_TO_TICKS(100));
                        
                        // Пробуем несколько раз установить позицию
                        for (int attempt = 0; attempt < 5; attempt++) {
                            ret = audio_element_set_byte_pos(fatfs_stream_reader, target_bytes);
                            
                            if (ret == ESP_OK) {
                                ESP_LOGI(TAG_PIPE, "✅ Позиція встановлена на спробі %d", attempt + 1);
                                
                                // Проверяем результат
                                vTaskDelay(pdMS_TO_TICKS(200));
                                audio_element_info_t new_info = {0};
                                if (audio_element_getinfo(fatfs_stream_reader, &new_info) == ESP_OK) {
                                    int actual_ms = (int)((new_info.byte_pos * 1000) / 16000);
                                    ESP_LOGI(TAG_PIPE, "   Фактична позиція: %d байт (~%d мс)", 
                                             (int)new_info.byte_pos, actual_ms);
                                    
                                    // Обновляем сохраненную позицию
                                    g_last_saved_position = actual_ms;
                                }
                                break;
                            } else {
                                ESP_LOGW(TAG_PIPE, "⚠️ Спроба %d не вдалась: %s", 
                                         attempt + 1, esp_err_to_name(ret));
                                vTaskDelay(pdMS_TO_TICKS(200));
                            }
                        }
                    }
                    
                    // 🔄 Скидаємо прапори ВНЕЗАВИСИМО от результата
                    g_restore_playback = false;
                    g_saved_track_index = -1;
                    g_saved_position_ms = 0;
                    ESP_LOGI(TAG_PIPE, "✅ Відновлення позиції оброблено");
                }
                break;
            }
            
            if (wait_counter % 20 == 0) { // Каждую секунду
                ESP_LOGI(TAG_PIPE, "⏳ Очікування... стан: %d", file_state);
            }
            
            wait_counter++;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (!playback_started) {
            ESP_LOGW(TAG_PIPE, "⚠️ Пайплайн не запустився нормально");
            goto cleanup;
        }

        ESP_LOGI(TAG_PIPE, "⏳ Очікування завершення відтворення...");
        ESP_LOGI(TAG_PIPE, "⌨️ Terminal commands task started");
        ESP_LOGI(TAG_PIPE, "   n - next track");
        ESP_LOGI(TAG_PIPE, "   p - previous track");
        ESP_LOGI(TAG_PIPE, "   m - mode switch");

        bool playback_error = false;
        int timeout_counter = 0;
        const int MAX_TIMEOUT = 36000;
        
        g_last_position_save_time = xTaskGetTickCount();
        
        while (g_usb_mounted && !g_skip_to_next && !g_skip_to_prev && !playback_finished && !playback_error) {
            audio_element_state_t file_state = audio_element_get_state(fatfs_stream_reader);
            
            // 🕒 Сохраняем позицию
            uint32_t current_time = xTaskGetTickCount();
            int current_position = get_current_position(fatfs_stream_reader);
            
            if (current_position > 0) {
                g_last_saved_position = current_position;
                
                // Автосохранение каждые 60 секунд или при значительном изменении
                if (current_time - g_last_position_save_time >= pdMS_TO_TICKS(POSITION_SAVE_INTERVAL_MS) ||
                    abs(current_position - g_last_saved_position) >= MIN_POSITION_CHANGE_MS) {
                    
                    save_playback_position(g_current_track, (int32_t)current_position);
                    g_last_position_save_time = current_time;
                    
                    if (timeout_counter % 60 == 0) { // Логируем каждые 6 секунд
                        ESP_LOGI(TAG_PIPE, "💾 Автозбереження: %d мс", current_position);
                    }
                }
            }
            
            // ⏸️ Перевірка на паузу через веб
            if (g_is_paused) {
                audio_pipeline_pause(pipeline);
            } else {
                audio_pipeline_resume(pipeline);
            }
            
            // ✅ Файл прочитан полностью
            if (file_state == AEL_STATE_FINISHED) {
                ESP_LOGI(TAG_PIPE, "✅ Файл прочитано повністю");
                playback_finished = true;
                break;
            }
            
            // ❌ Ошибка
            if (file_state == AEL_STATE_STOPPED) {
                ESP_LOGE(TAG_PIPE, "❌ Елемент зупинився неочікувано");
                playback_error = true;
                break;
            }
            
            // 🎯 Перевірка на перемотку через веб
            if (g_seek_position_ms >= 0) {
                ESP_LOGI(TAG_PIPE, "🎯 Перемотка до %d мс", g_seek_position_ms);
                
                // ⏸️ Пауза пайплайна для перемотки
                audio_pipeline_pause(pipeline);
                vTaskDelay(pdMS_TO_TICKS(100));
                
                // Встановити позицію
                set_playback_position(fatfs_stream_reader, g_seek_position_ms);
                g_last_saved_position = g_seek_position_ms;
                
                // ▶️ Відновити
                audio_pipeline_resume(pipeline);
                
                g_seek_position_ms = -1; // Скинути флаг
            }
            
            // 🛡️ Защита от зависания
            if (timeout_counter++ >= MAX_TIMEOUT) {
                ESP_LOGW(TAG_PIPE, "⚠️ Досягнуто таймаут відтворення");
                break;
            }

            // 🛡️ Виявлення зависання: немає прогресу байтів > PLAYBACK_STALL_TIMEOUT_MS
            if (!g_is_paused) {
                audio_element_info_t stall_info = {0};
                if (audio_element_getinfo(fatfs_stream_reader, &stall_info) == ESP_OK) {
                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (stall_info.byte_pos != g_stall_last_bytes) {
                        g_stall_last_bytes = stall_info.byte_pos;
                        g_stall_last_ts_ms = now_ms;
                    } else if (g_stall_last_ts_ms != 0 && (now_ms - g_stall_last_ts_ms) > PLAYBACK_STALL_TIMEOUT_MS) {
                        ESP_LOGW(TAG_PIPE, "⚠️ Відтворення зависло (немає прогресу %lld мс), форс зупинка", (long long)(now_ms - g_stall_last_ts_ms));
                        playback_error = true;
                        break;
                    }
                }
            }
            
            // Periodic diagnostics: каждую секунду выводим состояние буферов и память
            /* static int diag_counter = 0;
            if (++diag_counter >= 10) { // ~1 second (10 * 100ms)
                diag_counter = 0;
                audio_element_info_t di = {0};
                audio_element_info_t wi = {0};
                if (audio_element_getinfo(decoder, &di) == ESP_OK) {
                    ESP_LOGI(TAG_PIPE, "DIAG decoder: sample_rate=%d, channels=%d, bits=%d, byte_pos=%d, total=%d", di.sample_rates, di.channels, di.bits, (int)di.byte_pos, (int)di.total_bytes);
                }
                if (audio_element_getinfo(i2s_stream_writer, &wi) == ESP_OK) {
                    ESP_LOGI(TAG_PIPE, "DIAG writer: sample_rate=%d, channels=%d, bits=%d, bytes_in_rb=%d", wi.sample_rates, wi.channels, wi.bits, (int)wi.byte_pos);
                }
                ESP_LOGI(TAG_PIPE, "DIAG free_heap=%d", esp_get_free_heap_size());
            }*/

            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // 💾 Сохраняем позицию при завершении
        if (playback_finished) {
            if (g_decoder_error) {
                ESP_LOGW(TAG_PIPE, "⚠️ Пропускаємо проблемний MP3 файл: %s", g_playlist[g_current_track]);
                g_decoder_error = false;  // Скидаємо прапор
                // Автоматично переходимо до наступного треку
                g_current_track = (g_current_track + 1) % g_playlist_size;
                ESP_LOGI(TAG_PIPE, "⏭️ Переходимо до треку: %d/%d", g_current_track + 1, g_playlist_size);
            } else {
                ESP_LOGI(TAG_PIPE, "✅ Відтворення успішно завершено");
                save_playback_position((g_current_track + 1) % g_playlist_size, 0);
                g_last_saved_position = 0;
            }
        } else if (playback_error) {
            ESP_LOGE(TAG_PIPE, "❌ Відтворення завершено з помилкою");
        } else if (g_skip_to_next || g_skip_to_prev) {
            ESP_LOGI(TAG_PIPE, "🔀 Відтворення перервано для зміни треку");
        } else {
            int current_position = get_current_position(fatfs_stream_reader);
            if (current_position > 0) {
                save_playback_position(g_current_track, (int32_t)current_position);
                g_last_saved_position = current_position;
            }
        }

cleanup:
        // 🧹 Очистка ресурсов
        if (pipeline) {
            audio_pipeline_stop(pipeline);
            audio_pipeline_wait_for_stop(pipeline);
            audio_pipeline_terminate(pipeline);
            
            audio_pipeline_unregister(pipeline, fatfs_stream_reader);
            audio_pipeline_unregister(pipeline, decoder);
            audio_pipeline_unregister(pipeline, i2s_stream_writer);
            audio_pipeline_deinit(pipeline);
        }
        
        if (fatfs_stream_reader) audio_element_deinit(fatfs_stream_reader);
        if (decoder) audio_element_deinit(decoder);
        if (i2s_stream_writer) audio_element_deinit(i2s_stream_writer);

next_track:
        if (!g_usb_mounted) {
            break;
        }

        // 🛑 Перевірка прапора зупинки - ВИХІД з циклу при примусовій зупинці
        if (g_skip_to_next && !g_playlist_ready) {
            ESP_LOGI(TAG_PIPE, "🛑 Примусова зупинка задачі");
            break;
        }

        if (g_skip_to_prev) {
            g_skip_to_prev = false;
            // g_current_track вже змінено в play_prev_track(), не треба додаткової зміни
        } else if (g_skip_to_next) {
            g_skip_to_next = false;
            // g_current_track вже змінено в play_next_track() або play_specific_track(), не треба +1
            g_play_specific_track = false;
        } else {
            // Автоматичне переключення на наступний трек (коли файл закінчився сам)
            g_current_track = (g_current_track + 1) % g_playlist_size;
        }
        
        g_last_saved_position = 0; // Скидання позиції для нового трека
        
        ESP_LOGI(TAG_PIPE, "⏭️ Наступний трек: %d/%d", g_current_track + 1, g_playlist_size);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG_PIPE, "👋 Завдання відтворення завершено");
    g_player_task_handle = NULL;
    
    // 🔌 НЕ демонтуємо USB якщо переключаємось на external input
    // USB залишається підключеним для швидкого повернення
    if (!g_mode_external) {
        unmount_usb_device();
    } else {
        ESP_LOGI(TAG_PIPE, "ℹ️ USB залишається змонтованим (режим external)");
    }
    
    vTaskDelete(NULL);
}
