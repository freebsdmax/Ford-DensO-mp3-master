#include "player_can.h"
#include "player_sleep.h"
#include "player_pipeline.h"
#include "player_playlist.h"
#include "player_console.h"
#include "player_link.h"
#include "player_usb.h"
#include "player_nvs.h"
#include "app_messages.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* "Can" tag: CAN frames must be filtered by the "Car" log group
   (CAN + ELM327), not by the player group. */
static const char *TAG_CAN = "Can";

extern bool g_verbose_logs;
extern bool g_player_logs;
void player_control(const char *cmd, int track);
void enter_deep_sleep(void);
void stop_playback(void);
void play_next_track(void);
void play_prev_track(void);
void set_volume(int volume_percent);
void player_set_position(int position_ms);
void wakeup_from_sleep(void);



void send_can_event(uint8_t event_type, uint8_t data1, uint8_t data2)
{
    twai_message_t message;
    message.identifier = CAN_ID_MAIN;
    message.flags = TWAI_MSG_FLAG_NONE;
    message.data_length_code = 4;
    
    // 📝 Формат: [0x03, тип події, дані1, дані2]
    message.data[0] = 0x03;          // 📡 Індикатор події
    message.data[1] = event_type;    // 🎛️ Тип події
    message.data[2] = data1;         // 📊 Дані 1
    message.data[3] = data2;         // 📊 Дані 2
    
    esp_err_t ret = twai_transmit(&message, pdMS_TO_TICKS(100));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG_CAN, "📨 CAN TX ✅: ID=0x%03X, [0x%02X, 0x%02X, 0x%02X, 0x%02X] (подія: 0x%02X)", 
                 message.identifier, message.data[0], message.data[1], message.data[2], 
                 message.data[3], event_type);
    } else {
        ESP_LOGE(TAG_CAN, "❌ CAN TX ПОМИЛКА: не вдалося передати (0x%02X): %s", event_type, esp_err_to_name(ret));
    }
}

void send_can_status(void)
{
    twai_message_t message;
    message.identifier = CAN_ID_MAIN;
    message.flags = TWAI_MSG_FLAG_NONE;
    message.data_length_code = 4;
    
    // 📝 Формат даних: [0x02, статус, трек, всього]
    message.data[0] = 0x02;                     // 📡 Індикатор статусу
    
    // 📊 Статус: біт 0 - відтворення (1=грає, 0=пауза)
    uint8_t status = 0;
    if (!g_is_paused) status |= 0x01;           // 🎵 Біт 0: відтворення
    
    message.data[1] = status;                   // 📊 Статус
    message.data[2] = g_current_track + 1;      // 🎵 Поточний трек (1-based)
    message.data[3] = g_playlist_size;          // 📋 Всього треків
    /**/
    esp_err_t ret = twai_transmit(&message, pdMS_TO_TICKS(100));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG_CAN, "📨 CAN: Відправлено статус - трек %d/%d, статус: 0x%02X", 
                 g_current_track + 1, g_playlist_size, status);
    } else {
        ESP_LOGW(TAG_CAN, "⚠️ CAN: Не вдалося відправити статус: %s", esp_err_to_name(ret));
    }
}

esp_err_t change_can_bitrate(uint16_t new_bitrate_kbps)
{
    // Проверяем поддерживаемые скорости
    if (new_bitrate_kbps != 100 && new_bitrate_kbps != 125 && 
        new_bitrate_kbps != 250 && new_bitrate_kbps != 500 && new_bitrate_kbps != 1000) {
        ESP_LOGE(TAG_CAN, "❌ Неподдерживаемая скорость CAN: %d kbps", new_bitrate_kbps);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Если скорость уже такая же, ничего не делаем
    if (g_can_bitrate == new_bitrate_kbps) {
        ESP_LOGI(TAG_CAN, "⚠️ CAN уже работает на %d kbps", new_bitrate_kbps);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG_CAN, "🔄 Смена скорости CAN: %d -> %d kbps", g_can_bitrate, new_bitrate_kbps);
    
    // Останавливаем CAN
    esp_err_t ret = twai_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CAN, "❌ Ошибка при остановке CAN: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Деинициализируем драйвер
    ret = twai_driver_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CAN, "❌ Ошибка при деинициализации CAN: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Обновляем глобальную переменную
    g_can_bitrate = new_bitrate_kbps;
    
    // Переинициализируем CAN с новой скоростью
    ret = init_can();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CAN, "❌ Ошибка при переинициализации CAN: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG_CAN, "✅ Скорость CAN успешно изменена на %d kbps", new_bitrate_kbps);
    return ESP_OK;
}

void wifi_can_on_message_callback(const can_sniffer_frame_t *frame)
{
    if (!frame) return;

    ESP_LOGI(TAG_CAN, "📨 Wi-Fi CAN RX: ID=0x%03X, DLC=%d", frame->id, frame->length);

    // Парсим как обычный CAN-пакет
    twai_message_t msg = {
        .identifier = frame->id,
        .data_length_code = frame->length,
        .rtr = 0,
        .flags = 0,
    };

    memcpy(msg.data, frame->data, frame->length);
    
    // Обрабатываем через процесс_can_message
    process_can_message(&msg);
    
    // Обновляем статистику
    g_can_frames_per_second++;
    g_can_bytes_per_second += (4 + frame->length);
    g_can_total_frames++;
    g_can_total_bytes += 4 + frame->length;
}

void wifi_can_on_control_callback(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    ESP_LOGI(TAG_CAN, "🎛️ Wi-Fi CAN Control Command: 0x%02X, len=%d", cmd, len);

    switch (cmd) {
        case CAN_SNIFFER_CMD_DISCONNECT:
            ESP_LOGI(TAG_CAN, "🔌 Disconnect command received");
            break;

        case CAN_SNIFFER_CMD_CONNECT:
            if (len >= 2) {
                uint16_t bitrate = *(uint16_t *)data;
                ESP_LOGI(TAG_CAN, "🔌 Connect command: bitrate=%d kbps", bitrate);
                
                // 🔄 Попытка изменить скорость CAN
                esp_err_t ret = change_can_bitrate(bitrate);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG_CAN, "✅ CAN bitrate успешно изменена на %d kbps через Wi-Fi", bitrate);
                } else {
                    ESP_LOGE(TAG_CAN, "❌ Ошибка изменения CAN bitrate: %s", esp_err_to_name(ret));
                }
            }
            break;

        default:
            ESP_LOGW(TAG_CAN, "❓ Unknown control command: 0x%02X", cmd);
            break;
    }
}


void process_can_message(twai_message_t *message)
{
    // ⏰ ОНОВЛЮЄМО час останньої активності
    g_last_can_activity_time = xTaskGetTickCount();
    
    // 😴 Якщо ми в режимі сну - прокидаємося
    if (g_can_sleep_mode) {
        ESP_LOGI(TAG_CAN, "🌅 CAN: Пробудження від сну по CAN активності");
        g_can_sleep_mode = false;
        g_sleep_start_time = 0;
        
        // ⏰ Чекаємо 5 секунд перед поновленням відтворення
        ESP_LOGI(TAG_CAN, "⏳ CAN: Очікування %d мс перед поновленням", CAN_WAKEUP_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(CAN_WAKEUP_DELAY_MS));
    }
    
    ESP_LOGI(TAG_CAN, "📨 CAN: Отримано повідомлення ID: 0x%03X, DLC: %d", 
             message->identifier, message->data_length_code);
    
    app_message_t app_msg;
    
    // 🔍 Перевіряємо основний ID і перший байт даних
    if (message->identifier == CAN_ID_MAIN && 
        message->data_length_code >= 2 && 
        (message->data[0] & 0xF0) == 0x00) {  // data[0] в диапазоне 0x0x (0x00-0x0F)
        
        uint8_t cmd = message->data[1];
        uint8_t cmd_masked = cmd & 0xF0;  // Маскуємо молодші біти (останній символ)
        
        switch (cmd_masked) {
            case 0x40:  // NEXT_TRACK (0x4x, де x може бути будь-чим)
                {
                    // Исключаем CMD_NEXT_TRACK 0x49
                    if (cmd == CMD_NEXT_TRACK) {
                        ESP_LOGI(TAG_CAN, "⏭️ CAN: Команда 0x49 (CMD_NEXT_TRACK) ігнорується");
                        break;
                    }
                    
                    uint32_t current_time = pdTICKS_TO_MS(xTaskGetTickCount());
                    uint32_t time_since_last = current_time - g_last_next_command_time;
                    
                    // Перевіряємо debounce - ігноруємо якщо команда прийшла занадто швидко
                    if (time_since_last < CAN_COMMAND_DEBOUNCE_MS) {
                        ESP_LOGW(TAG_CAN, "⏭️ CAN: NEXT команда ігнорується (debounce: %lu мс < %d мс)", 
                                time_since_last, CAN_COMMAND_DEBOUNCE_MS);
                        break;
                    }
                    
                    g_last_next_command_time = current_time;
                    ESP_LOGI(TAG_CAN, "⏭️ CAN: Команда НАСТУПНИЙ ТРЕК (0x%02X)", cmd);
                    
                    // Если в режиме external input - отправляем по UART на внешнее устройство
                    if (g_mode_external) {
                        ESP_LOGI(TAG_CAN, "🔀 Режим external - отправка по UART");
                        if (send_i2c_command(LINK_CMD_NEXT_TRACK) != ESP_OK) {
                            ESP_LOGW(TAG_CAN, "⚠️ UART: не вдалося відправити команду NEXT_TRACK");
                        }
                    } else {
                        // Иначе - управляем локальным плеером
                        app_msg.id = APP_NEXT_TRACK;
                        xQueueSend(app_queue, &app_msg, portMAX_DELAY);
                    }
                }
                break;
                
            case 0x80:  // PREV_TRACK (0x8x, де x може бути будь-чим)
                {
                    // Исключаем CMD_PREV_TRACK 0x89
                    if (cmd == CMD_PREV_TRACK) {
                        ESP_LOGI(TAG_CAN, "⏮️ CAN: Команда 0x89 (CMD_PREV_TRACK) ігнорується");
                        break;
                    }
                    
                    uint32_t current_time = pdTICKS_TO_MS(xTaskGetTickCount());
                    uint32_t time_since_last = current_time - g_last_prev_command_time;
                    
                    // Перевіряємо debounce - ігноруємо якщо команда прийшла занадто швидко
                    if (time_since_last < CAN_COMMAND_DEBOUNCE_MS) {
                        ESP_LOGW(TAG_CAN, "⏮️ CAN: PREV команда ігнорується (debounce: %lu мс < %d мс)", 
                                time_since_last, CAN_COMMAND_DEBOUNCE_MS);
                        break;
                    }
                    
                    g_last_prev_command_time = current_time;
                    ESP_LOGI(TAG_CAN, "⏮️ CAN: Команда ПОПЕРЕДНІЙ ТРЕК (0x%02X)", cmd);
                    
                    // Если в режиме external input - отправляем по UART на внешнее устройство
                    if (g_mode_external) {
                        ESP_LOGI(TAG_CAN, "🔀 Режим external - отправка по UART");
                        if (send_i2c_command(LINK_CMD_PREV_TRACK) != ESP_OK) {
                            ESP_LOGW(TAG_CAN, "⚠️ UART: не вдалося відправити команду PREV_TRACK");
                        }
                    } else {
                        // Иначе - управляем локальным плеером
                        app_msg.id = APP_PREV_TRACK;
                        xQueueSend(app_queue, &app_msg, portMAX_DELAY);
                    }
                }
                break;
                
            case 0x10:  // MODE_SWITCH (0x1x, де x може бути будь-чим)
                // Исключаем CMD_MODE_SWITCH 0x19
                if (cmd == CMD_MODE_SWITCH) {
                    ESP_LOGI(TAG_CAN, "🔄 CAN: Команда 0x19 (CMD_MODE_SWITCH) ігнорується");
                    break;
                }
                
                ESP_LOGI(TAG_CAN, "🔄 CAN: Команда ПЕРЕМИКАННЯ РЕЖИМУ (0x%02X)", cmd);
                app_msg.id = APP_MODE_SWITCH;
                xQueueSend(app_queue, &app_msg, portMAX_DELAY);
                break;
                
            default:
                //ESP_LOGW(TAG_CAN, "❓ CAN: Невідома команда: 0x%02X", cmd);
                break;
        }
    } else {
       // ESP_LOGW(TAG_CAN, "⚠️ CAN: Некоректне повідомлення або невірний формат");
    }
}

void can_task(void *arg)
{
    ESP_LOGI(TAG_CAN, "📡 Завдання CAN запущено");
    
    while (1) {
        twai_message_t message;
        esp_err_t ret = twai_receive(&message, pdMS_TO_TICKS(100));
        
        if (ret == ESP_OK) {
            // 📨 БУДЬ-ЯКЕ CAN повідомлення оновлює час активності
            g_last_can_activity_time = xTaskGetTickCount();
            update_system_activity();  // 🌙 Оновлюємо системну активність для сну
            
            // 📊 Оновлюємо статистику
            g_can_frames_per_second++;
            g_can_bytes_per_second += (4 + message.data_length_code);  // ID(2) + FLAGS(1) + DLC(1) + DATA
            g_can_total_frames++;
            g_can_total_bytes += 4 + message.data_length_code;
            
            // 📡 ВІДПРАВЛЯЄМО CAN-дані через Wi-Fi на ПК
            esp_err_t wifi_ret = wifi_can_sniffer_send(
                message.identifier, 
                message.data_length_code, 
                message.data
            );
            
            if (wifi_ret == ESP_OK) {
                ESP_LOGI(TAG_CAN, "📤 CAN→Wi-Fi: ID=0x%03X, DLC=%d", 
                         message.identifier, message.data_length_code);
            } else {
                ESP_LOGW(TAG_CAN, "⚠️ Не вдалося відправити CAN через Wi-Fi: %s", 
                         esp_err_to_name(wifi_ret));
            }
            
            // 🎯 Обробляємо тільки якщо не в режимі сну або якщо це команда пробудження
            if (!g_can_sleep_mode || 
                (message.identifier == CAN_ID_MAIN && message.data[0] == 0x01)) {
                process_can_message(&message);
            }
        } else if (ret != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG_CAN, "❌ CAN помилка отримання: %s", esp_err_to_name(ret));
        }
        
        // 📨 Відправляти статус кожні 5 секунд (тільки якщо не спим)
        if (!g_can_sleep_mode) {
uint32_t last_status_sent = 0;
            uint32_t now = xTaskGetTickCount();
            if (now - last_status_sent >= pdMS_TO_TICKS(5000)) {
                // Відправка статусу по фізичній CAN-шині
                //send_can_status();
                
                // 📊 Відправка статистики через Wi-Fi
                esp_err_t stat_ret = wifi_can_sniffer_send_statistics(
                    g_can_frames_per_second,
                    g_can_bytes_per_second
                );                (void)stat_ret;                                
                //if (stat_ret == ESP_OK) {
                //    ESP_LOGI(TAG_CAN, "📊 Статистика: %d кадрів/с, %d байт/с", 
                //             g_can_frames_per_second, g_can_bytes_per_second);
                //}
                
                // Скидаємо лічильники
                g_can_frames_per_second = 0;
                g_can_bytes_per_second = 0;
                
                last_status_sent = now;
            }
            /*
            // 🧪 ТЕСТОВА ВІДПРАВКА: відправляємо різні CAN-пакети кожну секунду
uint32_t last_test_sent = 0;
uint8_t test_counter = 0;
            if (now - last_test_sent >= pdMS_TO_TICKS(50)) {
                // Генеруємо випадкові тестові пакети з різними ID та довжиною
                uint32_t test_ids[] = {0x100, 0x123, 0x1A5, 0x200, 0x2F8, 0x350, 0x420, 0x555,
                                      0x510, 0x622, 0x700, 0x7FF, 0x333, 0x444, 0x666, 0x777};
                
                // Рандомний ID та довжина (не по порядку)
                uint8_t id_index = (test_counter * 7 + test_counter / 3) % (sizeof(test_ids) / sizeof(test_ids[0]));
                uint8_t data_length = 1 + ((test_counter * 13) % 8);  // 1-8 байтів в рандомному порядку
                
                uint8_t test_data[8];
                // Генеруємо різні дані для кожного пакету
                for (int i = 0; i < data_length; i++) {
                    test_data[i] = (test_counter * 7 + i * 13 + id_index * 23) & 0xFF;
                }
                
                esp_err_t test_ret = wifi_can_sniffer_send(test_ids[id_index], data_length, test_data);
                if (test_ret == ESP_OK) {
                    // Логуємо тільки передані байти
                    char log_buf[128];
                    int pos = 0;
                    for (int i = 0; i < data_length; i++) {
                        pos += snprintf(log_buf + pos, sizeof(log_buf) - pos, "%02X ", test_data[i]);
                    }
                    ESP_LOGI(TAG_CAN, "🧪 TEST #%d: ID=0x%03X, Len=%d, Data=%s", 
                             test_counter, test_ids[id_index], data_length, log_buf);
                }
                
                wifi_can_sniffer_flush();
                test_counter++;
                last_test_sent = now;
            }*/
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void can_sleep_monitor_task(void *arg)
{
    ESP_LOGI(TAG_CAN, "CAN sleep monitor task started");
    
    // Ініціалізація таймерів активності
    uint32_t current_time = xTaskGetTickCount();
    g_last_can_activity_time = current_time;
    g_last_system_activity_time = current_time;
    ESP_LOGI(TAG_CAN, "CAN sleep monitor: ініціалізація таймерів активності");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CAN_SLEEP_CHECK_PERIOD));
        
        // Check new sleep settings first
        if (should_enter_sleep()) {
            ESP_LOGI(TAG_CAN, "CAN bus idle for %d seconds, entering sleep mode", 
                    g_sleep_settings.idle_timeout_sec);
            ESP_LOGI(TAG_CAN, "Sleep mode: %d, timeout: %d", g_sleep_settings.sleep_mode, g_sleep_settings.idle_timeout_sec);
            
            // Log current state before sleep
            
            // Log current state before sleep
            ESP_LOGI(TAG_CAN, "State before sleep:");
            ESP_LOGI(TAG_CAN, "  playlist_ready: %d", g_playlist_ready);
            ESP_LOGI(TAG_CAN, "  current_track: %d", g_current_track);
            ESP_LOGI(TAG_CAN, "  last_saved_position: %d ms", g_last_saved_position);
            ESP_LOGI(TAG_CAN, "  player_task_handle: %p", g_player_task_handle);
            
            // 🎵 Зберігаємо стан плеєра ПЕРЕД його зупинкою
            g_playback_was_active_before_sleep = (g_player_task_handle != NULL);
            ESP_LOGI(TAG_CAN, "🎵 Playback was active before sleep: %s", g_playback_was_active_before_sleep ? "YES" : "NO");
            
            // Stop player if playing
            if (g_player_task_handle) {
                ESP_LOGI(TAG_CAN, "Stopping player for sleep");
                stop_playback();
            }
            
            // Save position before sleep
            if (g_playlist_ready && g_current_track >= 0 && g_last_saved_position > 0) {
                ESP_LOGI(TAG_CAN, "Saving position before sleep: track %d, %d ms", 
                         g_current_track, g_last_saved_position);
                save_playback_position(g_current_track, g_last_saved_position);
            }
            
            // Unmount USB if mounted
            if (g_usb_mounted) {
                ESP_LOGI(TAG_CAN, "Unmounting USB before sleep");
                unmount_usb_device();
            }
            
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            // Enter sleep based on configured mode
            enter_configured_sleep();
            continue; // After wakeup, continue monitoring
        }
    }
}

esp_err_t init_can(void)
{
    
    // 🔄 Всегда сбрасываем на стандартную скорость 125 kbps при инициализации
    g_can_bitrate = CAN_DEFAULT_BITRATE;
    ESP_LOGI(TAG_CAN, "   📍 Скорость CAN установлена на: %d кбит/с", g_can_bitrate);
    
    // Get timing config based on global bitrate
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, CAN_MODE);
    twai_timing_config_t t_config = get_can_timing_config(g_can_bitrate);
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    
    ESP_LOGI(TAG_CAN, "   ⚙️ Конфигурация: TX=GPIO%d, RX=GPIO%d, Mode=%d", 
             CAN_TX_GPIO, CAN_RX_GPIO, CAN_MODE);
    
    // Configure
    g_config.tx_io = CAN_TX_GPIO;
    g_config.rx_io = CAN_RX_GPIO;
    g_config.clkout_io = GPIO_NUM_NC;
    g_config.bus_off_io = GPIO_NUM_NC;
    g_config.tx_queue_len = 5;
    g_config.rx_queue_len = 20;
    
    esp_err_t ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CAN, "   ❌ ОШИБКА установки CAN драйвера: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG_CAN, "   ✅ CAN драйвер установлен");
    
    ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_CAN, "   ❌ ОШИБКА запуска CAN: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG_CAN, "   ✅ CAN драйвер запущен");
    
    // Configure alerts
    uint32_t alerts_enabled;
    twai_read_alerts(&alerts_enabled, 0);
    twai_reconfigure_alerts(CAN_ALERT_FLAGS, NULL);
    
    ESP_LOGI(TAG_CAN, "✅ CAN ИНИЦИАЛИЗАЦИЯ УСПЕШНА");
    ESP_LOGI(TAG_CAN, "   🔌 TX: GPIO%d, RX: GPIO%d", CAN_TX_GPIO, CAN_RX_GPIO);
    ESP_LOGI(TAG_CAN, "   ⚡ Скорость: %d кбит/с", g_can_bitrate);
    return ESP_OK;
}

twai_timing_config_t get_can_timing_config(uint16_t bitrate_kbps)
{
    static struct {
        uint16_t rate;
        twai_timing_config_t config;
    } configs[] = {
        {100, TWAI_TIMING_CONFIG_100KBITS()},
        {125, TWAI_TIMING_CONFIG_125KBITS()},
        {250, TWAI_TIMING_CONFIG_250KBITS()},
        {500, TWAI_TIMING_CONFIG_500KBITS()},
        {1000, TWAI_TIMING_CONFIG_1MBITS()},
    };
    
    for (int i = 0; i < 5; i++) {
        if (configs[i].rate == bitrate_kbps) {
            return configs[i].config;
        }
    }
    
    // Default fallback to 125 kbps
    return configs[1].config;
}
