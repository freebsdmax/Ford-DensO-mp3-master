/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <sys/stat.h>
#include <dirent.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"
#include "ffconf.h"
#include "errno.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "cJSON.h"
#include "i2s_stream.h"
#include "esp_wifi.h"  // Для Wi-Fi функцій
#include "esp_netif.h"
#include "esp_event.h"
// ДОБАВЬТЕ ЭТОТ INCLUDE ДЛЯ I2S:
#include "driver/i2s_std.h"  // Для I2S в ESP-IDF v5.x
#include "driver/i2s_types.h"


#include <math.h>  // Для sin, M_PI
#include "board.h"
#include "audio_hal.h"




#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "fatfs_stream.h"
#include "i2s_stream.h"
#ifdef CONFIG_AUDIO_SUPPORT_MP3_DECODER
#include "mp3_decoder.h"
#elif (CONFIG_AUDIO_SUPPORT_AMRNB_DECODER ||    \
        CONFIG_AUDIO_SUPPORT_AMRWB_DECODER)
#include "amr_decoder.h"
#elif CONFIG_AUDIO_SUPPORT_OPUS_DECODER
#include "opus_decoder.h"
#elif CONFIG_AUDIO_SUPPORT_OGG_DECODER
#include "ogg_decoder.h"
#elif CONFIG_AUDIO_SUPPORT_FLAC_DECODER
#include "flac_decoder.h"
#elif CONFIG_AUDIO_SUPPORT_WAV_DECODER
#include "wav_decoder.h"
#elif ((CONFIG_AUDIO_SUPPORT_AAC_DECODER) ||    \
        (CONFIG_AUDIO_SUPPORT_M4A_DECODER) ||   \
        (CONFIG_AUDIO_SUPPORT_TS_DECODER) ||    \
        (CONFIG_AUDIO_SUPPORT_MP4_DECODER))
#include "aac_decoder.h"
#endif
#include "driver/twai.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "driver/uart.h"
#include "filter_resample.h"  // Для динамічної підстройки частоти
#include "wifi_can_sniffer.h"  // 📡 Wi-Fi CAN-Sniffer компонент
#include "web_ota.h"
#include "uart_ota_protocol.h"  // UART OTA протокол
#include "player_version.h"
#include "player_nvs.h"
#include "player_playlist.h"
#include "player_pipeline.h"
#include "player_console.h"
#include "app_messages.h"
#include "player_sleep.h"
#include "player_usb.h"
#include "player_can.h"
#include "player_ble.h"
#include "player_wifi.h"
#include "player_link.h"
#include "player_logs.h"
#include "player_slave.h"     // Версія основного додатку
#include "ble_player_service.h" // BLE GATT сервер для мобільного додатку
#include "elm327_emu.h"         // ELM327 емулятор для діагностики
#pragma GCC diagnostic ignored "-Wformat-truncation"

static const char *TAG = "Player";

#define MNT_PATH         "/ROOT"
#define APP_QUIT_PIN     GPIO_NUM_0
#define BUFFER_SIZE      8192
#define MAX_MSC_DEVICES  CONFIG_FATFS_VOLUME_COUNT
#define MAX_PLAYLIST_SIZE 1000
#define NVS_NAMESPACE    "audio_player"
// Keys in NVS
#define NVS_KEY_PLAYER_DATE "player_date" // Версія player - дата
#define NVS_KEY_PLAYER_TIME "player_time" // Версія player - час
// NVS key for sniffer enabled flag
#define NVS_KEY_UART_DATE   "uart_date"   // Версія UART - дата
#define NVS_KEY_UART_TIME   "uart_time"   // Версія UART - час
#define POSITION_SAVE_INTERVAL_MS 60000  // 🕒 Зберігати позицію кожні 60 секунд
#define MIN_POSITION_CHANGE_MS 10000     // 📏 Мінімальна зміна позиції для збереження
#define PLAYBACK_STALL_TIMEOUT_MS 8000   // ⏳ 8 сек без прогресу - перезапуск треку


// Глобальний прапор для детальних логів (false - приховати зайві логи)
extern bool g_verbose_logs;
extern bool g_player_logs;


// 🔌 CAN налаштування
#define CAN_TX_GPIO      GPIO_NUM_4
#define CAN_RX_GPIO      GPIO_NUM_15
#define CAN_MODE         TWAI_MODE_NORMAL

#define CAN_DEFAULT_BITRATE 125  // Default 125 kbps

// Глобальна скорість CAN (можна змінювати)
uint16_t g_can_bitrate = CAN_DEFAULT_BITRATE;

// Получить конфиг timing по скорости - используем статический кеш

// 🌐 Wi-Fi CAN-Sniffer конфигурация
#define WIFI_SSID        "ESP32-CAN-Sniffer"
#define WIFI_PASSWORD    "12345678"

// AP mode defaults (can be configured via web interface)
// Окремі налаштування для підключення як клієнт (STA)
// Заміни на свої значення мережі, до якої треба підключатися
#ifndef WIFI_STA_SSID
#endif
#ifndef WIFI_STA_PASSWORD
#endif
#define PC_IP_ADDR       "192.168.4.255"  // 🖥️ Broadcast - все устройства получат пакеты
#define UDP_PORT         0xAA55             // Порт как в оригинальном CAN-Sniffer

// UART for slave commands

// Команди для slave (використовуємо ті самі значення з протоколу)








static void check_i2s_input_signal(void);
// Callbacks from WiFi CAN sniffer (defined later)

// External I2S input pins (BCLK, WS, DIN) - change these to match your board
#ifndef EXT_I2S_BCLK_PIN
#define EXT_I2S_BCLK_PIN 41
#endif
#ifndef EXT_I2S_WS_PIN
#define EXT_I2S_WS_PIN 39
#endif
#ifndef EXT_I2S_DIN_PIN
#define EXT_I2S_DIN_PIN 40
#endif


// После других глобальных переменных
int g_volume = 10;  // Громкость от 0 до 100, по умолчанию 70%
audio_board_handle_t g_board_handle = NULL;  // Обработчик аудиочипа



// 📡 CAN ідентифікатори за вашим кодом:
#define CAN_ID_MAIN          0x2D5      // 📟 Основний ID CAN повідомлення
#define CMD_NEXT_TRACK       0x49       // ⏭️ Наступний трек
#define CMD_PREV_TRACK       0x89       // ⏮️ Попередній трек
#define CMD_MODE_SWITCH      0x19       // 🔄 Перемикання режиму (MODE)

// 🎛️ Типи подій для CAN:
#define EVENT_TRACK_CHANGED   0x01    // 🔀 Зміна треку
#define EVENT_PLAYBACK_STATE  0x02    // ⏯️ Зміна стану відтворення
#define EVENT_USB_CONNECTED   0x03    // 💾 USB підключено
#define EVENT_USB_DISCONNECTED 0x04   // 📤 USB відключено



bool g_can_sleep_mode = false;
uint32_t g_last_can_activity_time = 0;
uint32_t g_sleep_start_time = 0;
#define CAN_SLEEP_TIMEOUT_MS   1000000  // 😴 10 секунд без активності - спати
#define CAN_WAKEUP_DELAY_MS    5000   // ⏰ 5 секунд після пробудження перед відтворенням
#define CAN_SLEEP_CHECK_PERIOD 1000   // 🔍 Перевіряти кожну секунду

// Пин-индикатор сна: при переходе в сон — ставим LOW, при пробуждении — HIGH
#ifndef SLEEP_IND_PIN
#define SLEEP_IND_PIN 21
#endif

// 🌙 Sleep settings structure


// 🌙 Global sleep settings


// 🌙 Last system activity time (for idle detection)


// 🕒 Debounce для CAN команд (антидрібниця)
uint32_t g_last_next_command_time = 0;
uint32_t g_last_prev_command_time = 0;
#define CAN_COMMAND_DEBOUNCE_MS 500  // 500 мс між командами


// 📋 Глобальні змінні для плейлиста
bool g_decoder_error = false;  // 🔴 Прапор помилки декодера MP3
bool g_skip_to_next = false;
bool g_skip_to_prev = false;  // ⏮️ Додано для попереднього треку
bool g_play_specific_track = false;  // 🎯 Чи відтворювати конкретний трек з веб
bool g_is_paused = false;     // ⏸️ Додано для паузи
// Збережений стан сніфера (true=enabled)
bool g_sniffer_enabled = false;
TaskHandle_t g_player_task_handle = NULL;
int64_t g_stall_last_ts_ms = 0;    // ⏳ Час останньої перевірки прогресу
size_t g_stall_last_bytes = 0;     // 📊 Останній прогрес у байтах
int32_t g_seek_position_ms = -1;   // 🎯 Позиція для перемотки через веб
int32_t g_current_track_duration_ms = 180000; // Тривалість поточного треку

// 📡 Wi-Fi CAN-Sniffer статистика
uint16_t g_can_frames_per_second = 0;
uint16_t g_can_bytes_per_second = 0;
uint64_t g_can_total_frames = 0;
uint64_t g_can_total_bytes = 0;

// 🔌 Глобальні змінні для USB управління
bool g_usb_mounted = false;
int g_current_mount_slot = -1;

// Режим внешнего входа
volatile bool g_mode_external = false; // true = воспроизводим внешний I2S вход
static int g_last_source_pref = 0; // 0 = USB(default), 1 = external
audio_element_handle_t g_external_writer = NULL;
TaskHandle_t g_i2s_passthrough_task_handle = NULL;
volatile bool g_external_i2s_flush_requested = false;
volatile uint32_t g_external_i2s_rx_overflows = 0;
volatile uint32_t g_external_i2s_rx_overflow_bytes = 0;


// This task reads from the next available I2S port (slave) and writes directly to the g_external_writer element (I2S0 master)

// 💾 Змінні для збереження позиції
int32_t g_saved_track_index = -1;
int32_t g_saved_position_ms = 0;
bool g_restore_playback = false;
bool g_playback_was_active_before_sleep = false; // 🎵 Чи був активний плеєр перед сном
uint32_t g_last_position_save_time = 0;
int g_last_saved_position = 0;

static bool g_processing_disconnect = false;
static bool g_processing_connect = false;
static SemaphoreHandle_t g_usb_mutex = NULL;
i2s_chan_handle_t g_i2s_tx_handle = NULL;

/**
 * 📝 Запис про пристрій MSC
 */





// 📋 Прототипи функцій

// 🔉 ДОБАВЬТЕ ЭТУ СТРОЧКУ:


// 📡 CAN функції

// 🌐 WiFi функції

/**
 * 🗃️ Ініціалізація NVS
 */

// 💾 Збереження останнього вибраного джерела (USB або зовнішній вхід)

// 📥 Завантаження останнього вибраного джерела

// 💾 Збереження останнього Wi-Fi режиму (AP або STA)

// 💾 Збереження стану Wi-Fi CAN-Sniffer (1=enabled,0=disabled)

// 📥 Завантаження стану Wi-Fi CAN-Sniffer

// 📥 Завантаження останнього Wi-Fi режиму

// 💾 Збереження STA credentials

// 📥 Завантаження STA credentials

// 💾 Збереження AP credentials

// 📥 Завантаження AP credentials


// 🌐 Отримати поточні Wi-Fi налаштування (для веб-інтерфейсу)

// 🌐 Зберегти Wi-Fi налаштування (для веб-інтерфейсу)

// 🌙 Forward declarations for sleep functions

// 🌙 Отримати налаштування сну (для веб-інтерфейсу)

// 🌙 Зберегти налаштування сну (для веб-інтерфейсу)

/**
 * 🌙 Load sleep settings from NVS
 */

/**
 * 🌙 Save sleep settings to NVS
 */

/**
 * 🌙 Update system activity timestamp
 */

/**
 * 🌙 Check if should enter sleep mode
 */

/**
 * 🌙 Enter sleep mode based on configured settings
 */

/**
 * 🔌 Ініціалізація CAN драйвера
 */

/**
 * 🔄 Динамическая смена скорости CAN
 */

/**
 * Ініціалізація I2C master
 */

/**
 * Відправити просту команду по UART з префіксом
 * Підтримує як старий формат (без префіксу) для сумісності,
 * так і новий з префіксом для розділення OTA та звичайних команд
 */

/**
 * Отримати версію збірки від slave пристрою
 */



/**
 * 📨 Обробити CAN повідомлення
 */

/**
 * 🌐 Фонова задача для підключення до STA WiFi
 */

/**
 * 👁️🗨️ Завдання моніторингу CAN активності та переходу в сон
 */

/**
 * 📊 Відправити статус по CAN
 */

/**
 * 📨 Відправити подію по CAN
 */

/**
 * 📡 Завдання обробки CAN
 */

/**
 * 💾 Зберегти позицію відтворення (с проверкой на корректность)
 */


/**
 * 🗑️ Очистити збережену позицію
 */

/**
 * 📝 Додати файл до плейлиста
 */

/**
 * 🗑️ Очистити плейлист
 */

/**
 * 🧹 Очистка I2S драйвера (альтернатива i2s_driver_uninstall)
 */
/**
 * 🧹 Упрощенная очистка I2S драйвера
 */

/**
 * 🧹 Очистка аудио буферов (альтернатива audio_element_flush_*)
 */

/**
 * 🧹 Безопасная очистка I2S драйвера
 */


/**
 * ⏸️ Зупинити відтворення
 */

/**
 * ⏭️ Відтворити наступний трек
 */

/**
 * ⏮️ Відтворити попередній трек
 */

/**
 * 📱 BLE статус отримання - отримує поточний статус плеєра
 */







/**
 * 📱 BLE команди обробник - обробляє команди від BLE клієнта
 */


/**
 * 🌅 Выйти из глубокого сна
 */



/**
 * 🔌 Демонтувати USB пристрій
 */

/**
 * 🔌 Монтувати USB пристрій
 */

/**
 * 🔊 Изменить громкость аудиочипа
 */

/**
 * 🔊 Увеличить громкость
 */

/**
 * 🔊 Уменьшить громкость
 */

/**
 * 📞 Callback для обробки подій аудіо елементів
 */

/**
 * 🔍 Аналіз MP3 файлу для діагностики продуктивності
 */

/**
 * 🎯 Встановити позицію відтворення (альтернативный метод)
 */



/**
 * 🔍 Детальна перевірка позиції файлу
 */

/**
 * 🔊 Відтворити тестовий синусоїдальний сигнал для перевірки I2S виходу.
 *
 * @param i2s_writer    Елемент i2s_stream для запису.
 * @param duration_ms   Тривалість відтворення в мілісекундах.
 */

// 🛑 Stop all tasks for OTA update

/**
 * 🎧 Завдання відтворення аудіо (исправленная версия)
 */






/**
 * 🔍 Рекурсивно знайти всі MP3 файли та додати їх до плейлиста
 */

/**
 * 📝 Створити плейлист з усіх MP3 файлів на USB
 */

/**
 * 🔍 Знайти вільний слот у таблиці пристроїв
 */

/**
 * 🆕 Виділити новий запис пристрою MSC та монтувати його до VFS
 */

/**
 * 🔍 Знайти слот за дескриптором пристрою MSC
 */

/**
 * 🗑️ Звільнити ресурси, пов'язані з певним пристроєм MSC за індексом слота
 */

/**
 * 🗑️ Звільнити всі підключені пристрої MSC
 */

/**
 * 🔘 Зворотний виклик натискання кнопки BOOT
 */

/**
 * 🔍 Знайти USB-адресу за дескриптором пристрою MSC
 */

/**
 * 📞 Зворотний виклик драйвера MSC
 */


/**
 * Запуск пайплайна для внешнего I2S входа (ESP32 -> этот ESP32), который напрямую переадресует
 * входящий PCM на I2S-выход (PCM5102).
 */


/**
 * 🔌 Завдання USB
 */

// ============================================================================
// 🌐 Функції для веб-управління плеєром
// ============================================================================

/**
 * 📋 Отримати плейлист у форматі JSON
 */
char* get_playlist_json(void) {
    if (!g_playlist_ready || !g_playlist || g_playlist_size == 0) {
        return strdup("[]");
    }

    // Розрахувати розмір буфера
    size_t buf_size = 2; // []
    for (int i = 0; i < g_playlist_size; i++) {
        if (g_playlist[i]) {
            buf_size += strlen(g_playlist[i]) * 2 + 8; // utf8 expansion + "name",
        }
    }

    char *json = malloc(buf_size);
    if (!json) {
        return strdup("[]");
    }

    strcpy(json, "[");
    for (int i = 0; i < g_playlist_size; i++) {
        if (g_playlist[i]) {
            if (i > 0) strcat(json, ",");
            strcat(json, "\"");
            // Просте екранування лапок
            char display[512];
            playlist_get_display_name(i, display, sizeof(display));
            const char *src = display;
            char *dst = json + strlen(json);
            while (*src) {
                if (*src == '"') *dst++ = '\\';
                *dst++ = *src++;
            }
            *dst = '\0';
            strcat(json, "\"");
        }
    }
    strcat(json, "]");
    return json;
}

/**
 * 🎛️ Управління плеєром через веб
 */

/**
 * 🎯 Встановити позицію відтворення через веб
 */

/**
 * 📊 Отримати статус плеєра у форматі JSON
 */
char* get_player_status_json(void) {
    char *json = malloc(256);
    if (!json) return strdup("{}");
    
    // Використовуємо розраховану тривалість
    int duration_ms = g_current_track_duration_ms;
    if (duration_ms <= 0) duration_ms = 180000; // Запасний варіант
    
    snprintf(json, 256, "{\"current_track\":%d,\"position_ms\":%d,\"duration_ms\":%d,\"is_paused\":%s,\"playlist_size\":%d,\"mode\":\"%s\",\"sniffer_enabled\":%s}",
             g_current_track, g_last_saved_position, duration_ms, g_is_paused ? "true" : "false", g_playlist_size, g_mode_external ? "external" : "USB", g_sniffer_enabled ? "true" : "false");
    
    return json;
}

/**
 * @brief Callback для обработки входящих CAN-пакетов из Wi-Fi
 */

/**
 * @brief Callback для обработки команд управления из Wi-Fi
 */

void app_main(void)
{
#ifdef CONFIG_FATFS_MAX_LFN
    ESP_LOGI(TAG, "FATFS: LFN enabled, max=%d, codepage=%d", CONFIG_FATFS_MAX_LFN, CONFIG_FATFS_CODEPAGE);
#else
    ESP_LOGI(TAG, "FATFS: LFN DISABLED, codepage=%d", CONFIG_FATFS_CODEPAGE);
#endif
    // 🌙 ПЕРШ ЗА ВСЕ: перевіряємо пробудження від сну
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    
    // Рано ініціалізуємо NVS для завантаження налаштувань логів
    esp_err_t nvs_err = init_nvs();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Помилка ініціалізації NVS: %s", esp_err_to_name(nvs_err));
    } else {
        // Завантажуємо налаштування логів
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Loading component logs from NVS");
            uint8_t val;
            if (nvs_get_u8(nvs_handle, "verbose_logs", &val) == ESP_OK) {
                g_verbose_logs = (val != 0);
                ESP_LOGI(TAG, "Loaded verbose_logs: %d", g_verbose_logs);
            } else {
                ESP_LOGI(TAG, "Failed to load verbose_logs from NVS");
            }
            if (nvs_get_u8(nvs_handle, "player_logs", &val) == ESP_OK) {
                g_player_logs = (val != 0);
                ESP_LOGI(TAG, "Loaded player_logs: %d", g_player_logs);
            } else {
                ESP_LOGE(TAG, "Failed to load player_logs from NVS");
            }
            if (nvs_get_u8(nvs_handle, "wifi_logs", &val) == ESP_OK) {
                g_wifi_can_sniffer_logs = (val != 0);
                ESP_LOGI(TAG, "Loaded wifi_can_sniffer_logs: %d", g_wifi_can_sniffer_logs);
            } else {
                ESP_LOGE(TAG, "Failed to load wifi_can_sniffer_logs from NVS");
            }
            if (nvs_get_u8(nvs_handle, "web_ota_logs", &val) == ESP_OK) {
                g_web_ota_logs = (val != 0);
                ESP_LOGI(TAG, "Loaded web_ota_logs: %d", g_web_ota_logs);
            } else {
                ESP_LOGE(TAG, "Failed to load web_ota_logs from NVS");
            }
            if (nvs_get_u8(nvs_handle, "uart_ota_logs", &val) == ESP_OK) {
                g_uart_ota_logs = (val != 0);
                ESP_LOGI(TAG, "Loaded uart_ota_logs: %d", g_uart_ota_logs);
            } else {
                ESP_LOGE(TAG, "Failed to load uart_ota_logs from NVS");
            }
            if (nvs_get_u8(nvs_handle, "elm327_logs", &val) == ESP_OK) {
                g_elm327_logs = (val != 0);
                ESP_LOGI(TAG, "Loaded elm327_logs: %d", g_elm327_logs);
            } else {
                ESP_LOGE(TAG, "Failed to load elm327_logs from NVS");
            }
            nvs_close(nvs_handle);
        } else {
            ESP_LOGE(TAG, "Failed to open NVS for loading component logs: %s", esp_err_to_name(err));
        }
    }
    
    // Встановлюємо рівні логів на основі налаштувань
    esp_log_level_set("*", ESP_LOG_WARN);               // ⚠️ Інші компоненти - попередження
    esp_log_level_set("BLE_Player", ESP_LOG_INFO);
    esp_log_level_set("Player", g_player_logs ? ESP_LOG_INFO : ESP_LOG_NONE);          // ℹ️ Наш код - інформаційний рівень (TAG = "Player")
    esp_log_level_set("WiFiCanSniffer", g_wifi_can_sniffer_logs ? ESP_LOG_INFO : ESP_LOG_NONE);  // ✅ Дозволяємо бачити логи Wi-Fi CAN sniffer
    player_logs_load();
    player_logs_apply();
    // Ініціалізація TCP/IP стеку та дефолтного event-loop — робимо один раз тут
    esp_err_t _neterr = esp_netif_init();
    if (_neterr != ESP_OK && _neterr != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "❌ esp_netif_init failed: %s", esp_err_to_name(_neterr));
    } else {
        ESP_LOGI(TAG, "✅ esp_netif_init ok (or already initialized)");
    }
    _neterr = esp_event_loop_create_default();
    if (_neterr != ESP_OK && _neterr != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "❌ esp_event_loop_create_default failed: %s", esp_err_to_name(_neterr));
    } else {
        ESP_LOGI(TAG, "✅ esp_event_loop_create_default ok (or already created)");
    }
    esp_log_level_set("web_ota", ESP_LOG_WARN);         // 🔄 Дозволяємо бачити логи OTA
    esp_log_level_set("UART_OTA_MASTER", g_uart_ota_logs ? ESP_LOG_INFO : ESP_LOG_NONE); // 📡 Дозволяємо бачити логи UART OTA
    esp_log_level_set("ELM327_WIFI", g_elm327_logs ? ESP_LOG_INFO : ESP_LOG_NONE);      // 🔧 Дозволяємо бачити логи ELM327
    
    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        // Пробудження від Deep Sleep по таймеру
        ESP_LOGI(TAG, "🌅 ПРОБУДЖЕННЯ ВІД DEEP SLEEP");
        ESP_LOGI(TAG, "   Причина пробудження: %d", wakeup_reason);
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
        // Пробудження від Deep Sleep по GPIO
        ESP_LOGI(TAG, "🌅 ПРОБУДЖЕННЯ ВІД DEEP SLEEP (GPIO)");
    } else if (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGW(TAG, "🌅 Пробудження від невідомого джерела: %d", wakeup_reason);
    } else {
        ESP_LOGI(TAG, "🌅 Нормальний запуск системи");
    }
    
    ESP_LOGI(TAG, "... ...");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║     🎵 ESP32-S3 MP3 PLAYER + I2S INPUT       ║");
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "🔨 Build: %s %s", __DATE__, __TIME__);
    ESP_LOGI(TAG, "... ...");
    
    // 🛡️ Ініціалізація мьютексу для USB
    g_usb_mutex = xSemaphoreCreateMutex();
    if (g_usb_mutex == NULL) {
        ESP_LOGE(TAG, "❌ Не вдалося створити мьютекс USB");
    } else {
        ESP_LOGI(TAG, "✅ Мьютекс USB створено");
    }

    ESP_LOGI(TAG, "🚀 ЗАПУСК СИСТЕМИ MP3-ПЛЕЄРА");
    gpio_reset_pin((gpio_num_t)SLEEP_IND_PIN);          // Сбросить настройки пина
    gpio_set_direction((gpio_num_t)SLEEP_IND_PIN, GPIO_MODE_OUTPUT); // Настроить как выход
    gpio_set_level((gpio_num_t)SLEEP_IND_PIN, 1);       // Установить HIGH
    // === КРИТЕРІЙ 2: NVS вже ініціалізована вище ===
    {
        // 💾 Зберігаємо версію player при першому запуску або при зміні
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            // Перевіряємо поточну збережену версію
            char saved_date[12] = "";
            char saved_time[9] = "";
            size_t len = sizeof(saved_date);
            nvs_get_str(nvs_handle, NVS_KEY_PLAYER_DATE, saved_date, &len);
            len = sizeof(saved_time);
            nvs_get_str(nvs_handle, NVS_KEY_PLAYER_TIME, saved_time, &len);
            
            // Оновлюємо тільки якщо версія змінилась
            bool version_changed = (strcmp(saved_date, __DATE__) != 0) || (strcmp(saved_time, __TIME__) != 0);
            
            if (version_changed || strlen(saved_date) == 0) {
                // Зберігаємо поточну версію збірки player
                err = nvs_set_str(nvs_handle, NVS_KEY_PLAYER_DATE, __DATE__);
                if (err == ESP_OK) {
                    err = nvs_set_str(nvs_handle, NVS_KEY_PLAYER_TIME, __TIME__);
                }
                if (err == ESP_OK) {
                    nvs_commit(nvs_handle);
                    ESP_LOGI(TAG, "💾 Оновлено версію Player: %s %s", __DATE__, __TIME__);
                }
            } else {
                ESP_LOGI(TAG, "📋 Версія Player не змінилась: %s %s", saved_date, saved_time);
            }
            nvs_close(nvs_handle);
        }
        
        // 📥 Завантажуємо збережену позицію
        esp_err_t load_result = load_playback_position(&g_saved_track_index, &g_saved_position_ms);
        
        if (load_result == ESP_OK) {
    ESP_LOGI(TAG, "✅ Завантажено збережену позицію:");
    ESP_LOGI(TAG, "   🎵 Трек: %d", g_saved_track_index);
    ESP_LOGI(TAG, "   ⏱️ Позиція: %d мс", g_saved_position_ms);
    
    // 🎯 Устанавливаем флаг восстановления ТОЛЬКО если позиция > 0
    if (g_saved_position_ms > 0) {
        g_restore_playback = true;
        g_current_track = g_saved_track_index;
        ESP_LOGI(TAG, "🎵 Буде відновлено з треку %d, позиція %d мс", 
                 g_current_track, g_saved_position_ms);
    } else {
        // Если позиция 0, начинаем с начала трека
        if (g_saved_track_index >= 0) {
            g_current_track = g_saved_track_index;
            ESP_LOGI(TAG, "🎵 Починаємо з треку %d (з початку)", g_current_track);
        }
        g_restore_playback = false;
    }
}
        // 📥 Завантажуємо останній вибраний режим джерела (USB/External)
        load_last_source_pref(&g_last_source_pref);
        ESP_LOGI(TAG, "🎚️ Перевага джерела при старті: %s", (g_last_source_pref==1)?"External":"USB");
        
        // 🌙 Завантажуємо налаштування сну
        esp_err_t sleep_load_result = load_sleep_settings(&g_sleep_settings);
        if (sleep_load_result == ESP_OK) {
            ESP_LOGI(TAG, "🌙 Завантажено налаштування сну: режим=%d, таймаут=%d сек", 
                     g_sleep_settings.sleep_mode, g_sleep_settings.idle_timeout_sec);
        } else {
            ESP_LOGW(TAG, "⚠️ Не вдалося завантажити налаштування сну, використано дефолтні: режим=%d, таймаут=%d сек",
                     g_sleep_settings.sleep_mode, g_sleep_settings.idle_timeout_sec);
        }
        
        // 💾 Зберігаємо версію player в NVS
        nvs_handle_t nvs_handle_player;
        esp_err_t err_player = nvs_open("audio_player", NVS_READWRITE, &nvs_handle_player);
        if (err_player == ESP_OK) {
            nvs_set_str(nvs_handle_player, "player_date", __DATE__);
            nvs_set_str(nvs_handle_player, "player_time", __TIME__);
            nvs_commit(nvs_handle_player);
            nvs_close(nvs_handle_player);
            ESP_LOGI(TAG, "💾 Player version saved to NVS");
        } else {
            ESP_LOGE(TAG, "❌ Failed to save player version to NVS: %s", esp_err_to_name(err_player));
        }
        
        // 💾 Зберігаємо версію UART OTA в NVS
        extern void uart_ota_save_version(void);
        uart_ota_save_version();
        
        // 💾 Зберігаємо версію Web OTA в NVS
        extern void web_ota_save_version(void);
        web_ota_save_version();
    }
    
    // 🔉 ИНИЦИАЛИЗАЦИЯ АУДИОЧИПА ДЛЯ УПРАВЛЕНИЯ ГРОМКОСТЬЮ
ESP_LOGI(TAG, "🔊 Инициализация аудиочипа для контроля громкости...");
g_board_handle = audio_board_init();
if (g_board_handle) {
    // Запускаем кодек в режиме декодирования
    audio_hal_ctrl_codec(g_board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
    
    // Получаем текущую громкость
    int current_volume = 0;
    audio_hal_get_volume(g_board_handle->audio_hal, &current_volume);
    ESP_LOGI(TAG, "🔊 Текущая громкость аудиочипа: %d%%", current_volume);
    
    // Устанавливаем нашу громкость
    audio_hal_set_volume(g_board_handle->audio_hal, g_volume);
    ESP_LOGI(TAG, "🔊 Установлена громкость: %d%%", g_volume);
} else {
    ESP_LOGW(TAG, "⚠️ Не удалось инициализировать аудиочип, громкость не регулируется");
}
    // 🔍 Перевірка PSRAM (ESP-IDF v5.5.1)
#if CONFIG_SPIRAM
    ESP_LOGI(TAG, "🧠 Конфігурація PSRAM увімкнена");
    
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    
    if (total_psram > 0) {
        ESP_LOGI(TAG, "💾 Пам'ять системи:");
        ESP_LOGI(TAG, "   🧠 PSRAM: всього %d KB, вільно %d KB", 
                 (int)total_psram/1024, (int)free_psram/1024);
        ESP_LOGI(TAG, "   💾 Heap:  всього %d KB, вільно %d KB", 
                 (int)total_heap/1024, (int)free_heap/1024);
    } else {
        ESP_LOGW(TAG, "⚠️ PSRAM не ініціалізовано!");
    }
#else
    ESP_LOGW(TAG, "⚠️ PSRAM не сконфігуровано в menuconfig!");
#endif
    
    // === КРИТЕРІЙ 4: Перевір що CAN ініціалізується тільки тут ===
    ESP_LOGI(TAG, "🔌 Ініціалізація CAN...");
    esp_err_t can_err = init_can();
    if (can_err == ESP_OK) {
        // Ініціалізація UART master для відправки команд на зовнішній пристрій
        esp_err_t i2c_err = init_i2c_master();
    slave_version_start();
        if (i2c_err != ESP_OK) {
            ESP_LOGW(TAG, "⚠️ UART master ініціалізовано неуспішно: %s", esp_err_to_name(i2c_err));
        }
        // 🚀 Запускаємо задачу обробки CAN на ядрі 0
        if (xTaskCreatePinnedToCore(can_task, "can_task", 4096, NULL, 3, NULL, 0) == pdPASS) {
            ESP_LOGI(TAG, "✅ Задача CAN запущена (CPU0)");
        } else {
            ESP_LOGE(TAG, "❌ Не вдалося створити задачу CAN");
        }
        
        // === КРИТЕРІЙ 5: Перевір що задача моніторингу сну створюється тільки тут ===
        if (xTaskCreatePinnedToCore(can_sleep_monitor_task, "can_sleep_mon", 4096, NULL, 2, NULL, 0) == pdPASS) {
            ESP_LOGI(TAG, "✅ Задача моніторингу сну запущена (CPU0)");
        } else {
            ESP_LOGE(TAG, "❌ Не вдалося створити задачу моніторингу сну");
        }
    } else {
        ESP_LOGW(TAG, "⚠️ CAN не ініціалізовано, продовжуємо без нього");
    }
    
    // === КРИТЕРІЙ 6: Створення черги - має бути тільки один раз ===
    app_queue = xQueueCreate(10, sizeof(app_message_t));
    if (app_queue == NULL) {
        ESP_LOGE(TAG, "❌ Не вдалося створити чергу програми");
        return;
    }
    ESP_LOGI(TAG, "✅ Черга програми створена (розмір: 10)");
    
    // === КРИТЕРІЙ 7: Створення задачі USB - має бути тільки одна ===
    BaseType_t usb_task_created = xTaskCreatePinnedToCore(usb_task, "usb_task", 4096, NULL, 2, NULL, 0);
    if (usb_task_created != pdPASS) {
        ESP_LOGE(TAG, "❌ Не вдалося створити задачу USB");
        vQueueDelete(app_queue);
        return;
    }
    ESP_LOGI(TAG, "✅ Задача USB запущена");
    
    // === КРИТЕРІЙ 8: Налаштування GPIO для кнопки - перевір що не дублюється ===
    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    
    esp_err_t gpio_config_err = gpio_config(&input_pin);
    if (gpio_config_err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Помилка налаштування GPIO: %s", esp_err_to_name(gpio_config_err));
    } else {
        ESP_LOGI(TAG, "✅ GPIO %d налаштовано як вхід з підтяжкою вгору", APP_QUIT_PIN);
    }
    
    // === КРИТЕРІЙ 9: Встановлення обробника переривань - має бути один раз ===
    esp_err_t isr_install_err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    if (isr_install_err != ESP_OK && isr_install_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "❌ Помилка встановлення сервісу ISR: %s", esp_err_to_name(isr_install_err));
    } else {
        ESP_LOGI(TAG, "✅ Сервіс переривань встановлено");
    }
    
    // === КРИТЕРІЙ 10: Додавання обробника кнопки - перевір дублі ===
    esp_err_t isr_handler_err = gpio_isr_handler_add(APP_QUIT_PIN, gpio_cb, NULL);
    if (isr_handler_err != ESP_OK) {
        ESP_LOGE(TAG, "❌ Помилка додавання обробника кнопки: %s", esp_err_to_name(isr_handler_err));
    } else {
        ESP_LOGI(TAG, "✅ Обробник кнопки BOOT (GPIO%d) встановлено", APP_QUIT_PIN);
    }
    
    // 📡 Wi‑Fi CAN-Sniffer: load saved enabled flag from NVS
    int saved = 0;
    load_sniffer_enabled(&saved);
    if (saved) {
        ESP_LOGI(TAG, "📡 Wi-Fi CAN-Sniffer saved state = ENABLED — starting at boot");
        wifi_can_sniffer_config_t wifi_can_config = {
            .wifi_ssid = WIFI_SSID,
            .wifi_password = WIFI_PASSWORD,
            .pc_ip_str = PC_IP_ADDR,
            .udp_port = UDP_PORT,
            .on_can_message = wifi_can_on_message_callback,
            .on_control_command = wifi_can_on_control_callback,
        };
        esp_err_t wifi_can_err = wifi_can_sniffer_init(&wifi_can_config);
        if (wifi_can_err == ESP_OK) {
            g_sniffer_enabled = true;
            ESP_LOGI(TAG, "✅ Wi-Fi CAN-Sniffer initialized at boot");
        } else {
            ESP_LOGW(TAG, "⚠️ Failed to init sniffer at boot: %s", esp_err_to_name(wifi_can_err));
        }
    } else {
        ESP_LOGI(TAG, "📡 Wi-Fi CAN-Sniffer is DISABLED by default. Use web UI to enable.");
    }

    // Запускаємо веб-OTA незалежно від стану сніфера
    if (web_ota_start() == ESP_OK) {
        ESP_LOGI(TAG, "🌐 Web OTA доступен: http://192.168.4.1/");
    } else {
        ESP_LOGW(TAG, "⚠️ Web OTA не запустился");
    }

    // Спробувати Wi-Fi підключення з fallback (AP/STA) — робимо це завжди
    try_wifi_connection_with_fallback();

    // Встановити рівень логів для WiFiCanSniffer (буде використаний, коли сніфер запуститься)
    esp_log_level_set("WiFiCanSniffer", g_wifi_can_sniffer_logs ? ESP_LOG_INFO : ESP_LOG_NONE);
    
    ESP_LOGI(TAG, "🎧 Очікування підключення USB флеш-накопичувача...");
    ESP_LOGI(TAG, "✅ Для перемикання треків натисніть кнопку BOOT");
    
    // Запускаємо задачу обробки команд з терміналу
    if (xTaskCreatePinnedToCore(terminal_commands_task, "terminal_cmd", 4096, NULL, 3, NULL, 0) == pdPASS) {
        ESP_LOGI(TAG, "⌨️ Terminal commands task started (n=next, p=prev, m=mode)");
    } else {
        ESP_LOGW(TAG, "⚠️ Failed to create terminal commands task");
    }
    
    // ⏳ Затримка для стабілізації системи
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    // � BLE Player Service ініціалізація
    ESP_LOGI(TAG, "📱 Ініціалізація BLE Player Service...");
    esp_err_t ble_err = ble_player_service_init();
    if (ble_err == ESP_OK) {
        ble_player_set_command_handler(ble_command_handler);
        ble_player_set_playlist_callback(ble_get_player_playlist);
        ble_player_set_rpc_callback(ble_rpc_handler);
        ble_player_set_ota_frame_handler(ble_file_ota_frame);
        // Запускаємо періодичну відправку статусу кожну секунду
        ble_player_start_periodic_status(ble_get_player_status, 1000);
        ESP_LOGI(TAG, "✅ BLE Player Service готовий");
    } else {
        ESP_LOGW(TAG, "⚠️ BLE Player Service ініціалізація не вдалася: %s", esp_err_to_name(ble_err));
    }
    
    // �🔍 Враховуємо останній вибраний режим: якщо external — запускаємо його навіть без USB
    if (g_last_source_pref == 1 && !g_mode_external) {
        ESP_LOGI(TAG, "🎚️ Відновлення режиму: External input");
        if (start_external_input() == ESP_OK) {
            g_mode_external = true;
            ESP_LOGI(TAG, "✅ External input активовано на старті");
        } else {
            ESP_LOGW(TAG, "⚠️ Не вдалося запустити external input при старті");
        }
    }
    
    // === КРИТЕРІЙ 11: Головний цикл обробки повідомлень - має бути один ===
    while (1) {
        app_message_t msg;
        
        // ⏳ Чекаємо повідомлень у черзі
        if (xQueueReceive(app_queue, &msg, portMAX_DELAY) == pdTRUE) {
            
            /// === КРИТЕРІЙ 12: Обробка підключення USB ===
                 if (msg.id == APP_DEVICE_CONNECTED) {
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                    ESP_LOGI(TAG, "💾 [USB] ПРИСТРІЙ ПІДКЛЮЧЕНО - ПОЧАТОК ОБРОБКИ");
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                    
                    // 🛡️ ЗАХИСТ ВІД ПОВТОРНОГО ПІДКЛЮЧЕННЯ
                    if (g_processing_connect) {
                        ESP_LOGW(TAG, "⚠️ Підключення вже обробляється, пропускаємо...");
                        continue;
                    }
                    g_processing_connect = true;
                    
                    ESP_LOGI(TAG, "📌 Адреса USB пристрою: %d", msg.data.new_dev_address);
                    
                    // ⏳ КРИТИЧНА ЗАТРИМКА ПЕРЕД ПОЧАТКОМ
                    //ESP_LOGI(TAG, "⏳ 1. Критична затримка перед монтуванням (3 секунди)");
                    //vTaskDelay(pdMS_TO_TICKS(3000));
                    
                    // 🎯 Виділяємо слот для нового пристрою
                    int slot;
                    ESP_LOGI(TAG, "🔧 2. Монтування USB пристрою...");
                    bool is_new = false;
                    esp_err_t msc_res = allocate_new_msc_device(&msg, &slot, &is_new);
                    
                    if (msc_res != ESP_OK) {
                        ESP_LOGE(TAG, "❌ Помилка монтування USB: %s", esp_err_to_name(msc_res));
                        g_processing_connect = false;
                        continue;
                    }

                    if (!is_new) {
                        ESP_LOGI(TAG, "USB device already mounted (slot %d), ignoring duplicate connect", slot);
                        g_processing_connect = false;
                        continue;
                    }
                    
                    // 💾 Зберігаємо поточний слот монтування
                    g_current_mount_slot = slot;
                    ESP_LOGI(TAG, "✅ USB змонтовано в слот %d", slot);
                    
                    // ⏳ ЗАТРИМКА ДЛЯ СТАБІЛІЗАЦІЇ ФАЙЛОВОЇ СИСТЕМИ
                    // ESP_LOGI(TAG, "⏳ 3. Затримка для стабілізації файлової системи (2 секунди)");
                    // vTaskDelay(pdMS_TO_TICKS(2000));
                    
                    // 📝 Створюємо плейлист з усіх MP3 файлів
                    ESP_LOGI(TAG, "🔍 4. Створення плейлисту...");
                    create_playlist_from_usb();

                    // Якщо користувач раніше обрав External і він активний, не перезапускаємо плеєр з USB
                    if (g_mode_external || g_last_source_pref == 1) {
                        ESP_LOGI(TAG, "ℹ️ External режим у пріоритеті — USB змонтовано без автозапуску відтворення");
                        g_processing_connect = false;
                        continue;
                    }
                    
                    if (g_playlist_ready) {
                        ESP_LOGI(TAG, "✅ Плейлист створено: знайдено %d треків", g_playlist_size);
                        
                        // 💾 Завантаження збереженої позиції
                        if (g_saved_track_index >= 0 && g_saved_track_index < g_playlist_size) {
                            g_current_track = g_saved_track_index;
                            g_restore_playback = true;
                            ESP_LOGI(TAG, "💾 Відновлення з треку %d, позиція %d мс", 
                                    g_saved_track_index, g_saved_position_ms);
                        }
                        
                        // ⏳ ЗАТРИМКА ПЕРЕД ЗАПУСКОМ ВІДТВОРЕННЯ
                        //ESP_LOGI(TAG, "⏳ 5. Затримка перед запуском відтворення (2 секунди)");
                        //vTaskDelay(pdMS_TO_TICKS(2000));
                        
                        // 🔧 ДОДАТКОВИЙ СБРОС АУДІО СИСТЕМИ ПЕРЕД ЗАПУСКОМ
                        ESP_LOGI(TAG, "🔄 6. Фінальний сброс аудіо системи");
                        
                        // 🧹 Гарантована очистка I2S (тепер вже зупиняє external input якщо потрібно)
                        cleanup_i2s_driver();  
                        vTaskDelay(pdMS_TO_TICKS(300));
                        
                        // 🔧 Перезапуск аудіочипа
                        if (g_board_handle) {
                            audio_hal_ctrl_codec(g_board_handle->audio_hal, 
                                                AUDIO_HAL_CODEC_MODE_DECODE, 
                                                AUDIO_HAL_CTRL_STOP);
                            vTaskDelay(pdMS_TO_TICKS(100));
                            
                            audio_hal_ctrl_codec(g_board_handle->audio_hal, 
                                                AUDIO_HAL_CODEC_MODE_DECODE, 
                                                AUDIO_HAL_CTRL_START);
                            vTaskDelay(pdMS_TO_TICKS(100));
                        }
                        
                        // 🚀 Запускаємо задачу відтворення
                        ESP_LOGI(TAG, "🚀 7. Запуск задачі відтворення");
                        
                        if (xTaskCreatePinnedToCore(player_task, "player_task", 16384, NULL, 5, &g_player_task_handle, 1) == pdPASS) {
                            ESP_LOGI(TAG, "✅ Задача відтворення успішно запущена (CPU1)");
                        } else {
                            ESP_LOGE(TAG, "❌ Не вдалося створити задачу відтворення");
                        }
                    } else {
                        ESP_LOGW(TAG, "⚠️ MP3 файли не знайдено на USB пристрої");
                    }
                    
                    // 📨 Відправка події по CAN
                    if (g_playlist_ready) {
                        ESP_LOGI(TAG, "📨 8. Відправка події підключення USB");
                        //send_can_event(EVENT_USB_CONNECTED, g_playlist_size, 0x00);
                    }
                    
                    // 🔓 Зняття захисту
                    g_processing_connect = false;
                    
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                    ESP_LOGI(TAG, "✅ [USB] ОБРОБКА ПІДКЛЮЧЕННЯ УСПІШНО ЗАВЕРШЕНА");
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                }

                else if (msg.id == APP_MODE_SWITCH) {
                    ESP_LOGI(TAG, "🔀 APP: Режим переключення (CAN mode)");

                    // Если сейчас не в режиме external, переключаемся на внешний вход
                    if (!g_mode_external) {
                        ESP_LOGI(TAG, "🔀 Переключаемся на внешний вход");
                        
                        // Сохраняем позицию текущего воспроизведения
                        if (g_player_task_handle != NULL) {
                            int cur_pos = g_last_saved_position;
                            save_playback_position(g_current_track, cur_pos);
                            // Остановим плеер (він сам почекає завершення)
                            stop_playback();
                        }
                        // Запускаем external input
                        if (start_external_input() == ESP_OK) {
                            g_mode_external = true;
                            // Зберігаємо вибір джерела як External
                            save_last_source_pref(1);
                        } else {
                            ESP_LOGW(TAG, "⚠️ Не удалось запустить внешний вход");
                        }
                    } else {
                        ESP_LOGI(TAG, "🔀 Возвращаемся к воспроизведению с флешки");
                        // Остановим external input
                        stop_external_input();
                        g_mode_external = false;
                        // Зберігаємо вибір джерела як USB
                        save_last_source_pref(0);

                        // Перевіряємо чи є підключені MSC пристрої
                        bool has_msc_device = false;
                        int available_slot = -1;
                        for (int i = 0; i < MAX_MSC_DEVICES; i++) {
                            if (msc_devices[i]) {
                                has_msc_device = true;
                                available_slot = i;
                                break;
                            }
                        }

                        // Якщо є пристрій але не змонтований - монтуємо
                        if (has_msc_device && !g_usb_mounted) {
                            ESP_LOGI(TAG, "🔌 MSC пристрій знайдено в слоті %d, монтуємо...", available_slot);
                            if (mount_usb_device(available_slot)) {
                                ESP_LOGI(TAG, "✅ USB успішно змонтовано");
                                // Створюємо плейлист якщо його немає
                                if (!g_playlist_ready) {
                                    create_playlist_from_usb();
                                }
                            }
                        }

                        // Если флешка доступна и есть playlist — запустим плеер и восстановим позицию
                        if (g_usb_mounted && g_playlist_ready) {
                            // Загрузим сохраненную позицию
                            load_playback_position(&g_saved_track_index, &g_saved_position_ms);
                            if (g_saved_position_ms > 0 && g_saved_track_index >= 0 && g_saved_track_index < g_playlist_size) {
                                g_current_track = g_saved_track_index;
                                g_restore_playback = true;
                            }
                            if (xTaskCreatePinnedToCore(player_task, "player_task", 16384, NULL, 5, &g_player_task_handle, 1) == pdPASS) {
                                ESP_LOGI(TAG, "✅ Задача відтворення успішно запущена після mode switch");
                            } else {
                                ESP_LOGE(TAG, "❌ Не вдалося створити задачу відтворення після mode switch");
                            }
                        } else {
                            ESP_LOGW(TAG, "⚠️ Флешка не доступна — відновлення неможливе (usb_mounted=%d, playlist_ready=%d)", 
                                     g_usb_mounted, g_playlist_ready);
                        }
                    }
                }
                
                // === BLE команда: Toggle Pause ===
                else if (msg.id == APP_TOGGLE_PAUSE) {
                    ESP_LOGI(TAG, "⏸️ BLE: Toggle pause/play");
                    if (g_player_task_handle != NULL) {
                        // Если сейчас есть задача плеера — отправим сигнал на паузу
                        if (g_is_paused) {
                            // Resume
                            g_is_paused = false;
                            ESP_LOGI(TAG, "▶️ Resuming playback via BLE");
                        } else {
                            // Pause
                            g_is_paused = true;
                            ESP_LOGI(TAG, "⏸️ Pausing playback via BLE");
                        }
                    } else {
                        ESP_LOGW(TAG, "⚠️ No player task running");
                    }
                }
                
                // === BLE команда: Play specific track ===
                else if (msg.id == APP_PLAY_TRACK) {
                    int track_idx = msg.data.int_param;
                    ESP_LOGI(TAG, "▶️ BLE: Play track %d", track_idx);
                    if (track_idx >= 0 && track_idx < g_playlist_size) {
                        g_current_track = track_idx;
                        g_is_paused = false;
                        g_play_specific_track = true;
                        g_skip_to_prev = false;
                        g_skip_to_next = true;   // force pipeline reload of selected track
                        if (g_player_task_handle != NULL) {
                            // Плеер должен заметить изменение g_current_track и переключиться
                            ESP_LOGI(TAG, "✅ Switched to track %d", track_idx);
                        }
                    } else {
                        ESP_LOGW(TAG, "⚠️ Invalid track index: %d (playlist size: %d)", track_idx, g_playlist_size);
                    }
                }
                
                // === BLE команда: Seek to position ===
                else if (msg.id == APP_SEEK_POSITION) {
                    int pos_ms = msg.data.int_param;
                    ESP_LOGI(TAG, "⏩ BLE: Seek to %d ms", pos_ms);
                    if (pos_ms >= 0) {
                        g_seek_position_ms = pos_ms;
                        ESP_LOGI(TAG, "✅ Seek requested to %d ms", pos_ms);
                    } else {
                        ESP_LOGW(TAG, "⚠️ Invalid seek position: %d", pos_ms);
                    }
                }
                            // === КРИТЕРІЙ 13: Обработка отключения USB ===
                else if (msg.id == APP_DEVICE_DISCONNECTED) {
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                    ESP_LOGI(TAG, "🔌 [USB] ПРИСТРІЙ ВІДКЛЮЧЕНО - ПОЧАТОК ОБРОБКИ");
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                    
                    // 🛡️ ГЛОБАЛЬНИЙ ЗАХИСТ ВІД ПОВТОРНОЇ ОБРОБКИ
                    if (g_processing_disconnect) {
                        ESP_LOGW(TAG, "⚠️ Відключення вже обробляється, пропускаємо...");
                        continue;
                    }
                    g_processing_disconnect = true;
                    
                    // 🔍 Знаходимо слот пристрою
                    int slot = find_slot_by_handle(msg.data.device_handle);
                    if (slot < 0) {
                        ESP_LOGW(TAG, "⚠️ Слот пристрою не знайдено, але продовжуємо обробку");
                    } else {
                        ESP_LOGI(TAG, "📌 Знайдено слот пристрою: %d", slot);
                    }
                    
                    // ⚡ ЕТАП 1: МИТТЄВА ЗУПИНКА ВІДТВОРЕННЯ
                    ESP_LOGI(TAG, "⏸️ 1. Миттєва зупинка відтворення");
                    
                    if (g_player_task_handle != NULL) {
                        // 🎯 Позначимо що треба зупинитись
                        g_skip_to_next = true;  // Це зупинить цикл в player_task
                        
                        // 🔇 НЕМАРНО зупиняємо аудіо-чип
                        if (g_board_handle) {
                            ESP_LOGI(TAG, "   🔇 Зупинка аудіо-чипу...");
                            audio_hal_ctrl_codec(g_board_handle->audio_hal, 
                                                AUDIO_HAL_CODEC_MODE_DECODE, 
                                                AUDIO_HAL_CTRL_STOP);
                            vTaskDelay(pdMS_TO_TICKS(50));
                        }
                        
                        // ⏳ Коротка затримка для зупинки потоку даних
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    
                        // ⚡ ЕТАП 2: ПОВНА ДЕІНІЦІАЛІЗАЦІЯ АУДІО СИСТЕМИ
                        ESP_LOGI(TAG, "🔄 2. Повна деініціалізація аудіо системи");

                        // 🧹 Деініціалізація I2S драйвера (НАЙВАЖЛИВІШЕ!)
                        //ESP_LOGI(TAG, "   🧹 Деініціалізація I2S драйвера...");
                        //cleanup_i2s_driver();  // <-- ИСПОЛЬЗУЙТЕ НОВУЮ ФУНКЦИЮ
                        //vTaskDelay(pdMS_TO_TICKS(200));

                        // 📊 Очистка всіх буферів
                        ESP_LOGI(TAG, "   🧹 Очистка аудіо буферів...");
                        flush_audio_buffers();  // <-- ИСПОЛЬЗУЙТЕ НОВУЮ ФУНКЦИЮ
                        vTaskDelay(pdMS_TO_TICKS(150));
                    
                    // ⚡ ЕТАП 3: ЗАКРИТТЯ ЗАДАЧІ ВІДТВОРЕННЯ
                    ESP_LOGI(TAG, "🗑️ 3. Закриття задачі відтворення");
                    
                    if (g_player_task_handle != NULL) {
                        ESP_LOGI(TAG, "   🎯 Видалення задачі player_task...");
                        TaskHandle_t task_to_delete = g_player_task_handle;
                        g_player_task_handle = NULL;
                        
                        // 🔥 СИЛОВЕ видалення задачі
                        vTaskDelete(task_to_delete);
                        
                        // ⏳ Затримка для гарантованого завершення
                        vTaskDelay(pdMS_TO_TICKS(300));
                        
                        // 🧹 Додаткова очистка після видалення задачі
                        //ESP_LOGI(TAG, "   🧹 Деініціалізація I2S драйвера...");
                        //cleanup_i2s_driver();  // <-- ИСПОЛЬЗУЙТЕ НОВУЮ ФУНКЦИЮ
                        //vTaskDelay(pdMS_TO_TICKS(200));

                        // 📊 Очистка всіх буферів
                        //ESP_LOGI(TAG, "   🧹 Очистка аудіо буферів...");
                        //flush_audio_buffers();  // <-- ИСПОЛЬЗУЙТЕ НОВУЮ ФУНКЦИЮ
                        //vTaskDelay(pdMS_TO_TICKS(150));
                    }
                    
                    // ⚡ ЕТАП 4: ДЕМОНТУВАННЯ USB
                    ESP_LOGI(TAG, "🔌 4. Демонтування USB пристрою");
                    
                    if (g_usb_mounted) {
                        ESP_LOGI(TAG, "   📤 Демонтування зі слота %d...", g_current_mount_slot);
                        
                        // 💾 Збереження позиції ПЕРЕД демонтуванням
                        if (g_playlist_ready && g_current_track >= 0 && g_last_saved_position > 0) {
                            ESP_LOGI(TAG, "   💾 Автозбереження позиції: трек %d, %d мс", 
                                    g_current_track, g_last_saved_position);
                            save_playback_position(g_current_track, g_last_saved_position);
                        }
                        
                        g_usb_mounted = false;
                        g_current_mount_slot = -1;
                        
                        // ⏳ Затримка для стабілізації файлової системи
                        vTaskDelay(pdMS_TO_TICKS(500));
                    }
                    
                    // ⚡ ЕТАП 5: ЗВІЛЬНЕННЯ РЕСУРСІВ MSC
                    ESP_LOGI(TAG, "🗑️ 5. Звільнення ресурсів MSC пристрою");
                    
                    if (slot >= 0 && slot < MAX_MSC_DEVICES && msc_devices[slot]) {
                        ESP_LOGI(TAG, "   🧹 Звільнення слота %d...", slot);
                        
                        if (msc_devices[slot]->vfs_handle) {
                            ESP_ERROR_CHECK(msc_host_vfs_unregister(msc_devices[slot]->vfs_handle));
                        }
                        if (msc_devices[slot]->msc_device) {
                            ESP_ERROR_CHECK(msc_host_uninstall_device(msc_devices[slot]->msc_device));
                        }
                        
                        free(msc_devices[slot]);
                        msc_devices[slot] = NULL;
                        
                        ESP_LOGI(TAG, "   ✅ Слот %d звільнено", slot);
                    }
                    
                    // ⏳ КРИТИЧНА ЗАТРИМКА ПІСЛЯ ВІДКЛЮЧЕННЯ
                    ESP_LOGI(TAG, "⏳ 6. Критична затримка для стабілізації (2 секунди)");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    
                    // ⚡ ЕТАП 6: ОЧИЩЕННЯ ГЛОБАЛЬНИХ ЗМІННИХ
                    ESP_LOGI(TAG, "🧹 7. Очищення глобальних змінних");
                    
                    // 🗑️ Очищення плейлиста
                    clear_playlist();
                    
                    // 📝 Скидання всіх флагів стану
                    g_skip_to_next = false;
                    g_skip_to_prev = false;
                    g_is_paused = false;
                    
                    // 💾 Очищення збереженої позиції (флешка вилучена)
                    ESP_LOGI(TAG, "   🗑️ Очищення збереженої позиції...");
                    clear_playback_position();
                    
                    // 📊 Скидання позиційних змінних
                    g_last_saved_position = 0;
                    g_saved_track_index = -1;
                    g_saved_position_ms = 0;
                    g_restore_playback = false;
                    g_current_track = 0;
                    
                    // ⚡ ЕТАП 7: ПОВТОРНА ІНІЦІАЛІЗАЦІЯ АУДІОЧИПА
                    ESP_LOGI(TAG, "🔊 8. Повторна ініціалізація аудіочипа");
                    
                    if (g_board_handle) {
                        ESP_LOGI(TAG, "   🔧 Перезапуск аудіо-чипу...");
                        
                        // 🔄 Повна переініціалізація
                        audio_hal_ctrl_codec(g_board_handle->audio_hal, 
                                            AUDIO_HAL_CODEC_MODE_DECODE, 
                                            AUDIO_HAL_CTRL_STOP);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        
                        audio_hal_ctrl_codec(g_board_handle->audio_hal, 
                                            AUDIO_HAL_CODEC_MODE_DECODE, 
                                            AUDIO_HAL_CTRL_START);
                        vTaskDelay(pdMS_TO_TICKS(100));
                        
                        // 🔊 Встановлення гучності за замовчуванням
                        audio_hal_set_volume(g_board_handle->audio_hal, g_volume);
                    }
                    
                    // 📨 Відправка CAN події про відключення USB
                    ESP_LOGI(TAG, "📨 9. Відправка події по CAN");
                    //send_can_event(EVENT_USB_DISCONNECTED, 0x00, 0x00);
                    
                    // 🔓 Зняття захисту
                    g_processing_disconnect = false;
                    
                    // ⏳ ФІНАЛЬНА ЗАТРИМКА
                    vTaskDelay(pdMS_TO_TICKS(500));
                    
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                    ESP_LOGI(TAG, "✅ [USB] ОБРОБКА ВІДКЛЮЧЕННЯ УСПІШНО ЗАВЕРШЕНА");
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                    // Якщо флешка відсутня — автоматично переключаємось на зовнішній вхід
                    if (!g_mode_external && !g_usb_mounted) {
                        ESP_LOGI(TAG, "ℹ️ USB відсутній — автоматичне включення зовнішнього входу");
                        if (start_external_input() == ESP_OK) {
                            g_mode_external = true;
                            // Зберігаємо перевагу джерела як External, щоб після перезапуску лишитися на вході
                            save_last_source_pref(1);
                        } else {
                            ESP_LOGW(TAG, "⚠️ Не вдалося автоматично запустити зовнішній вхід");
                        }
                    }
                }
            // === КРИТЕРІЙ 15: Обробка наступного треку - перевір дублі ===
            else if (msg.id == APP_NEXT_TRACK) {
                ESP_LOGI(TAG, "⏭️ Команда: наступний трек");
                
                // Якщо в режимі external input - відправляємо по UART на зовнішній пристрій
                if (g_mode_external) {
                    ESP_LOGI(TAG, "🔀 Режим external - відправка по UART");
                    if (send_i2c_command(LINK_CMD_NEXT_TRACK) != ESP_OK) {
                        ESP_LOGW(TAG, "⚠️ UART: не вдалося відправити команду NEXT_TRACK");
                    }
                } else {
                    // Иначе - управляем локальным плеером
                    play_next_track();
                }
            }
            // === КРИТЕРІЙ 16: Обробка попереднього треку - перевір дублі ===
            else if (msg.id == APP_PREV_TRACK) {
                ESP_LOGI(TAG, "⏮️ Команда: попередній трек");

                // Якщо в режимі external input - відправляємо по UART на зовнішній пристрій
                if (g_mode_external) {
                    ESP_LOGI(TAG, "🔀 Режим external - відправка по UART");
                    if (send_i2c_command(LINK_CMD_PREV_TRACK) != ESP_OK) {
                        ESP_LOGW(TAG, "⚠️ UART: не вдалося відправити команду PREV_TRACK");
                    }
                } else {
                    // Иначе - управляем локальным плеером
                    play_prev_track();
                }
            }
            else if (msg.id == APP_WIFI_SWITCH_STA) {
                ESP_LOGI(TAG, "🌐 Обробка: перемикання Wi-Fi у STA (DHCP)");
                #ifdef WIFI_STA_SSID
                wifi_can_switch_to_sta(WIFI_STA_SSID, WIFI_STA_PASSWORD);
                #else
                wifi_can_switch_to_sta(WIFI_SSID, WIFI_PASSWORD);
                #endif
                save_wifi_mode_pref(1);
            }
            else if (msg.id == APP_WIFI_SWITCH_AP) {
                ESP_LOGI(TAG, "🌐 Обробка: перемикання Wi-Fi у AP");
                char ap_ssid[32] = "";
                char ap_password[64] = "";
                load_ap_credentials(ap_ssid, sizeof(ap_ssid), ap_password, sizeof(ap_password));
                wifi_can_switch_to_ap(ap_ssid, ap_password);
                save_wifi_mode_pref(0);
            }
            // === КРИТЕРІЙ 17: Завершення роботи - має бути одна точка виходу ===
            else if (msg.id == APP_QUIT) {
                ESP_LOGI(TAG, "🛑 Завершення роботи...");
                
                // ⏸️ Зупиняємо відтворення
                stop_playback();
                
                // 🔌 Демонтуємо USB
                unmount_usb_device();
                
                // 🗑️ Звільняємо всі пристрої
                free_all_msc_devices();
                
                // 🗑️ Очищаємо плейлист
                clear_playlist();
                
                // 🧹 Деініціалізація MSC
                msc_host_uninstall();
                
                // 💾 Збереження позиції перед виходом
                if (g_playlist_ready && g_current_track >= 0) {
                    ESP_LOGI(TAG, "💾 Збереження позиції перед завершенням:");
                    ESP_LOGI(TAG, "   🎵 Трек: %d, ⏱️ Позиція: %d мс", g_current_track, g_last_saved_position);
                    save_playback_position(g_current_track, g_last_saved_position);
                }

                // 🧹 Очищення ресурсів
                ESP_LOGI(TAG, "🧹 Очищення ресурсів...");
                gpio_isr_handler_remove(APP_QUIT_PIN);
                gpio_uninstall_isr_service();
                vQueueDelete(app_queue);
                ESP_LOGI(TAG, "👋 Завершено");

                break;
            }
        }
    }
}

/**
 * Задача для читання команд з терміналу (UART)
 */

