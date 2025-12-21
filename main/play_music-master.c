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
#include "esp_sleep.h"
#include "driver/uart.h"
#include "filter_resample.h"  // Для динамічної підстройки частоти
#include "wifi_can_sniffer.h"  // 📡 Wi-Fi CAN-Sniffer компонент
#include "web_ota.h"
#include "uart_ota_protocol.h"  // UART OTA протокол
#include "player_version.h"     // Версія основного додатку

#pragma GCC diagnostic ignored "-Wformat-truncation"

static const char *TAG = "Player";

#define MNT_PATH         "/ROOT"
#define APP_QUIT_PIN     GPIO_NUM_0
#define BUFFER_SIZE      4096
#define MAX_MSC_DEVICES  CONFIG_FATFS_VOLUME_COUNT
#define MAX_PLAYLIST_SIZE 1000
#define NVS_NAMESPACE    "audio_player"
// Keys in NVS
#define NVS_KEY_LAST_SOURCE "last_source" // 0 = USB/flash, 1 = external input
#define NVS_KEY_WIFI_MODE  "wifi_mode"    // 0 = AP, 1 = STA
#define NVS_KEY_STA_SSID   "sta_ssid"     // STA SSID
#define NVS_KEY_STA_PASS   "sta_pass"     // STA password
#define NVS_KEY_AP_SSID    "ap_ssid"      // AP SSID
#define NVS_KEY_AP_PASS    "ap_pass"      // AP password
#define NVS_KEY_PLAYER_DATE "player_date" // Версія player - дата
#define NVS_KEY_PLAYER_TIME "player_time" // Версія player - час
// NVS key for sniffer enabled flag
#define NVS_KEY_SNIFFER_ENABLED "sniffer_enabled"
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
#define CAN_ALERT_FLAGS  TWAI_ALERT_ALL
#define CAN_DEFAULT_BITRATE 125  // Default 125 kbps

// Глобальна скорість CAN (можна змінювати)
static uint16_t g_can_bitrate = CAN_DEFAULT_BITRATE;

// Получить конфиг timing по скорости - используем статический кеш
static twai_timing_config_t get_can_timing_config(uint16_t bitrate_kbps)
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

// 🌐 Wi-Fi CAN-Sniffer конфигурация
#define WIFI_SSID        "ESP32-CAN-Sniffer"
#define WIFI_PASSWORD    "12345678"

// AP mode defaults (can be configured via web interface)
#define WIFI_AP_SSID     "ESP32-MusicPlayer"
#define WIFI_AP_PASSWORD "music1234"
// Окремі налаштування для підключення як клієнт (STA)
// Заміни на свої значення мережі, до якої треба підключатися
#ifndef WIFI_STA_SSID
#define WIFI_STA_SSID     "House"
#endif
#ifndef WIFI_STA_PASSWORD
#define WIFI_STA_PASSWORD "P!752Drx!r"
#endif
#define PC_IP_ADDR       "192.168.4.255"  // 🖥️ Broadcast - все устройства получат пакеты
#define UDP_PORT         0xAA55             // Порт как в оригинальном CAN-Sniffer

// UART for slave commands
#define MASTER_UART_PORT           UART_NUM_1
#define MASTER_UART_TX_PIN         8
#define MASTER_UART_RX_PIN         9
#define MASTER_UART_BAUD           115200
#define MASTER_UART_RX_BUF         256
#define MASTER_UART_TX_BUF         256

// Команди для slave (використовуємо ті самі значення з протоколу)
#define LINK_CMD_NEXT_TRACK        UART_CMD_NEXT_TRACK  // 0x04
#define LINK_CMD_PREV_TRACK        UART_CMD_PREV_TRACK  // 0x05








static void check_i2s_input_signal(void);
static void stop_external_input(void);
// Callbacks from WiFi CAN sniffer (defined later)
static void wifi_can_on_message_callback(const can_sniffer_frame_t *frame);
static void wifi_can_on_control_callback(uint8_t cmd, const uint8_t *data, uint8_t len);

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
static int g_volume = 10;  // Громкость от 0 до 100, по умолчанию 70%
static audio_board_handle_t g_board_handle = NULL;  // Обработчик аудиочипа



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


static void debug_audio_state(audio_element_handle_t el, const char *name);

static bool g_can_sleep_mode = false;
static uint32_t g_last_can_activity_time = 0;
static uint32_t g_sleep_start_time = 0;
#define CAN_SLEEP_TIMEOUT_MS   1000000  // 😴 10 секунд без активності - спати
#define CAN_WAKEUP_DELAY_MS    5000   // ⏰ 5 секунд після пробудження перед відтворенням
#define CAN_SLEEP_CHECK_PERIOD 1000   // 🔍 Перевіряти кожну секунду

// Пин-индикатор сна: при переходе в сон — ставим LOW, при пробуждении — HIGH
#ifndef SLEEP_IND_PIN
#define SLEEP_IND_PIN 21
#endif

// 🌙 Sleep settings structure
typedef struct {
    int sleep_mode;        // 0=disabled, 1=light sleep, 2=modem sleep, 3=deep sleep
    int idle_timeout_sec;  // idle timeout in seconds (minimum 10)
} sleep_settings_t;

// 🌙 Global sleep settings
static sleep_settings_t g_sleep_settings = {
    .sleep_mode = 0,       // default: disabled
    .idle_timeout_sec = 30 // default: 30 seconds
};

// 🌙 Last system activity time (for idle detection)
static uint32_t g_last_system_activity_time = 0;



// 📋 Глобальні змінні для плейлиста
static char **g_playlist = NULL;
static int g_playlist_size = 0;
static int g_current_track = 0;
static bool g_playlist_ready = false;
static bool g_decoder_error = false;  // 🔴 Прапор помилки декодера MP3
static bool g_skip_to_next = false;
static bool g_skip_to_prev = false;  // ⏮️ Додано для попереднього треку
static bool g_play_specific_track = false;  // 🎯 Чи відтворювати конкретний трек з веб
static bool g_is_paused = false;     // ⏸️ Додано для паузи
// Збережений стан сніфера (true=enabled)
static bool g_sniffer_enabled = false;
static TaskHandle_t g_player_task_handle = NULL;
static int64_t g_stall_last_ts_ms = 0;    // ⏳ Час останньої перевірки прогресу
static size_t g_stall_last_bytes = 0;     // 📊 Останній прогрес у байтах
static int32_t g_seek_position_ms = -1;   // 🎯 Позиція для перемотки через веб
static int32_t g_current_track_duration_ms = 180000; // Тривалість поточного треку

// 📡 Wi-Fi CAN-Sniffer статистика
static uint16_t g_can_frames_per_second = 0;
static uint16_t g_can_bytes_per_second = 0;
static uint32_t g_last_stat_time = 0;

// 🔌 Глобальні змінні для USB управління
static bool g_usb_mounted = false;
static int g_current_mount_slot = -1;

// Режим внешнего входа
static volatile bool g_mode_external = false; // true = воспроизводим внешний I2S вход
static int g_last_source_pref = 0; // 0 = USB(default), 1 = external
static audio_element_handle_t g_external_writer = NULL;
static TaskHandle_t g_i2s_passthrough_task_handle = NULL;

// This task reads from the next available I2S port (slave) and writes directly to the g_external_writer element (I2S0 master)
static void i2s_passthrough_task(void *pvParameters)
{
    (void)pvParameters;

    // Гібридний підхід: низькорівневий I2S slave для входу + ESP-ADF writer для виходу
    
    if (g_verbose_logs && g_player_logs) ESP_LOGI(TAG, "🎵 Ініціалізація I2S slave для входу...");
    
    // Включаємо pull-up на GPIO 40 (DIN) для стабільності
    gpio_set_pull_mode(EXT_I2S_DIN_PIN, GPIO_PULLUP_ONLY);
    if (g_verbose_logs && g_player_logs) ESP_LOGI(TAG, "✅ Pull-up включена на GPIO %d (DIN)", EXT_I2S_DIN_PIN);
    
    // 1. Низькорівневий I2S slave для читання (отримує clock ззовні)
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    rx_cfg.dma_desc_num = 6;        // збільшуємо кількість DMA дескрипторів
    rx_cfg.dma_frame_num = 512;     // збільшуємо розмір кадру
    
    i2s_chan_handle_t rx_handle = NULL;
    esp_err_t ret = i2s_new_channel(&rx_cfg, NULL, &rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ RX error: %s", esp_err_to_name(ret));
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
        ESP_LOGE(TAG, "❌ RX init: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        g_i2s_passthrough_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ RX enable: %s", esp_err_to_name(ret));
        i2s_del_channel(rx_handle);
        g_i2s_passthrough_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }
    
    if (g_verbose_logs && g_player_logs) {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════");
        ESP_LOGI(TAG, "✅ RX SLAVE ГОТОВИЙ (I2S_NUM_1)");
        ESP_LOGI(TAG, "📍 GPIO: BCLK=%d, WS=%d, DIN=%d", EXT_I2S_BCLK_PIN, EXT_I2S_WS_PIN, EXT_I2S_DIN_PIN);
        ESP_LOGI(TAG, "🔊 Формат: 44.1kHz, 16bit, Stereo");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    }

    // 2. ESP-ADF I2S stream для виходу (master на PCM5102)
    if (g_verbose_logs && g_player_logs) ESP_LOGI(TAG, "🎵 Створення ESP-ADF I2S writer для виходу...");
    
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
        ESP_LOGE(TAG, "❌ Writer error");
        i2s_del_channel(rx_handle);
        g_i2s_passthrough_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // Запускаємо writer
    audio_element_run(i2s_writer);
    if (g_verbose_logs && g_player_logs) {
        ESP_LOGI(TAG, "═══════════════════════════════════════════════");
        ESP_LOGI(TAG, "✅ TX MASTER ГОТОВИЙ (I2S_NUM_0)");
        ESP_LOGI(TAG, "📍 GPIO: BCLK=17, WS=18, DOUT=16");
        ESP_LOGI(TAG, "🔊 Формат: 44.1kHz, 16bit, Stereo");
        ESP_LOGI(TAG, "🎯 Вихід: PCM5102 DAC");
        ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    }

    // 3. Основний цикл: читаємо з RX slave, пишемо в writer
    if (g_verbose_logs && g_player_logs) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════════════════╗");
        ESP_LOGI(TAG, "║  🎵 PASSTHROUGH АКТИВНИЙ                      ║");
        ESP_LOGI(TAG, "║  RX: I2S1 (slave) → TX: I2S0 (master)        ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════════════════╝");
    }
    if (g_verbose_logs && g_player_logs) ESP_LOGI(TAG, "");
    
    // Менший буфер для меншої затримки, але частіші передачі
    uint8_t *buffer = malloc(2048);
    if (!buffer) {
        ESP_LOGE(TAG, "❌ Malloc failed");
        audio_element_deinit(i2s_writer);
        i2s_del_channel(rx_handle);
        g_i2s_passthrough_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    size_t bytes_read;
    uint32_t total_read = 0, total_write = 0;
    bool first_data = false;

    while (g_mode_external) {
        ret = i2s_channel_read(rx_handle, buffer, 2048, &bytes_read, pdMS_TO_TICKS(100));
        
        if (ret == ESP_OK && bytes_read > 0) {
            if (!first_data) {
                if (g_verbose_logs) {
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                    ESP_LOGI(TAG, "✅ ПЕРШІ ДАНІ ОТРИМАНО З SLAVE!");
                    ESP_LOGI(TAG, "📊 Розмір блоку: %d байт", bytes_read);
                    ESP_LOGI(TAG, "🎯 I2S порт: I2S_NUM_1 (slave mode)");
                    ESP_LOGI(TAG, "📍 GPIO: BCLK=%d, WS=%d, DIN=%d", EXT_I2S_BCLK_PIN, EXT_I2S_WS_PIN, EXT_I2S_DIN_PIN);
                    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                }
                first_data = true;
            }
            total_read += bytes_read;
            
            int written = audio_element_output(i2s_writer, (char*)buffer, bytes_read);
            if (written > 0) {
                total_write += written;
            }
        }

        // Статистика (відключено для продуктивності)
        /*
        if ((xTaskGetTickCount() - log_time) > pdMS_TO_TICKS(3000)) {
            ESP_LOGI(TAG, "═══════════════════════════════════════════════");
            ESP_LOGI(TAG, "🎵 PASSTHROUGH СТАТУС (I2S1→I2S0)");
            ESP_LOGI(TAG, "───────────────────────────────────────────────");
            ESP_LOGI(TAG, "📥 Прочитано:  %u байт", total_read);
            ESP_LOGI(TAG, "📤 Відправлено: %u байт", total_write);
            ESP_LOGI(TAG, "⚡ Швидкість:  %.2f кБ/с", (float)total_read / 3.0 / 1024.0);
            if (total_read != total_write) {
                ESP_LOGW(TAG, "⚠️ РОЗБІЖНІСТЬ: read≠write (%d байт)", (int)(total_read - total_write));
            } else {
                ESP_LOGI(TAG, "✅ Синхронізація: OK");
            }
            ESP_LOGI(TAG, "═══════════════════════════════════════════════");
            total_read = 0;
            total_write = 0;
            log_time = xTaskGetTickCount();
        }
        */
    }

    free(buffer);
    
    if (g_verbose_logs) {
        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════════════════╗");
        ESP_LOGI(TAG, "║  🛑 ЗУПИНКА PASSTHROUGH                       ║");
        ESP_LOGI(TAG, "╚═══════════════════════════════════════════════╝");
    }
    
    audio_element_stop(i2s_writer);
    audio_element_deinit(i2s_writer);
    if (g_verbose_logs) ESP_LOGI(TAG, "✅ I2S writer зупинено");
    
    i2s_channel_disable(rx_handle);
    i2s_del_channel(rx_handle);
    if (g_verbose_logs) ESP_LOGI(TAG, "✅ I2S slave зупинено");
    
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    ESP_LOGI(TAG, "✅ PASSTHROUGH ПОВНІСТЮ ЗУПИНЕНО");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");

    g_i2s_passthrough_task_handle = NULL;
    vTaskDelete(NULL);
}

// 💾 Змінні для збереження позиції
static int32_t g_saved_track_index = -1;
static int32_t g_saved_position_ms = 0;
static bool g_restore_playback = false;
static bool g_playback_was_active_before_sleep = false; // 🎵 Чи був активний плеєр перед сном
static uint32_t g_last_position_save_time = 0;
static int g_last_saved_position = 0;
static bool g_is_reconnecting = false;

static bool g_processing_disconnect = false;
static bool g_processing_connect = false;
static SemaphoreHandle_t g_usb_mutex = NULL;
static i2s_chan_handle_t g_i2s_tx_handle = NULL;

/**
 * 📝 Запис про пристрій MSC
 */
typedef struct {
    uint8_t usb_addr;
    msc_host_device_handle_t msc_device;
    msc_host_vfs_handle_t vfs_handle;
} msc_dev_entry_t;

static msc_dev_entry_t *msc_devices[MAX_MSC_DEVICES] = {0};

/**
 * 📨 Черга програми та ідентифікатори її повідомлень
 */
static QueueHandle_t app_queue;
typedef struct {
    enum {
        APP_QUIT,                // 🛑 Завершення роботи
        APP_DEVICE_CONNECTED,    // 💾 Пристрій підключено
        APP_DEVICE_DISCONNECTED, // 📤 Пристрій відключено
        APP_MODE_SWITCH,         // 🔀 Переключение режима (flash <-> external input)
        APP_NEXT_TRACK,          // ⏭️ Наступний трек
        APP_PREV_TRACK,          // ⏮️ Попередній трек
        APP_SAVE_POSITION,       // 💾 Зберегти позицію
        APP_WIFI_SWITCH_STA,     // 🌐 Перемкнути Wi-Fi у STA (DHCP)
        APP_WIFI_SWITCH_AP       // 🌐 Перемкнути Wi-Fi у AP
    } id;
    union {
        uint8_t new_dev_address;
        msc_host_device_handle_t device_handle;
    } data;
} app_message_t;

static void enter_deep_sleep(void);
// 📋 Прототипи функцій
static void unmount_usb_device(void);
static bool mount_usb_device(int slot);
static void player_task(void *pvParam);
static void create_playlist_from_usb(void);
static void find_mp3_files_recursive(const char *path);
static void add_to_playlist(const char *file_path);
static void clear_playlist(void);
static void stop_playback(void);
static void play_next_track(void);
static void play_prev_track(void);  // ⏮️ Додано прототип

// 🔉 ДОБАВЬТЕ ЭТУ СТРОЧКУ:
static void play_test_tone(audio_element_handle_t i2s_writer, int duration_ms);

static esp_err_t save_playback_position(int32_t track_index, int32_t position_ms);
static esp_err_t load_playback_position(int32_t *track_index, int32_t *position_ms);
static esp_err_t clear_playback_position(void);
static esp_err_t init_nvs(void);
static esp_err_t save_last_source_pref(int source);
static esp_err_t load_last_source_pref(int *source);
static esp_err_t load_wifi_mode_pref(int *mode);
static esp_err_t save_sta_credentials(const char *ssid, const char *password);
static esp_err_t load_sta_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len);
static int get_current_position(audio_element_handle_t fatfs_stream_reader);
static esp_err_t set_playback_position(audio_element_handle_t fatfs_stream_reader, int position_ms);
static esp_err_t audio_event_cb(audio_element_handle_t el, audio_event_iface_msg_t *event, void *ctx);
static int find_free_slot(void);
static esp_err_t allocate_new_msc_device(const app_message_t *msg, int *out_slot);
static int find_slot_by_handle(msc_host_device_handle_t handle);
static void free_msc_device(int slot);
static void free_all_msc_devices(void);
static void gpio_cb(void *arg);
static int8_t find_usb_addr_by_handle(msc_host_device_handle_t handle);
static void msc_event_cb(const msc_host_event_t *event, void *arg);
static void usb_task(void *args);
static void terminal_commands_task(void *arg);

// 📡 CAN функції
static esp_err_t init_can(void);
static void can_task(void *arg);
static void send_can_status(void);
static void process_can_message(twai_message_t *message);
static void send_can_event(uint8_t event_type, uint8_t data1, uint8_t data2);

// 🌐 WiFi функції
static void sta_connection_task(void *arg);

/**
 * 🗃️ Ініціалізація NVS
 */
static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

// 💾 Збереження останнього вибраного джерела (USB або зовнішній вхід)
static esp_err_t save_last_source_pref(int source)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "💾 save_last_source_pref: Помилка відкриття NVS: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i32(nvs_handle, NVS_KEY_LAST_SOURCE, source);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "💾 Збережено last_source=%d (0=USB,1=EXT)", source);
    } else {
        ESP_LOGE(TAG, "❌ Не вдалося зберегти last_source: %s", esp_err_to_name(err));
    }
    return err;
}

// 📥 Завантаження останнього вибраного джерела
static esp_err_t load_last_source_pref(int *source)
{
    if (!source) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ℹ️ load_last_source_pref: NVS не відкрито (використаємо дефолт USB): %s", esp_err_to_name(err));
        *source = 0;
        return err;
    }
    int32_t val = 0;
    err = nvs_get_i32(nvs_handle, NVS_KEY_LAST_SOURCE, &val);
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        *source = (val != 0) ? 1 : 0;
        ESP_LOGI(TAG, "📥 Завантажено last_source=%d (0=USB,1=EXT)", *source);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *source = 0; // default USB
        ESP_LOGI(TAG, "📥 Ключ last_source відсутній — дефолт USB");
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG, "⚠️ Неможливо отримати last_source: %s (дефолт USB)", esp_err_to_name(err));
        *source = 0;
    }
    return err;
}

// 💾 Збереження останнього Wi-Fi режиму (AP або STA)
static esp_err_t save_wifi_mode_pref(int mode)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(nvs_handle, NVS_KEY_WIFI_MODE, mode);
    if (err == ESP_OK) err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "💾 Збережено wifi_mode=%d (0=AP,1=STA)", mode);
    }
    return err;
}

// 💾 Збереження стану Wi-Fi CAN-Sniffer (1=enabled,0=disabled)
static esp_err_t save_sniffer_enabled(int enabled)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(nvs_handle, NVS_KEY_SNIFFER_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "💾 Збережено sniffer_enabled=%d", enabled ? 1 : 0);
    return err;
}

// 📥 Завантаження стану Wi-Fi CAN-Sniffer
static esp_err_t load_sniffer_enabled(int *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) { *enabled = 0; return err; }
    int32_t val = 0;
    err = nvs_get_i32(nvs_handle, NVS_KEY_SNIFFER_ENABLED, &val);
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        *enabled = (val != 0) ? 1 : 0;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *enabled = 0; // default disabled
        err = ESP_OK;
    } else {
        *enabled = 0;
    }
    ESP_LOGI(TAG, "📥 Завантажено sniffer_enabled=%d", *enabled);
    return err;
}

// 📥 Завантаження останнього Wi-Fi режиму
static esp_err_t load_wifi_mode_pref(int *mode)
{
    if (!mode) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) { *mode = 0; return err; }
    int32_t val = 0;
    err = nvs_get_i32(nvs_handle, NVS_KEY_WIFI_MODE, &val);
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        *mode = (val != 0) ? 1 : 0;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *mode = 0; // default AP
        err = ESP_OK;
    } else {
        *mode = 0;
    }
    ESP_LOGI(TAG, "📥 Завантажено wifi_mode=%d (0=AP,1=STA)", *mode);
    return err;
}

// 💾 Збереження STA credentials
static esp_err_t save_sta_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_str(nvs_handle, NVS_KEY_STA_SSID, ssid);
    if (err != ESP_OK) goto cleanup;
    
    err = nvs_set_str(nvs_handle, NVS_KEY_STA_PASS, password);
    if (err != ESP_OK) goto cleanup;
    
    err = nvs_commit(nvs_handle);
    
cleanup:
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "💾 Збережено STA credentials: SSID='%s'", ssid);
    }
    return err;
}

// 📥 Завантаження STA credentials
static esp_err_t load_sta_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        // Use defaults
        strncpy(ssid, WIFI_STA_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
        strncpy(password, WIFI_STA_PASSWORD, pass_len - 1);
        password[pass_len - 1] = '\0';
        return ESP_OK;
    }
    
    size_t len = ssid_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_STA_SSID, ssid, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(ssid, WIFI_STA_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    } else if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    len = pass_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_STA_PASS, password, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(password, WIFI_STA_PASSWORD, pass_len - 1);
        password[pass_len - 1] = '\0';
        err = ESP_OK;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "📥 Завантажено STA credentials: SSID='%s'", ssid);
    return err;
}

// 💾 Збереження AP credentials
static esp_err_t save_ap_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, ssid);
    if (err != ESP_OK) goto cleanup;
    
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_PASS, password);
    if (err != ESP_OK) goto cleanup;
    
    err = nvs_commit(nvs_handle);
    
cleanup:
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "💾 Збережено AP credentials: SSID='%s'", ssid);
    }
    return err;
}

// 📥 Завантаження AP credentials
static esp_err_t load_ap_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        // Use defaults
        strncpy(ssid, WIFI_AP_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
        strncpy(password, WIFI_AP_PASSWORD, pass_len - 1);
        password[pass_len - 1] = '\0';
        return ESP_OK;
    }
    
    size_t len = ssid_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, ssid, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(ssid, WIFI_AP_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    } else if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    len = pass_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_PASS, password, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(password, WIFI_AP_PASSWORD, pass_len - 1);
        password[pass_len - 1] = '\0';
        err = ESP_OK;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "📥 Завантажено AP credentials: SSID='%s'", ssid);
    return err;
}

static esp_err_t try_wifi_connection_with_fallback(void)
{
    int wifi_mode_pref = 0; // 0=AP, 1=STA
    load_wifi_mode_pref(&wifi_mode_pref);
    
    if (wifi_mode_pref == 1) {
        // Спробувати STA в фоновому режимі
        char sta_ssid[32] = "";
        char sta_password[64] = "";
        load_sta_credentials(sta_ssid, sizeof(sta_ssid), sta_password, sizeof(sta_password));
        
        if (strlen(sta_ssid) > 0) {
            ESP_LOGI(TAG, "🌐 Запуск фонового підключення до STA: %s", sta_ssid);
            
            // Запускаємо окрему задачу для STA підключення
            if (xTaskCreatePinnedToCore(sta_connection_task, "sta_connect", 4096, NULL, 1, NULL, 0) == pdPASS) {
                ESP_LOGI(TAG, "✅ Задача STA підключення запущена");
            } else {
                ESP_LOGE(TAG, "❌ Не вдалося створити задачу STA підключення");
                // Fallback to AP
                goto fallback_to_ap;
            }
            return ESP_OK; // Не чекаємо, повертаємося одразу
        } else {
            ESP_LOGW(TAG, "⚠️ STA credentials не налаштовані");
        }
    }
    
fallback_to_ap:
    // Fallback to AP
    ESP_LOGI(TAG, "📶 Запуск AP режиму");
    char ap_ssid[32] = "";
    char ap_password[64] = "";
    load_ap_credentials(ap_ssid, sizeof(ap_ssid), ap_password, sizeof(ap_password));
    
    esp_err_t ret = wifi_can_switch_to_ap(ap_ssid, ap_password);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ AP запущено: %s", ap_ssid);
    }
    return ret;
}

// 🌐 Отримати поточні Wi-Fi налаштування (для веб-інтерфейсу)
esp_err_t get_wifi_settings(char *sta_ssid, size_t sta_ssid_len, char *sta_password, size_t sta_pass_len, 
                           char *ap_ssid, size_t ap_ssid_len, char *ap_password, size_t ap_pass_len, int *mode)
{
    esp_err_t err = load_wifi_mode_pref(mode);
    if (err != ESP_OK) return err;
    
    err = load_sta_credentials(sta_ssid, sta_ssid_len, sta_password, sta_pass_len);
    if (err != ESP_OK) return err;
    
    return load_ap_credentials(ap_ssid, ap_ssid_len, ap_password, ap_pass_len);
}

// 🌐 Зберегти Wi-Fi налаштування (для веб-інтерфейсу)
esp_err_t set_wifi_settings(const char *sta_ssid, const char *sta_password, 
                           const char *ap_ssid, const char *ap_password, int mode)
{
    esp_err_t err = save_wifi_mode_pref(mode);
    if (err != ESP_OK) return err;
    
    err = save_sta_credentials(sta_ssid, sta_password);
    if (err != ESP_OK) return err;
    
    return save_ap_credentials(ap_ssid, ap_password);
}

// 🌙 Forward declarations for sleep functions
esp_err_t load_sleep_settings(sleep_settings_t *settings);
esp_err_t save_sleep_settings(const sleep_settings_t *settings);

// 🌙 Отримати налаштування сну (для веб-інтерфейсу)
esp_err_t get_sleep_settings(int *sleep_mode, int *idle_timeout_sec)
{
    if (!sleep_mode || !idle_timeout_sec) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *sleep_mode = g_sleep_settings.sleep_mode;
    *idle_timeout_sec = g_sleep_settings.idle_timeout_sec;
    return ESP_OK;
}

// 🌙 Зберегти налаштування сну (для веб-інтерфейсу)
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
    
    ESP_LOGI(TAG, "Sleep settings updated: mode=%d, timeout=%d sec", 
             sleep_mode, idle_timeout_sec);
    
    return ESP_OK;
}

/**
 * 🌙 Load sleep settings from NVS
 */
esp_err_t load_sleep_settings(sleep_settings_t *settings)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("sleep", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS sleep settings not found, using defaults");
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
            ESP_LOGI(TAG, "Migrated idle_timeout from %d min to %d sec", idle_timeout / 60, idle_timeout);
            // Save with new key
            nvs_set_i32(nvs_handle, "idle_timeout_s", idle_timeout);
            nvs_erase_key(nvs_handle, "idle_timeout");
            nvs_commit(nvs_handle);
        }
    }
    settings->idle_timeout_sec = idle_timeout;
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Loaded sleep settings: mode=%d, timeout=%d sec", 
             settings->sleep_mode, settings->idle_timeout_sec);
    return ESP_OK;
}

/**
 * 🌙 Save sleep settings to NVS
 */
esp_err_t save_sleep_settings(const sleep_settings_t *settings)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("sleep", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for sleep settings: %s (%d)", esp_err_to_name(err), err);
        return err;
    }
    
    // Erase old key if it exists (migration from minutes to seconds)
    nvs_erase_key(nvs_handle, "idle_timeout");
    
    // Save sleep mode
    err = nvs_set_i32(nvs_handle, "sleep_mode", settings->sleep_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save sleep_mode: %s (%d)", esp_err_to_name(err), err);
        nvs_close(nvs_handle);
        return err;
    }
    
    // Save idle timeout
    err = nvs_set_i32(nvs_handle, "idle_timeout_s", settings->idle_timeout_sec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save idle_timeout_s: %s (%d)", esp_err_to_name(err), err);
        // Try to erase all keys and retry
        nvs_erase_all(nvs_handle);
        // Re-save sleep mode after erase
        err = nvs_set_i32(nvs_handle, "sleep_mode", settings->sleep_mode);
        if (err == ESP_OK) {
            err = nvs_set_i32(nvs_handle, "idle_timeout_s", settings->idle_timeout_sec);
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Still failed to save idle_timeout_s after erase: %s (%d)", esp_err_to_name(err), err);
            nvs_close(nvs_handle);
            return err;
        }
        ESP_LOGW(TAG, "NVS namespace was full, erased and retried successfully");
    }
    
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit NVS changes: %s (%d)", esp_err_to_name(err), err);
        nvs_close(nvs_handle);
        return err;
    }
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Sleep settings saved: mode=%d, timeout=%d sec", 
                 settings->sleep_mode, settings->idle_timeout_sec);
    }
    return err;
}

/**
 * 🌙 Update system activity timestamp
 */
static void update_system_activity(void)
{
    g_last_system_activity_time = xTaskGetTickCount();
}

/**
 * 🌙 Check if should enter sleep mode
 */
static bool should_enter_sleep(void)
{
    if (g_sleep_settings.sleep_mode == 0 || g_sleep_settings.idle_timeout_sec == 0) {
        return false; // sleep disabled
    }
    
    uint32_t idle_time_ms = pdTICKS_TO_MS(xTaskGetTickCount() - g_last_can_activity_time);
    uint32_t idle_timeout_ms = g_sleep_settings.idle_timeout_sec * 1000;
    
    ESP_LOGD(TAG, "Sleep check: mode=%d, timeout=%d, idle_time=%d ms, timeout_ms=%d", 
             g_sleep_settings.sleep_mode, g_sleep_settings.idle_timeout_sec, idle_time_ms, idle_timeout_ms);
    
    return idle_time_ms >= idle_timeout_ms;
}

/**
 * 🌙 Enter sleep mode based on configured settings
 */
static void enter_configured_sleep(void)
{
    ESP_LOGI(TAG, "Entering sleep mode: %d", g_sleep_settings.sleep_mode);
    
    // 🎵 Стан плеєра вже збережено в can_sleep_monitor_task
    
    switch (g_sleep_settings.sleep_mode) {
        case 1: // Light sleep - ЗАМІНИЛИ НА DEEP SLEEP
        case 2: // Modem sleep - ЗАМІНИЛИ НА DEEP SLEEP  
        case 3: // Deep sleep
        default:
            ESP_LOGI(TAG, "Deep sleep - full power off");
            enter_deep_sleep();
            break;
    }
}

/**
 * 🔌 Ініціалізація CAN драйвера
 */
static esp_err_t init_can(void)
{
    ESP_LOGI(TAG, "🔌 CAN ИНИЦИАЛИЗАЦИЯ НАЧАЛО");
    
    // 🔄 Всегда сбрасываем на стандартную скорость 125 kbps при инициализации
    g_can_bitrate = CAN_DEFAULT_BITRATE;
    ESP_LOGI(TAG, "   📍 Скорость CAN установлена на: %d кбит/с", g_can_bitrate);
    
    // Get timing config based on global bitrate
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, CAN_MODE);
    twai_timing_config_t t_config = get_can_timing_config(g_can_bitrate);
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    
    ESP_LOGI(TAG, "   ⚙️ Конфигурация: TX=GPIO%d, RX=GPIO%d, Mode=%d", 
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
        ESP_LOGE(TAG, "   ❌ ОШИБКА установки CAN драйвера: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "   ✅ CAN драйвер установлен");
    
    ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "   ❌ ОШИБКА запуска CAN: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "   ✅ CAN драйвер запущен");
    
    // Configure alerts
    uint32_t alerts_enabled;
    twai_read_alerts(&alerts_enabled, 0);
    twai_reconfigure_alerts(CAN_ALERT_FLAGS, NULL);
    
    ESP_LOGI(TAG, "✅ CAN ИНИЦИАЛИЗАЦИЯ УСПЕШНА");
    ESP_LOGI(TAG, "   🔌 TX: GPIO%d, RX: GPIO%d", CAN_TX_GPIO, CAN_RX_GPIO);
    ESP_LOGI(TAG, "   ⚡ Скорость: %d кбит/с", g_can_bitrate);
    return ESP_OK;
}

/**
 * 🔄 Динамическая смена скорости CAN
 */
static esp_err_t change_can_bitrate(uint16_t new_bitrate_kbps)
{
    // Проверяем поддерживаемые скорости
    if (new_bitrate_kbps != 100 && new_bitrate_kbps != 125 && 
        new_bitrate_kbps != 250 && new_bitrate_kbps != 500 && new_bitrate_kbps != 1000) {
        ESP_LOGE(TAG, "❌ Неподдерживаемая скорость CAN: %d kbps", new_bitrate_kbps);
        return ESP_ERR_INVALID_ARG;
    }
    
    // Если скорость уже такая же, ничего не делаем
    if (g_can_bitrate == new_bitrate_kbps) {
        ESP_LOGI(TAG, "⚠️ CAN уже работает на %d kbps", new_bitrate_kbps);
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "🔄 Смена скорости CAN: %d -> %d kbps", g_can_bitrate, new_bitrate_kbps);
    
    // Останавливаем CAN
    esp_err_t ret = twai_stop();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Ошибка при остановке CAN: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Деинициализируем драйвер
    ret = twai_driver_uninstall();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Ошибка при деинициализации CAN: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // Обновляем глобальную переменную
    g_can_bitrate = new_bitrate_kbps;
    
    // Переинициализируем CAN с новой скоростью
    ret = init_can();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Ошибка при переинициализации CAN: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "✅ Скорость CAN успешно изменена на %d kbps", new_bitrate_kbps);
    return ESP_OK;
}

/**
 * Ініціалізація I2C master
 */
static esp_err_t init_i2c_master(void)
{
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
        ESP_LOGE(TAG, "❌ Помилка встановлення UART драйвера: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "✅ UART master ініціалізовано (TX=%d RX=%d, %d bps)", MASTER_UART_TX_PIN, MASTER_UART_RX_PIN, MASTER_UART_BAUD);
    }
    // Clean any pending RX
    uart_flush(MASTER_UART_PORT);
    return err;
}

/**
 * Відправити просту команду по UART з префіксом
 * Підтримує як старий формат (без префіксу) для сумісності,
 * так і новий з префіксом для розділення OTA та звичайних команд
 */
static esp_err_t send_i2c_command(uint8_t cmd)
{
    // Відправляємо з префіксом UART_CMD_PREFIX_NORMAL
    uint8_t packet[2] = {UART_CMD_PREFIX_NORMAL, cmd};
    int w = uart_write_bytes(MASTER_UART_PORT, (const char *)packet, 2);
    return (w == 2) ? ESP_OK : ESP_FAIL;
}

/**
 * Отримати версію збірки від slave пристрою
 */
esp_err_t get_slave_version_extended(char *main_date, size_t main_date_len, char *main_time, size_t main_time_len,
                                     char *crc_date, size_t crc_date_len, char *crc_time, size_t crc_time_len,
                                     char *uart_date, size_t uart_date_len, char *uart_time, size_t uart_time_len)
{
    // Спробувати до 3 разів отримати версію
    for (int attempt = 0; attempt < 3; attempt++) {
        // Відправляємо команду отримання версії
        uint8_t packet[2] = {UART_CMD_PREFIX_NORMAL, UART_CMD_GET_VERSION};
        int w = uart_write_bytes(MASTER_UART_PORT, (const char *)packet, 2);
        if (w != 2) {
            ESP_LOGW(TAG, "Failed to send version request (attempt %d)", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        // Читаємо відповідь з більшим таймаутом
        version_response_t resp;
        int r = uart_read_bytes(MASTER_UART_PORT, (uint8_t *)&resp, sizeof(resp), pdMS_TO_TICKS(2000));
        if (r != sizeof(resp)) {
            ESP_LOGW(TAG, "Failed to read version response (attempt %d): %d/%d bytes", 
                    attempt + 1, r, sizeof(resp));
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        
        // Перевіряємо відповідь
        if (resp.prefix != UART_CMD_PREFIX_NORMAL || resp.cmd != UART_CMD_GET_VERSION) {
            ESP_LOGW(TAG, "Invalid version response (attempt %d): prefix=0x%02X, cmd=0x%02X", 
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
        
        //ESP_LOGI(TAG, "✅ Slave versions received:");
        //ESP_LOGI(TAG, "  🎵 Main: %s %s", resp.main_build_date, resp.main_build_time);
        //ESP_LOGI(TAG, "  📡 CRC: %s %s", resp.crc_build_date, resp.crc_build_time);
        //ESP_LOGI(TAG, "  📡 UART: %s %s", resp.uart_build_date, resp.uart_build_time);
        return ESP_OK;
    }
    
    // Якщо всі спроби невдалі, повертаємо порожні рядки
    ESP_LOGW(TAG, "Failed to get slave version after 3 attempts");
    if (main_date && main_date_len > 0) main_date[0] = '\0';
    if (main_time && main_time_len > 0) main_time[0] = '\0';
    if (crc_date && crc_date_len > 0) crc_date[0] = '\0';
    if (crc_time && crc_time_len > 0) crc_time[0] = '\0';
    if (uart_date && uart_date_len > 0) uart_date[0] = '\0';
    if (uart_time && uart_time_len > 0) uart_time[0] = '\0';
    return ESP_FAIL;
}



/**
 * 📨 Обробити CAN повідомлення
 */
static void process_can_message(twai_message_t *message)
{
    // ⏰ ОНОВЛЮЄМО час останньої активності
    g_last_can_activity_time = xTaskGetTickCount();
    
    // 😴 Якщо ми в режимі сну - прокидаємося
    if (g_can_sleep_mode) {
        ESP_LOGI(TAG, "🌅 CAN: Пробудження від сну по CAN активності");
        g_can_sleep_mode = false;
        g_sleep_start_time = 0;
        
        // ⏰ Чекаємо 5 секунд перед поновленням відтворення
        ESP_LOGI(TAG, "⏳ CAN: Очікування %d мс перед поновленням", CAN_WAKEUP_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(CAN_WAKEUP_DELAY_MS));
    }
    
    ESP_LOGI(TAG, "📨 CAN: Отримано повідомлення ID: 0x%03X, DLC: %d", 
             message->identifier, message->data_length_code);
    
    app_message_t app_msg;
    
    // 🔍 Перевіряємо основний ID і перший байт даних
    if (message->identifier == CAN_ID_MAIN && 
        message->data_length_code >= 2 && 
        message->data[0] == 0x01) {
        
        uint8_t cmd = message->data[1];
        uint8_t cmd_masked = cmd & 0xF0;  // Маскуємо молодші біти (останній символ)
        
        switch (cmd_masked) {
            case 0x40:  // NEXT_TRACK (0x4x, де x може бути будь-чим)
                ESP_LOGI(TAG, "⏭️ CAN: Команда НАСТУПНИЙ ТРЕК (0x%02X)", cmd);
                
                // Если в режиме external input - отправляем по UART на внешнее устройство
                if (g_mode_external) {
                    ESP_LOGI(TAG, "🔀 Режим external - отправка по UART");
                    if (send_i2c_command(LINK_CMD_NEXT_TRACK) != ESP_OK) {
                        ESP_LOGW(TAG, "⚠️ UART: не вдалося відправити команду NEXT_TRACK");
                    }
                } else {
                    // Иначе - управляем локальным плеером
                    app_msg.id = APP_NEXT_TRACK;
                    xQueueSend(app_queue, &app_msg, portMAX_DELAY);
                }
                break;
                
            case 0x80:  // PREV_TRACK (0x8x, де x може бути будь-чим)
                ESP_LOGI(TAG, "⏮️ CAN: Команда ПОПЕРЕДНІЙ ТРЕК (0x%02X)", cmd);
                
                // Если в режиме external input - отправляем по UART на внешнее устройство
                if (g_mode_external) {
                    ESP_LOGI(TAG, "🔀 Режим external - отправка по UART");
                    if (send_i2c_command(LINK_CMD_PREV_TRACK) != ESP_OK) {
                        ESP_LOGW(TAG, "⚠️ UART: не вдалося відправити команду PREV_TRACK");
                    }
                } else {
                    // Иначе - управляем локальным плеером
                    app_msg.id = APP_PREV_TRACK;
                    xQueueSend(app_queue, &app_msg, portMAX_DELAY);
                }
                break;
                
            case 0x10:  // MODE_SWITCH (0x1x, де x може бути будь-чим)
                ESP_LOGI(TAG, "🔄 CAN: Команда ПЕРЕМИКАННЯ РЕЖИМУ (0x%02X)", cmd);
                app_msg.id = APP_MODE_SWITCH;
                xQueueSend(app_queue, &app_msg, portMAX_DELAY);
                break;
                
            default:
                //ESP_LOGW(TAG, "❓ CAN: Невідома команда: 0x%02X", cmd);
                break;
        }
    } else {
       // ESP_LOGW(TAG, "⚠️ CAN: Некоректне повідомлення або невірний формат");
    }
}

/**
 * 🌐 Фонова задача для підключення до STA WiFi
 */
static void sta_connection_task(void *arg)
{
    ESP_LOGI(TAG, "🌐 STA підключення: початок фонової задачі");
    
    // Завантажуємо credentials
    char sta_ssid[32] = "";
    char sta_password[64] = "";
    load_sta_credentials(sta_ssid, sizeof(sta_ssid), sta_password, sizeof(sta_password));
    
    if (strlen(sta_ssid) == 0) {
        ESP_LOGW(TAG, "🌐 STA підключення: credentials не знайдені");
        vTaskDelete(NULL);
        return;
    }
    
    // Спробувати переключитись в STA
    esp_err_t ret = wifi_can_switch_to_sta(sta_ssid, sta_password);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "🌐 STA підключення: помилка переключення: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    
    // Чекати підключення до 15 секунд (збільшено для DHCP)
    ESP_LOGI(TAG, "⏳ STA підключення: очікування 15 сек...");
    vTaskDelay(pdMS_TO_TICKS(15000));
    
    // Перевірити чи отримали IP адресу
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            if (ip_info.ip.addr != 0) {
                ESP_LOGI(TAG, "✅ STA підключення: успішно! IP: " IPSTR, IP2STR(&ip_info.ip));
                vTaskDelete(NULL);
                return;
            }
        }
    }
    
    ESP_LOGW(TAG, "⚠️ STA підключення: не отримав IP адресу, перехід в AP режим");
    
    // Якщо STA не вдалося, переключаємося в AP режим
    char ap_ssid[32] = "";
    char ap_password[64] = "";
    load_ap_credentials(ap_ssid, sizeof(ap_ssid), ap_password, sizeof(ap_password));
    
    ret = wifi_can_switch_to_ap(ap_ssid, ap_password);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "✅ STA підключення: fallback до AP режиму: %s", ap_ssid);
    } else {
        ESP_LOGE(TAG, "❌ STA підключення: помилка fallback до AP: %s", esp_err_to_name(ret));
    }
    
    vTaskDelete(NULL);
}

/**
 * 👁️🗨️ Завдання моніторингу CAN активності та переходу в сон
 */
static void can_sleep_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "CAN sleep monitor task started");
    
    // Ініціалізація таймерів активності
    uint32_t current_time = xTaskGetTickCount();
    g_last_can_activity_time = current_time;
    g_last_system_activity_time = current_time;
    ESP_LOGI(TAG, "CAN sleep monitor: ініціалізація таймерів активності");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CAN_SLEEP_CHECK_PERIOD));
        
        // Check new sleep settings first
        if (should_enter_sleep()) {
            ESP_LOGI(TAG, "CAN bus idle for %d seconds, entering sleep mode", 
                    g_sleep_settings.idle_timeout_sec);
            ESP_LOGI(TAG, "Sleep mode: %d, timeout: %d", g_sleep_settings.sleep_mode, g_sleep_settings.idle_timeout_sec);
            
            // Log current state before sleep
            
            // Log current state before sleep
            ESP_LOGI(TAG, "State before sleep:");
            ESP_LOGI(TAG, "  playlist_ready: %d", g_playlist_ready);
            ESP_LOGI(TAG, "  current_track: %d", g_current_track);
            ESP_LOGI(TAG, "  last_saved_position: %d ms", g_last_saved_position);
            ESP_LOGI(TAG, "  player_task_handle: %p", g_player_task_handle);
            
            // 🎵 Зберігаємо стан плеєра ПЕРЕД його зупинкою
            g_playback_was_active_before_sleep = (g_player_task_handle != NULL);
            ESP_LOGI(TAG, "🎵 Playback was active before sleep: %s", g_playback_was_active_before_sleep ? "YES" : "NO");
            
            // Stop player if playing
            if (g_player_task_handle) {
                ESP_LOGI(TAG, "Stopping player for sleep");
                stop_playback();
            }
            
            // Save position before sleep
            if (g_playlist_ready && g_current_track >= 0 && g_last_saved_position > 0) {
                ESP_LOGI(TAG, "Saving position before sleep: track %d, %d ms", 
                         g_current_track, g_last_saved_position);
                save_playback_position(g_current_track, g_last_saved_position);
            }
            
            // Unmount USB if mounted
            if (g_usb_mounted) {
                ESP_LOGI(TAG, "Unmounting USB before sleep");
                unmount_usb_device();
            }
            
            vTaskDelay(pdMS_TO_TICKS(1000));
            
            // Enter sleep based on configured mode
            enter_configured_sleep();
            continue; // After wakeup, continue monitoring
        }
    }
}

/**
 * 📊 Відправити статус по CAN
 */
static void send_can_status(void)
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
        ESP_LOGI(TAG, "📨 CAN: Відправлено статус - трек %d/%d, статус: 0x%02X", 
                 g_current_track + 1, g_playlist_size, status);
    } else {
        ESP_LOGW(TAG, "⚠️ CAN: Не вдалося відправити статус: %s", esp_err_to_name(ret));
    }
}

/**
 * 📨 Відправити подію по CAN
 */
static void send_can_event(uint8_t event_type, uint8_t data1, uint8_t data2)
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
        ESP_LOGI(TAG, "📨 CAN TX ✅: ID=0x%03X, [0x%02X, 0x%02X, 0x%02X, 0x%02X] (подія: 0x%02X)", 
                 message.identifier, message.data[0], message.data[1], message.data[2], 
                 message.data[3], event_type);
    } else {
        ESP_LOGE(TAG, "❌ CAN TX ПОМИЛКА: не вдалося передати (0x%02X): %s", event_type, esp_err_to_name(ret));
    }
}

/**
 * 📡 Завдання обробки CAN
 */
static void can_task(void *arg)
{
    ESP_LOGI(TAG, "📡 Завдання CAN запущено");
    
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
            
            // 📡 ВІДПРАВЛЯЄМО CAN-дані через Wi-Fi на ПК
            esp_err_t wifi_ret = wifi_can_sniffer_send(
                message.identifier, 
                message.data_length_code, 
                message.data
            );
            
            if (wifi_ret == ESP_OK) {
                ESP_LOGI(TAG, "📤 CAN→Wi-Fi: ID=0x%03X, DLC=%d", 
                         message.identifier, message.data_length_code);
            } else {
                ESP_LOGW(TAG, "⚠️ Не вдалося відправити CAN через Wi-Fi: %s", 
                         esp_err_to_name(wifi_ret));
            }
            
            // 🎯 Обробляємо тільки якщо не в режимі сну або якщо це команда пробудження
            if (!g_can_sleep_mode || 
                (message.identifier == CAN_ID_MAIN && message.data[0] == 0x01)) {
                process_can_message(&message);
            }
        } else if (ret != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "❌ CAN помилка отримання: %s", esp_err_to_name(ret));
        }
        
        // 📨 Відправляти статус кожні 5 секунд (тільки якщо не спим)
        if (!g_can_sleep_mode) {
            static uint32_t last_status_sent = 0;
            uint32_t now = xTaskGetTickCount();
            if (now - last_status_sent >= pdMS_TO_TICKS(5000)) {
                // Відправка статусу по фізичній CAN-шині
                //send_can_status();
                
                // 📊 Відправка статистики через Wi-Fi
                esp_err_t stat_ret = wifi_can_sniffer_send_statistics(
                    g_can_frames_per_second,
                    g_can_bytes_per_second
                );
                
                //if (stat_ret == ESP_OK) {
                //    ESP_LOGI(TAG, "📊 Статистика: %d кадрів/с, %d байт/с", 
                //             g_can_frames_per_second, g_can_bytes_per_second);
                //}
                
                // Скидаємо лічильники
                g_can_frames_per_second = 0;
                g_can_bytes_per_second = 0;
                
                last_status_sent = now;
            }
            /*
            // 🧪 ТЕСТОВА ВІДПРАВКА: відправляємо різні CAN-пакети кожну секунду
            static uint32_t last_test_sent = 0;
            static uint8_t test_counter = 0;
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
                    ESP_LOGI(TAG, "🧪 TEST #%d: ID=0x%03X, Len=%d, Data=%s", 
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

/**
 * 💾 Зберегти позицію відтворення (с проверкой на корректность)
 */
static esp_err_t save_playback_position(int32_t track_index, int32_t position_ms) {
    // 🚨 ВАЖНО: Проверяем что позиция корректная
    if (position_ms < 0 || position_ms > 3600000) { // Больше 1 часа - ошибка
        ESP_LOGW(TAG, "⚠️ Подозрительная позиция для сохранения: %d мс", position_ms);
        position_ms = 0;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "💾 save_playback_position: Помилка відкриття NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG, "💾 ЗБЕРЕЖЕННЯ: трек=%d, позиція=%d мс", track_index, position_ms);
    
    // 🎯 Сохраняем также время сохранения для отладки
    uint32_t save_time = (uint32_t)(esp_timer_get_time() / 1000);
    err = nvs_set_u32(nvs_handle, "save_time_ms", save_time);
    
    err = nvs_set_i32(nvs_handle, "track_index", track_index);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "💾 save_playback_position: Помилка track_index: %s", esp_err_to_name(err));  // ИСПРАВЛЕНО: err вместо ret
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_i32(nvs_handle, "position_ms", position_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "💾 save_playback_position: Помилка position_ms: %s", esp_err_to_name(err));  // ИСПРАВЛЕНО: err вместо ret
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✅ Позиція збережена (час: %"PRIu32" мс)", save_time);
    } else {
        ESP_LOGE(TAG, "❌ Помилка коміту NVS: %s", esp_err_to_name(err));
    }
    
    return err;
}

static esp_err_t load_playback_position(int32_t *track_index, int32_t *position_ms)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "💾 load_playback_position: Помилка відкриття NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_get_i32(nvs_handle, "track_index", track_index);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "💾 load_playback_position: track_index не знайдено: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_get_i32(nvs_handle, "position_ms", position_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "💾 load_playback_position: position_ms не знайдено: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // 🚨 ВАЖНО: Проверяем корректность загруженных данных
    if (*position_ms < 0 || *position_ms > 3600000) { // > 1 часа
        ESP_LOGW(TAG, "⚠️ Завантажена підозріла позиція: %d мс, скидаємо до 0", *position_ms);
        *position_ms = 0;
    }
    
    // Проверяем время сохранения
    uint32_t save_time = 0;
    nvs_get_u32(nvs_handle, "save_time_ms", &save_time);  // Эта функция может вернуть ошибку, игнорируем
    uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000);
    
    if (save_time > 0) {
        uint32_t time_diff = current_time - save_time;
        ESP_LOGI(TAG, "💾 Позиція збережена %d секунд тому", time_diff / 1000);
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "📥 ЗАВАНТАЖЕНО: трек=%" PRId32 ", позиція=%" PRId32 " мс", 
             *track_index, *position_ms);
    
    return ESP_OK;
}

/**
 * 🗑️ Очистити збережену позицію
 */
static esp_err_t clear_playback_position(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_erase_key(nvs_handle, "track_index");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_erase_key(nvs_handle, "position_ms");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "🗑️ Збережену позицію очищено");
        g_saved_track_index = -1;
        g_saved_position_ms = 0;
    }
    
    return err;
}

/**
 * 📝 Додати файл до плейлиста
 */
static void add_to_playlist(const char *file_path) {
    if (g_playlist_size >= MAX_PLAYLIST_SIZE) {
        ESP_LOGW(TAG, "⚠️ Плейлист заповнений, не можу додати: %s", file_path);
        return;
    }
    
    if (g_playlist_size == 0) {
        g_playlist = malloc(sizeof(char*));
    } else {
        g_playlist = realloc(g_playlist, (g_playlist_size + 1) * sizeof(char*));
    }
    
    if (!g_playlist) {
        ESP_LOGE(TAG, "❌ Не вдалося виділити пам'ять для плейлиста");
        return;
    }
    
    g_playlist[g_playlist_size] = strdup(file_path);
    g_playlist_size++;
    //ESP_LOGI(TAG, "📝 Додано до плейлиста: %s (всього: %d)", file_path, g_playlist_size);
}

/**
 * 🗑️ Очистити плейлист
 */
static void clear_playlist(void) {
    if (g_playlist) {
        for (int i = 0; i < g_playlist_size; i++) {
            free(g_playlist[i]);
        }
        free(g_playlist);
        g_playlist = NULL;
    }
    g_playlist_size = 0;
    g_current_track = 0;
    g_playlist_ready = false;
}

/**
 * 🧹 Очистка I2S драйвера (альтернатива i2s_driver_uninstall)
 */
/**
 * 🧹 Упрощенная очистка I2S драйвера
 */
static void cleanup_i2s_driver(void) {
    ESP_LOGI(TAG, "🧹 Упрощенная очистка I2S драйвера (перевірка g_mode_external)...");
    if (g_mode_external) {
        ESP_LOGI(TAG, "ℹ️ Зовнішній режим активний, викликаємо stop_external_input()...");
        stop_external_input();
    }
}

/**
 * 🧹 Очистка аудио буферов (альтернатива audio_element_flush_*)
 */
static void flush_audio_buffers(void) {
    ESP_LOGI(TAG, "🧹 Очистка аудио буферов...");
    
    // 🔧 Простая задержка вместо реальной очистки буферов
    // В вашей версии ESP-ADF может не быть функций flush
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Можно попробовать пересоздать аудио элементы
    ESP_LOGI(TAG, "✅ Аудио буферы очищены");
}

/**
 * 🧹 Безопасная очистка I2S драйвера
 */
static void safe_i2s_cleanup(void) {
    ESP_LOGI(TAG, "🧹 Безопасная очистка I2S...");
    
    if (g_i2s_tx_handle != NULL) {
        // 🔧 Останавливаем канал
        i2s_channel_disable(g_i2s_tx_handle);
        vTaskDelay(pdMS_TO_TICKS(50));
        
        // 🗑️ Удаляем канал
        i2s_del_channel(g_i2s_tx_handle);
        g_i2s_tx_handle = NULL;
        
        ESP_LOGI(TAG, "✅ I2S канал остановлен и удален");
    } else {
        ESP_LOGW(TAG, "⚠️ I2S хэндл уже NULL, пропускаем очистку");
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
}


/**
 * ⏸️ Зупинити відтворення
 */
static void stop_playback(void) {
    if (g_player_task_handle) {
        ESP_LOGI(TAG, "⏸️ Полная остановка воспроизведения");
        
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
        ESP_LOGI(TAG, "⏳ Очікування завершення задачі плеєра...");
        int timeout = 50; // 5 секунд
        while (g_player_task_handle != NULL && timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            timeout--;
        }
        
        if (g_player_task_handle != NULL) {
            ESP_LOGW(TAG, "⚠️ Задача не завершилась, примусове видалення");
            TaskHandle_t task_to_delete = g_player_task_handle;
            g_player_task_handle = NULL;
            vTaskDelete(task_to_delete);
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            ESP_LOGI(TAG, "✅ Задача плеєра успішно завершилась");
        }
        
        // 4. Відновлюємо стан плейлиста якщо він був готовий
        g_playlist_ready = old_playlist_ready;
        
        ESP_LOGI(TAG, "✅ Воспроизведение полностью остановлено");
    }
    g_skip_to_next = false;
    g_skip_to_prev = false;
}

/**
 * ⏭️ Відтворити наступний трек
 */
static void play_next_track(void) {
    // 😴 Якщо в режимі сну - спочатку прокидаємося
    if (g_can_sleep_mode) {
        ESP_LOGI(TAG, "🌅 Пробудження від сну по команді next track");
        g_can_sleep_mode = false;
        g_last_can_activity_time = xTaskGetTickCount();
        vTaskDelay(pdMS_TO_TICKS(CAN_WAKEUP_DELAY_MS));
    }
    
    if (!g_playlist_ready || g_playlist_size == 0) {
        ESP_LOGE(TAG, "❌ Плейлист порожній або не готовий");
        return;
    }
    
    g_current_track = (g_current_track + 1) % g_playlist_size;
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ⏭️  НАСТУПНИЙ ТРЕК                            ║");
    ESP_LOGI(TAG, "╠═══════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║  📀 %d / %d", g_current_track + 1, g_playlist_size);
    ESP_LOGI(TAG, "║  📁 %s", g_playlist[g_current_track]);
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    g_skip_to_next = true;
    
    // 💾 Зберегти позицію при зміні треку (початок нового треку)
    save_playback_position(g_current_track, 0);
    g_last_saved_position = 0;
    
    // 📨 Відправити подію по CAN
    //send_can_event(EVENT_TRACK_CHANGED, g_current_track + 1, 0x01); // 0x01 = вперед
}

/**
 * ⏮️ Відтворити попередній трек
 */
static void play_prev_track(void) {
    // 😴 Якщо в режимі сну - спочатку прокидаємося
    if (g_can_sleep_mode) {
        ESP_LOGI(TAG, "🌅 Пробудження від сну по команді prev track");
        g_can_sleep_mode = false;
        g_last_can_activity_time = xTaskGetTickCount();
        vTaskDelay(pdMS_TO_TICKS(CAN_WAKEUP_DELAY_MS));
    }
    
    if (!g_playlist_ready || g_playlist_size == 0) {
        ESP_LOGE(TAG, "❌ Плейлист порожній або не готовий");
        return;
    }
    
    g_current_track = (g_current_track - 1 + g_playlist_size) % g_playlist_size;
    
    ESP_LOGI(TAG, "");
    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════╗");
    ESP_LOGI(TAG, "║  ⏮️  ПОПЕРЕДНІЙ ТРЕК                           ║");
    ESP_LOGI(TAG, "╠═══════════════════════════════════════════════╣");
    ESP_LOGI(TAG, "║  📀 %d / %d", g_current_track + 1, g_playlist_size);
    ESP_LOGI(TAG, "║  📁 %s", g_playlist[g_current_track]);
    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════╝");
    ESP_LOGI(TAG, "");
    
    g_skip_to_prev = true;
    
    // 💾 Зберегти позицію
    save_playback_position(g_current_track, 0);
    g_last_saved_position = 0;
    
    // 📨 Відправити подію по CAN
    //send_can_event(EVENT_TRACK_CHANGED, g_current_track + 1, 0x00); // 0x00 = назад
}

static void enter_deep_sleep(void)
{
    ESP_LOGI(TAG, "😴 Перехід в глибокий сон...");

    // 🏷️ Встановлюємо прапорець сну
    g_can_sleep_mode = true;
    g_sleep_start_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "⏰ Встановлення будильника на GPIO%d (рівень LOW)", CAN_RX_GPIO);

    // ⏰ Налаштовуємо пробудження по CAN RX
    // Зупиняємо CAN контролер
    if (twai_stop() != ESP_OK) {
        ESP_LOGE(TAG, "Помилка зупинки TWAI драйвера перед сном.");
        // Можливо, варто не переходити в сон, якщо не можемо зупинити драйвер
        return;
    }
    ESP_LOGI(TAG, "Драйвер TWAI зупинено.");

    // Налаштовуємо пін CAN RX як вхід з підтяжкою вгору
    gpio_config_t io_conf = {
        .pin_bit_mask = BIT64(CAN_RX_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    ESP_LOGI(TAG, "GPIO%d налаштовано як вхід з pull-up.", CAN_RX_GPIO);

    // Вмикаємо пробудження від зовнішнього джерела 0 (ext0)
    // Пробудження відбудеться, коли на піні CAN_RX_GPIO буде низький рівень (0)
    if (esp_sleep_enable_ext0_wakeup(CAN_RX_GPIO, 0) != ESP_OK) {
        ESP_LOGE(TAG, "Помилка налаштування пробудження ext0.");
        return;
    }
    ESP_LOGI(TAG, "Пробудження ext0 налаштовано для GPIO%d.", CAN_RX_GPIO);

    ESP_LOGI(TAG, "💤 Перехід в глибокий сон через 2 секунди...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    gpio_set_level((gpio_num_t)SLEEP_IND_PIN, 0);

    // 😴 Переходимо в сон
    esp_deep_sleep_start();
}

/**
 * 🌅 Выйти из глубокого сна
 */
static void wakeup_from_sleep(void)
{
    ESP_LOGI(TAG, "🌅 Пробудження від сну...");
    
    // 🔄 Скидаємо прапорець сну
    g_can_sleep_mode = false;
    
    // 📥 Завантажуємо збережену позицію
    load_playback_position(&g_saved_track_index, &g_saved_position_ms);
    g_restore_playback = true;
    
    // ⏰ Оновлюємо останню активність
    g_last_can_activity_time = xTaskGetTickCount();
    
    ESP_LOGI(TAG, "💾 Позиція відновлена: трек %" PRId32 ", %" PRId32 " мс", 
             g_saved_track_index, g_saved_position_ms);
    
    // ⏳ Чекаємо перед початком відтворення
    vTaskDelay(pdMS_TO_TICKS(CAN_WAKEUP_DELAY_MS));

    // Устанавливаем пин-индикатор сна в HIGH при пробуждении
    gpio_set_level((gpio_num_t)SLEEP_IND_PIN, 1);
}



/**
 * 🔌 Демонтувати USB пристрій
 */
static void unmount_usb_device(void) {
    if (!g_usb_mounted) {
        return;
    }
    
    vTaskDelay(pdMS_TO_TICKS(500));
    
    g_usb_mounted = false;
    g_current_mount_slot = -1;
    ESP_LOGI(TAG, "🔌 USB демонтовано");
}

/**
 * 🔌 Монтувати USB пристрій
 */
static bool mount_usb_device(int slot) {
    if (g_usb_mounted && g_current_mount_slot == slot) {
        return true;
    }
    
    if (g_usb_mounted) {
        unmount_usb_device();
    }
    
    if (slot < 0 || slot >= MAX_MSC_DEVICES || !msc_devices[slot]) {
        ESP_LOGE(TAG, "❌ Невірний слот для монтування: %d", slot);
        return false;
    }
    
    g_current_mount_slot = slot;
    g_usb_mounted = true;
    ESP_LOGI(TAG, "🔌 USB монтовано зі слота %d", slot);
    return true;
}

/**
 * 🔊 Изменить громкость аудиочипа
 */
static void set_volume(int volume_percent) {
    if (volume_percent < 0) volume_percent = 0;
    if (volume_percent > 100) volume_percent = 100;
    
    g_volume = volume_percent;
    
    if (g_board_handle) {
        audio_hal_set_volume(g_board_handle->audio_hal, g_volume);
        ESP_LOGI(TAG, "🔊 Громкость установлена: %d%%", g_volume);
    } else {
        ESP_LOGW(TAG, "⚠️ Аудиочип не инициализирован, громкость не изменена");
    }
}

/**
 * 🔊 Увеличить громкость
 */
static void volume_up(void) {
    int new_volume = g_volume + 10;
    if (new_volume > 100) new_volume = 100;
    set_volume(new_volume);
}

/**
 * 🔊 Уменьшить громкость
 */
static void volume_down(void) {
    int new_volume = g_volume - 10;
    if (new_volume < 0) new_volume = 0;
    set_volume(new_volume);
}

/**
 * 📞 Callback для обробки подій аудіо елементів
 */
static esp_err_t audio_event_cb(audio_element_handle_t el, audio_event_iface_msg_t *event, void *ctx)
{
    if (event->cmd == AEL_MSG_CMD_REPORT_STATUS) {
        if ((int)event->data == AEL_STATUS_STATE_FINISHED) {
            bool *playback_finished = (bool*)ctx;
            *playback_finished = true;
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔═══════════════════════════════════════════════╗");
            ESP_LOGI(TAG, "║  ✅ ТРЕК ЗАВЕРШЕНО                            ║");
            ESP_LOGI(TAG, "╚═══════════════════════════════════════════════╝");
            ESP_LOGI(TAG, "");
        }
        // 🔴 Перевіряємо на помилки декодування
        else if ((int)event->data == AEL_STATUS_ERROR_OPEN ||
                 (int)event->data == AEL_STATUS_ERROR_INPUT ||
                 (int)event->data == AEL_STATUS_ERROR_OUTPUT) {
            ESP_LOGE(TAG, "❌ Помилка аудіо елемента: %d", (int)event->data);
            
            // Якщо це декодер - позначаємо як проблемний файл
            if (strstr(audio_element_get_tag(el), "decoder")) {
                ESP_LOGW(TAG, "⚠️ Проблемний MP3 файл - пропускаємо!");
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
        ESP_LOGI(TAG, "🎵 Audio info - Sample rate: %d Hz, Channels: %d, Bits: %d", 
                info.sample_rates, info.channels, info.bits);
        
        // Перевіримо чи це MP3 з високим бітрейтом
        if (info.sample_rates > 44100 || (info.bits > 16 && info.channels > 2)) {
            ESP_LOGW(TAG, "⚠️ Високий бітрейт/семпл рейт - можливі проблеми з продуктивністю!");
        }
    }
    
    // 🔊 Моніторинг буферів
    if (event->cmd == AEL_MSG_CMD_REPORT_STATUS) {
        audio_element_info_t info = {0};
        audio_element_getinfo(el, &info);
        
        // Логування інформації про стан елемента
        ESP_LOGD(TAG, "🔊 Audio element status - Bytes: %d/%d", 
                (int)info.byte_pos, (int)info.total_bytes);
    }
    
    return ESP_OK;
}

/**
 * 🔍 Аналіз MP3 файлу для діагностики продуктивності
 */
static void analyze_mp3_file(const char *filename) {
    ESP_LOGI(TAG, "🔍 Спроба відкрити файл: %s", filename);
    FILE *file = fopen(filename, "rb");
    if (!file) {
        ESP_LOGE(TAG, "❌ Не вдалося відкрити файл для аналізу: %s (errno: %d)", filename, errno);
        return;
    }
    
    ESP_LOGI(TAG, "✅ Файл успішно відкрито для аналізу");
    
    // Отримуємо розмір файлу
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    ESP_LOGI(TAG, "📏 Розмір файлу: %ld байт", file_size);
    
    // Читаємо більше байтів для пошуку MP3 фреймів (враховуючи ID3 теги)
    uint8_t buffer[1024];
    size_t bytes_to_read = sizeof(buffer);
    if (bytes_to_read > file_size) bytes_to_read = file_size;
    
    size_t read = fread(buffer, 1, bytes_to_read, file);
    fclose(file);
    
    if (read < 4) {
        ESP_LOGW(TAG, "⚠️ Файл занадто малий для MP3: %s", filename);
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
        ESP_LOGW(TAG, "⚠️ Не знайдено MP3 фрейм синхронізації в файлі: %s", filename);
        return;
    }
    
    // Використовуємо знайдений MP3 фрейм для аналізу
    uint8_t *header = &buffer[mp3_offset];
    
    ESP_LOGI(TAG, "🎵 Знайдено MP3 фрейм на позиції: %d байт", mp3_offset);
    
    // Дебаг: виведемо перші 4 байти заголовка
    ESP_LOGI(TAG, "🔍 Заголовок MP3: %02X %02X %02X %02X", 
             header[0], header[1], header[2], header[3]);
    
    // Парсимо MPEG версію та шар
    int version = (header[1] >> 3) & 0x03;
    int layer = (header[1] >> 1) & 0x03;
    
    // Парсимо бітрейт
    int bitrate_index = (header[2] >> 4) & 0x0F;
    int sample_rate_index = (header[2] >> 2) & 0x03;
    
    ESP_LOGI(TAG, "🔍 Індекси: version=%d, layer=%d, bitrate_idx=%d, sample_rate_idx=%d", 
             version, layer, bitrate_index, sample_rate_index);
    
    const int bitrates[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    const int sample_rates[4] = {44100, 48000, 32000, 0};
    
    int bitrate = bitrates[bitrate_index];
    int sample_rate = sample_rates[sample_rate_index];
    
    // Валідація MP3 заголовка
    if (bitrate == 0 || sample_rate == 0 || version > 2 || layer != 1) {
        ESP_LOGW(TAG, "⚠️ Недійсний MP3 заголовок (bitrate=%d, sample_rate=%d, version=%d, layer=%d)", 
                 bitrate, sample_rate, version, layer);
        ESP_LOGW(TAG, "🔍 Спробуємо знайти наступний MP3 фрейм...");
        
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
                    ESP_LOGI(TAG, "✅ Знайдено валідний MP3 фрейм на позиції: %d байт", next_mp3_offset);
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
            
            ESP_LOGI(TAG, "🔍 Новий заголовок: %02X %02X %02X %02X", 
                     header[0], header[1], header[2], header[3]);
        } else {
            ESP_LOGW(TAG, "❌ Не знайдено валідний MP3 фрейм у файлі: %s", filename);
            return;
        }
    }
    
    ESP_LOGI(TAG, "🎵 MP3 аналіз - Файл: %s", filename);
    ESP_LOGI(TAG, "🎵 Бітрейт: %d kbps, Семпл рейт: %d Hz", bitrate, sample_rate);
    
    // Попередження про потенційні проблеми з продуктивністю
    if (bitrate > 192) {
        ESP_LOGW(TAG, "⚠️ Високий бітрейт (%d kbps) - можливі проблеми з декодуванням!", bitrate);
    }
    if (sample_rate > 44100) {
        ESP_LOGW(TAG, "⚠️ Високий семпл рейт (%d Hz) - можливі проблеми з продуктивністю!", sample_rate);
    }
    
    // Додаткові перевірки для проблемних файлів
    if (bitrate >= 256) {
        ESP_LOGW(TAG, "🚨 Дуже високий бітрейт (%d kbps) - ESP32 може не справитися!", bitrate);
    }
}
static int get_current_position(audio_element_handle_t fatfs_stream_reader) {
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
            ESP_LOGD(TAG, "⏱️ get_current_position: %d/%d байт -> ~%d мс", 
                    (int)info.byte_pos, (int)info.total_bytes, position_ms);
            
            return position_ms;
        }
    }
    return 0;
}

/**
 * 🎯 Встановити позицію відтворення (альтернативный метод)
 */
static esp_err_t set_playback_position(audio_element_handle_t fatfs_stream_reader, int position_ms) {
    ESP_LOGI(TAG, "🎯 Встановлення позиції: %d мс", position_ms);
    
    if (position_ms <= 0) {
        return ESP_OK;
    }
    
    // 🚨 ВАЖНО: Для FATFS stream нужно использовать другой подход
    // Получаем информацию о файле
    audio_element_info_t info = {0};
    esp_err_t ret = audio_element_getinfo(fatfs_stream_reader, &info);
    
    if (ret != ESP_OK || info.total_bytes <= 0) {
        ESP_LOGE(TAG, "❌ Не вдалося отримати інформацію про файл");
        return ret;
    }
    
    // Вычисляем позицию в байтах
    // Для MP3 примерно: 1 секунда = 16000 байт (для 128kbps)
    size_t target_bytes = (size_t)((position_ms * 16000) / 1000);
    
    // Ограничиваем 90% файла
    if (target_bytes > (size_t)(info.total_bytes * 0.9)) {
        target_bytes = (size_t)(info.total_bytes * 0.9);
    }
    
    ESP_LOGI(TAG, "   Ціль: %d байт (з %d)", (int)target_bytes, (int)info.total_bytes);
    
    // ⏳ Даем время
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // Пытаемся установить позицию
    ret = audio_element_set_byte_pos(fatfs_stream_reader, target_bytes);
    
    if (ret == ESP_OK) {
        // Проверяем результат
        vTaskDelay(pdMS_TO_TICKS(300));
        audio_element_getinfo(fatfs_stream_reader, &info);
        
        int actual_ms = (int)((info.byte_pos * 1000) / 16000);
        ESP_LOGI(TAG, "✅ Позиція встановлена: %d байт (~%d мс)", 
                 (int)info.byte_pos, actual_ms);
    } else {
        ESP_LOGE(TAG, "❌ Помилка встановлення позиції: %s", esp_err_to_name(ret));
    }
    
    return ret;
}



/**
 * 🔍 Детальна перевірка позиції файлу
 */
static void debug_file_position(audio_element_handle_t el, const char *context) {
    audio_element_info_t info = {0};
    if (audio_element_getinfo(el, &info) == ESP_OK) {
        if (info.total_bytes > 0) {
            // Рассчитываем позицию несколькими способами для проверки
            int pos_ms_method1 = (int)(info.byte_pos / 16);  // Для 128kbps
            int pos_ms_method2 = (int)((info.byte_pos * 1000) / 16000);
            float percent = (info.byte_pos * 100.0f) / info.total_bytes;
            
            ESP_LOGI(TAG, "🔍 %s:", context);
            ESP_LOGI(TAG, "   Байти: %d / %d", (int)info.byte_pos, (int)info.total_bytes);
            ESP_LOGI(TAG, "   Відсоток: %.1f%%", percent);
            ESP_LOGI(TAG, "   Позиція (метод 1): %d мс", pos_ms_method1);
            ESP_LOGI(TAG, "   Позиція (метод 2): %d мс", pos_ms_method2);
            ESP_LOGI(TAG, "   URI: %s", info.uri ? info.uri : "NULL");
        }
    }
}

/**
 * 🔊 Відтворити тестовий синусоїдальний сигнал для перевірки I2S виходу.
 *
 * @param i2s_writer    Елемент i2s_stream для запису.
 * @param duration_ms   Тривалість відтворення в мілісекундах.
 */
static void play_test_tone(audio_element_handle_t i2s_writer, int duration_ms)
{
    ESP_LOGI(TAG, "🔊 Відтворення тестового тону (%d мс)...", duration_ms);

    int sample_rate = 44100;
    int channels = 2;
    float freq = 440.0; // Нота 'Ля' (A4)
    int volume = 8000;  // Амплітуда (макс. 32767 для 16-біт)

    int buffer_size = 1024;
    int16_t *buffer = malloc(buffer_size);
    if (!buffer) {
        ESP_LOGE(TAG, "❌ Не вдалося виділити пам'ять для буфера тестового тону");
        return;
    }

    int total_samples_to_write = duration_ms * sample_rate / 1000;
    int samples_written = 0;

    // Ensure the element is running before trying to write to it
    audio_element_state_t writer_state = audio_element_get_state(i2s_writer);
    if (writer_state != AEL_STATE_RUNNING) {
        ESP_LOGW(TAG, "⚠️ I2S writer не в стані RUNNING (%d), неможливо відтворити тестовий тон", writer_state);
        free(buffer);
        return;
    }

    ESP_LOGI(TAG, "✍️  Початок запису тестового тону в i2s_writer...");
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
            ESP_LOGE(TAG, "❌ Помилка запису тестового тону в I2S writer (записано %d)", bytes_written_now);
            break;
        }
        samples_written += (bytes_written_now / (channels * sizeof(int16_t)));
    }

    free(buffer);
    ESP_LOGI(TAG, "✅ Тестовий тон завершено (записано %d семплів)", samples_written);
}

// 🛑 Stop all tasks for OTA update
void stop_playback_and_services(void)
{
    ESP_LOGI(TAG, "🛑 Stopping playback and services for OTA...");
    
    // Stop CAN/TWAI sniffer if running (safe to call even if not initialized)
    esp_err_t err = twai_stop();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "✅ CAN sniffer stopped");
    }
    
    // Give other tasks time to stop naturally
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "✅ All services stopped, ready for OTA");
}

/**
 * 🎧 Завдання відтворення аудіо (исправленная версия)
 */
static void player_task(void *pvParam)
{
    ESP_LOGI(TAG, "🎧 Завдання відтворення запущено");
    
    if (!mount_usb_device(g_current_mount_slot)) {
        ESP_LOGE(TAG, "❌ Не вдалося монтувати USB для відтворення");
        g_player_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 🔄 Сброс флага восстановления если трек не совпадает
    if (g_restore_playback && g_current_track != g_saved_track_index) {
        ESP_LOGW(TAG, "⚠️ Запрошенный трек %d не совпадает с сохраненным %d", 
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

        ESP_LOGI(TAG, "");
        ESP_LOGI(TAG, "╔═══════════════════════════════════════════════╗");
        ESP_LOGI(TAG, "║  🎵 ВІДТВОРЕННЯ ТРЕКУ                         ║");
        ESP_LOGI(TAG, "╠═══════════════════════════════════════════════╣");
        ESP_LOGI(TAG, "║  📀 Трек: %d / %d", g_current_track + 1, g_playlist_size);
        ESP_LOGI(TAG, "║  📁 Файл: %s", g_playlist[g_current_track]);
        ESP_LOGI(TAG, "╚═══════════════════════════════════════════════╝");
        ESP_LOGI(TAG, "");
        
        // 🔍 Проаналізуємо MP3 файл перед відтворенням
        // Використовуємо шлях безпосередньо з плейлиста (він вже містить повний шлях)
        ESP_LOGI(TAG, "🔍 Аналіз файлу: %s", g_playlist[g_current_track]);
        analyze_mp3_file(g_playlist[g_current_track]);
        
        if (!g_usb_mounted) {
            ESP_LOGE(TAG, "❌ USB демонтовано під час відтворення");
            break;
        }
        
        // 🏗️ Створити пайплайн
        audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
        pipeline = audio_pipeline_init(&pipeline_cfg);
        if (!pipeline) {
            ESP_LOGE(TAG, "❌ Не вдалося створити пайплайн");
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

        i2s_stream_writer = i2s_stream_init(&i2s_cfg);
        
        // 🎵 Створити декодер для MP3
        mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
        decoder = mp3_decoder_init(&mp3_cfg);

        if (!fatfs_stream_reader || !i2s_stream_writer || !decoder) {
            ESP_LOGE(TAG, "❌ Не вдалося створити аудіо елементи");
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
                ESP_LOGI(TAG, "🌅 Спроба відновлення ДО запуску пайплайна: %d байт", (int)target_bytes_pre);
                esp_err_t ret_pre = audio_element_set_byte_pos(fatfs_stream_reader, target_bytes_pre);
                if (ret_pre == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    audio_element_info_t new_info_pre = {0};
                    if (audio_element_getinfo(fatfs_stream_reader, &new_info_pre) == ESP_OK) {
                        int actual_ms_pre = (int)((new_info_pre.byte_pos * 1000) / 16000);
                        ESP_LOGI(TAG, "✅ Позиція (до запуску) встановлена: %d байт (~%d мс)", (int)new_info_pre.byte_pos, actual_ms_pre);
                        g_last_saved_position = actual_ms_pre;
                        // Якщо вдалося — скидаємо прапори відновлення, щоб не повторювати після старту
                        g_restore_playback = false;
                        g_saved_track_index = -1;
                        g_saved_position_ms = 0;
                    }
                } else {
                    ESP_LOGW(TAG, "⚠️ Не вдалося встановити позицію до запуску пайплайна: %s", esp_err_to_name(ret_pre));
                }
            } else {
                ESP_LOGI(TAG, "ℹ️ Інфо файлу недоступне до запуску пайплайна, спробуємо після старту");
            }
        }
        
        // 📞 Встановити callback для відстеження завершення
        audio_element_set_event_callback(fatfs_stream_reader, audio_event_cb, &playback_finished);
        
        // 🎯 Додамо callback для всіх елементів для моніторингу продуктивності
        audio_element_set_event_callback(decoder, audio_event_cb, NULL);
        audio_element_set_event_callback(i2s_stream_writer, audio_event_cb, NULL);

        // 🚀 Запускаємо пайплайн
        ESP_LOGI(TAG, "🚀 Запуск пайплайну");
        esp_err_t ret = audio_pipeline_run(pipeline);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ Не вдалося запустити пайплайн: %s", esp_err_to_name(ret));
            goto cleanup;
        }

        // Скидаємо сторож прогресу
        g_stall_last_bytes = 0;
        g_stall_last_ts_ms = esp_timer_get_time() / 1000;

        // Якщо після запуску пайплайну потрібно відновити позицію —
        // попробуем поставить паузу, выполнить seek и затем возобновить,
        // это часто помогает корректно пропустить начальные байты декодером.
        if (g_restore_playback && g_current_track == g_saved_track_index && g_saved_position_ms > 0) {
            ESP_LOGI(TAG, "⏸️ Пауза пайплайна для установки позиції");
            audio_pipeline_pause(pipeline);

            audio_element_info_t info_seek = {0};
            if (audio_element_getinfo(fatfs_stream_reader, &info_seek) == ESP_OK && info_seek.total_bytes > 0) {
                size_t target_bytes = (size_t)((g_saved_position_ms * 16000) / 1000);
                if (target_bytes > (size_t)(info_seek.total_bytes * 0.9)) {
                    target_bytes = (size_t)(info_seek.total_bytes * 0.9);
                }
                ESP_LOGI(TAG, "🌅 Встановлюємо позицію ПІСЛЯ запуску, до resume: %d байт", (int)target_bytes);
                esp_err_t rseek = audio_element_set_byte_pos(fatfs_stream_reader, target_bytes);
                if (rseek == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    audio_element_info_t check = {0};
                    if (audio_element_getinfo(fatfs_stream_reader, &check) == ESP_OK) {
                        int actual_ms = (int)((check.byte_pos * 1000) / 16000);
                        ESP_LOGI(TAG, "✅ Позиція встановлена після pause: %d байт (~%d мс)", (int)check.byte_pos, actual_ms);
                        g_last_saved_position = actual_ms;
                    }
                } else {
                    ESP_LOGW(TAG, "⚠️ Не вдалося встановити позицію після pause: %s", esp_err_to_name(rseek));
                }
            } else {
                ESP_LOGW(TAG, "⚠️ Інфо файлу недоступне після запуску пайплайна");
            }

            ESP_LOGI(TAG, "▶️ Відновлюємо пайплайн після seek");
            audio_pipeline_resume(pipeline);

            // Сбросим флаги восстановления, чтобы не пытаться снова
            g_restore_playback = false;
            g_saved_track_index = -1;
            g_saved_position_ms = 0;
        }

        // ⏳ Чекаємо, поки пайплайн реально почне відтворювати
        ESP_LOGI(TAG, "⏳ Очікування старту відтворення...");
        bool playback_started = false;
        int wait_counter = 0;
        
        while (!playback_started && wait_counter < 100) { // Макс 5 секунд
            audio_element_state_t file_state = audio_element_get_state(fatfs_stream_reader);
            
            if (file_state == AEL_STATE_RUNNING) {
                playback_started = true;
                ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                ESP_LOGI(TAG, "✅ ПАЙПЛАЙН ЗАПУЩЕНО");
                ESP_LOGI(TAG, "🎯 Decoder → I2S → PCM5102");
                ESP_LOGI(TAG, "═══════════════════════════════════════════════");
                
                // 📊 Розрахувати тривалість треку
                audio_element_info_t dur_info = {0};
                if (audio_element_getinfo(decoder, &dur_info) == ESP_OK && dur_info.duration > 0) {
                    g_current_track_duration_ms = dur_info.duration;
                    int corrected_duration = g_current_track_duration_ms;
                    ESP_LOGI(TAG, "📊 Тривалість треку: %d мс (%d:%02d)", corrected_duration, 
                             corrected_duration / 60000, (corrected_duration % 60000) / 1000);
                } else {
                    // Fallback: приблизний розрахунок для 128 kbps
                    audio_element_info_t file_info = {0};
                    if (audio_element_getinfo(fatfs_stream_reader, &file_info) == ESP_OK && file_info.total_bytes > 0) {
                        g_current_track_duration_ms = (int32_t)((file_info.total_bytes * 1000) / 16000);
                        int corrected_duration = g_current_track_duration_ms;
                        ESP_LOGI(TAG, "📊 Приблизна тривалість треку: %d мс (%d:%02d)", corrected_duration, 
                                 corrected_duration / 60000, (corrected_duration % 60000) / 1000);
                    } else {
                        g_current_track_duration_ms = 180000; // За замовчуванням 3 хв
                    }
                }
                
                // 🌅 СПРОБА ВІДНОВЛЕННЯ ПОЗИЦІЇ
                if (g_restore_playback && g_current_track == g_saved_track_index && g_saved_position_ms > 0) {
                    ESP_LOGI(TAG, "");
                    ESP_LOGI(TAG, "╔═══════════════════════════════════════════════╗");
                    ESP_LOGI(TAG, "║  🌅 ВІДНОВЛЕННЯ ПОЗИЦІЇ                       ║");
                    ESP_LOGI(TAG, "╠═══════════════════════════════════════════════╣");
                    ESP_LOGI(TAG, "║  🎵 Трек: %d", g_current_track);
                    ESP_LOGI(TAG, "║  ⏱️  Позиція: %d мс (%.1f сек)", g_saved_position_ms, g_saved_position_ms / 1000.0);
                    ESP_LOGI(TAG, "╚═══════════════════════════════════════════════╝");
                    
                    // Даем время на инициализацию
                    vTaskDelay(pdMS_TO_TICKS(300));
                    
                    // Получаем информацию о файле
                    audio_element_info_t info = {0};
                    if (audio_element_getinfo(fatfs_stream_reader, &info) == ESP_OK && info.total_bytes > 0) {
                        ESP_LOGI(TAG, "   Розмір файлу: %d байт", (int)info.total_bytes);
                        
                        // Вычисляем позицию в байтах
                        // Для MP3 примерно: 1 секунда = 16000 байт (128kbps)
                        size_t target_bytes = (size_t)((g_saved_position_ms * 16000) / 1000);
                        
                        // Ограничиваем 90% файла
                        if (target_bytes > (size_t)(info.total_bytes * 0.9)) {
                            target_bytes = (size_t)(info.total_bytes * 0.9);
                            ESP_LOGW(TAG, "   Позиція скоригована до %d байт", (int)target_bytes);
                        }
                        
                        ESP_LOGI(TAG, "   Встановлюємо позицію: %d байт", (int)target_bytes);
                        
                        // ⏳ Задержка перед установкой
                        vTaskDelay(pdMS_TO_TICKS(100));
                        
                        // Пробуем несколько раз установить позицию
                        for (int attempt = 0; attempt < 5; attempt++) {
                            ret = audio_element_set_byte_pos(fatfs_stream_reader, target_bytes);
                            
                            if (ret == ESP_OK) {
                                ESP_LOGI(TAG, "✅ Позиція встановлена на спробі %d", attempt + 1);
                                
                                // Проверяем результат
                                vTaskDelay(pdMS_TO_TICKS(200));
                                audio_element_info_t new_info = {0};
                                if (audio_element_getinfo(fatfs_stream_reader, &new_info) == ESP_OK) {
                                    int actual_ms = (int)((new_info.byte_pos * 1000) / 16000);
                                    ESP_LOGI(TAG, "   Фактична позиція: %d байт (~%d мс)", 
                                             (int)new_info.byte_pos, actual_ms);
                                    
                                    // Обновляем сохраненную позицию
                                    g_last_saved_position = actual_ms;
                                }
                                break;
                            } else {
                                ESP_LOGW(TAG, "⚠️ Спроба %d не вдалась: %s", 
                                         attempt + 1, esp_err_to_name(ret));
                                vTaskDelay(pdMS_TO_TICKS(200));
                            }
                        }
                    }
                    
                    // 🔄 Скидаємо прапори ВНЕЗАВИСИМО от результата
                    g_restore_playback = false;
                    g_saved_track_index = -1;
                    g_saved_position_ms = 0;
                    ESP_LOGI(TAG, "✅ Відновлення позиції оброблено");
                }
                break;
            }
            
            if (wait_counter % 20 == 0) { // Каждую секунду
                ESP_LOGI(TAG, "⏳ Очікування... стан: %d", file_state);
            }
            
            wait_counter++;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (!playback_started) {
            ESP_LOGW(TAG, "⚠️ Пайплайн не запустився нормально");
            goto cleanup;
        }

        ESP_LOGI(TAG, "⏳ Очікування завершення відтворення...");
        ESP_LOGI(TAG, "⌨️ Terminal commands task started");
        ESP_LOGI(TAG, "   n - next track");
        ESP_LOGI(TAG, "   p - previous track");
        ESP_LOGI(TAG, "   m - mode switch");

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
                        ESP_LOGI(TAG, "💾 Автозбереження: %d мс", current_position);
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
                ESP_LOGI(TAG, "✅ Файл прочитано повністю");
                playback_finished = true;
                break;
            }
            
            // ❌ Ошибка
            if (file_state == AEL_STATE_STOPPED) {
                ESP_LOGE(TAG, "❌ Елемент зупинився неочікувано");
                playback_error = true;
                break;
            }
            
            // 🎯 Перевірка на перемотку через веб
            if (g_seek_position_ms >= 0) {
                ESP_LOGI(TAG, "🎯 Перемотка до %d мс", g_seek_position_ms);
                
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
                ESP_LOGW(TAG, "⚠️ Досягнуто таймаут відтворення");
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
                        ESP_LOGW(TAG, "⚠️ Відтворення зависло (немає прогресу %lld мс), форс зупинка", (long long)(now_ms - g_stall_last_ts_ms));
                        playback_error = true;
                        break;
                    }
                }
            }
            
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // 💾 Сохраняем позицию при завершении
        if (playback_finished) {
            if (g_decoder_error) {
                ESP_LOGW(TAG, "⚠️ Пропускаємо проблемний MP3 файл: %s", g_playlist[g_current_track]);
                g_decoder_error = false;  // Скидаємо прапор
                // Автоматично переходимо до наступного треку
                g_current_track = (g_current_track + 1) % g_playlist_size;
                ESP_LOGI(TAG, "⏭️ Переходимо до треку: %d/%d", g_current_track + 1, g_playlist_size);
            } else {
                ESP_LOGI(TAG, "✅ Відтворення успішно завершено");
                save_playback_position((g_current_track + 1) % g_playlist_size, 0);
                g_last_saved_position = 0;
            }
        } else if (playback_error) {
            ESP_LOGE(TAG, "❌ Відтворення завершено з помилкою");
        } else if (g_skip_to_next || g_skip_to_prev) {
            ESP_LOGI(TAG, "🔀 Відтворення перервано для зміни треку");
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
            ESP_LOGI(TAG, "🛑 Примусова зупинка задачі");
            break;
        }

        if (g_skip_to_prev) {
            g_skip_to_prev = false;
            g_current_track = (g_current_track - 1 + g_playlist_size) % g_playlist_size;
        } else if (g_skip_to_next) {
            g_skip_to_next = false;
            if (!g_play_specific_track) {
                g_current_track = (g_current_track + 1) % g_playlist_size;
            }
            g_play_specific_track = false;
        } else {
            // Автоматичне переключення на наступний трек
            g_current_track = (g_current_track + 1) % g_playlist_size;
        }
        
        g_last_saved_position = 0; // Скидання позиції для нового трека
        
        ESP_LOGI(TAG, "⏭️ Наступний трек: %d/%d", g_current_track + 1, g_playlist_size);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    ESP_LOGI(TAG, "👋 Завдання відтворення завершено");
    g_player_task_handle = NULL;
    
    // 🔌 НЕ демонтуємо USB якщо переключаємось на external input
    // USB залишається підключеним для швидкого повернення
    if (!g_mode_external) {
        unmount_usb_device();
    } else {
        ESP_LOGI(TAG, "ℹ️ USB залишається змонтованим (режим external)");
    }
    
    vTaskDelete(NULL);
}



static void debug_audio_state(audio_element_handle_t el, const char *name) {
    audio_element_info_t info = {0};
    audio_element_state_t state = audio_element_get_state(el);
    
    if (audio_element_getinfo(el, &info) == ESP_OK) {
        ESP_LOGI(TAG, "%s: стан=%d, байт=%d/%d, URI=%s", 
                 name, state, (int)info.byte_pos, (int)info.total_bytes,
                 info.uri ? info.uri : "NULL");
    } else {
        ESP_LOGI(TAG, "%s: стан=%d, не вдалося отримати інфо", name, state);
    }
}



/**
 * 🔍 Рекурсивно знайти всі MP3 файли та додати їх до плейлиста
 */
static void find_mp3_files_recursive(const char *path)
{
    struct dirent *d;
    DIR *dh = opendir(path);
    if (!dh) {
        ESP_LOGE(TAG, "❌ Не вдалося відкрити директорію: %s", path);
        return;
    }

    while ((d = readdir(dh)) != NULL) {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0) {
            continue;
        }

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, d->d_name);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                find_mp3_files_recursive(full_path);
            } else if (S_ISREG(st.st_mode)) {
                const char *ext = strrchr(d->d_name, '.');
                if (ext && (strcasecmp(ext, ".mp3") == 0)) {
                    //ESP_LOGI(TAG, "🔍 Знайдено MP3 файл: %s", full_path);
                    add_to_playlist(full_path);
                }
            }
        }
    }
    closedir(dh);
}

/**
 * 📝 Створити плейлист з усіх MP3 файлів на USB
 */
static void create_playlist_from_usb(void)
{
    clear_playlist();
    
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i]) {
            char mount_path[16];
            snprintf(mount_path, sizeof(mount_path), MNT_PATH "%d", i);
            ESP_LOGI(TAG, "🔍 Пошук MP3 файлів в %s", mount_path);
            find_mp3_files_recursive(mount_path);
        }
    }
    
    if (g_playlist_size > 0) {
        g_playlist_ready = true;
        
        // 🔍 Перевірити, чи є збережена позиція для цього плейлиста
        if (g_saved_track_index >= 0 && g_saved_track_index < g_playlist_size) {
            g_current_track = g_saved_track_index;
            g_restore_playback = true;
            ESP_LOGI(TAG, "✅ Плейлист створено! Знайдено %d треків. Відновлюємо з треку %" PRId32, 
                     g_playlist_size, g_saved_track_index + 1);
        } else {
            g_current_track = 0;
            ESP_LOGI(TAG, "✅ Плейлист створено! Знайдено %d треків", g_playlist_size);
        }
        
        // 📨 Відправити подію про підключення USB
        //send_can_event(EVENT_USB_CONNECTED, g_playlist_size, 0x00);
    } else {
        ESP_LOGW(TAG, "⚠️ MP3 файлів не знайдено");
    }
}

/**
 * 🔍 Знайти вільний слот у таблиці пристроїв
 */
static inline int find_free_slot(void)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i] == NULL) {
            return i;
        }
    }
    return -1;
}

/**
 * 🆕 Виділити новий запис пристрою MSC та монтувати його до VFS
 */
static esp_err_t allocate_new_msc_device(const app_message_t *msg, int *out_slot)
{
    int slot = find_free_slot();
    if (slot < 0) {
        ESP_LOGW(TAG, "⚠️ Немає вільних слотів для нового пристрою MSC (макс. %d)", MAX_MSC_DEVICES);
        return ESP_ERR_NOT_FOUND;
    }

    msc_devices[slot] = calloc(1, sizeof(msc_dev_entry_t));
    if (!msc_devices[slot]) {
        ESP_LOGE(TAG, "❌ Не вдалося виділити пам'ять для запису пристрою MSC");
        return ESP_ERR_NO_MEM;
    }
    
    esp_err_t err = msc_host_install_device(msg->data.new_dev_address, &msc_devices[slot]->msc_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ msc_host_install_device не вдалося: %s", esp_err_to_name(err));
        free(msc_devices[slot]);
        msc_devices[slot] = NULL;
        return err;
    }

    msc_devices[slot]->usb_addr = msg->data.new_dev_address;

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    char mount_path[16];
    snprintf(mount_path, sizeof(mount_path), MNT_PATH "%d", slot);

    err = msc_host_vfs_register(msc_devices[slot]->msc_device, mount_path, &mount_config, &msc_devices[slot]->vfs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "❌ msc_host_vfs_register не вдалося: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(msc_host_uninstall_device(msc_devices[slot]->msc_device));
        free(msc_devices[slot]);
        msc_devices[slot] = NULL;
        return err;
    }

    *out_slot = slot;
    return ESP_OK;
}

/**
 * 🔍 Знайти слот за дескриптором пристрою MSC
 */
static int find_slot_by_handle(msc_host_device_handle_t handle)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i] && msc_devices[i]->msc_device == handle) {
            return i;
        }
    }
    return -1;
}

/**
 * 🗑️ Звільнити ресурси, пов'язані з певним пристроєм MSC за індексом слота
 */
static void free_msc_device(int slot)
{
    if (slot < 0 || slot >= MAX_MSC_DEVICES || !msc_devices[slot]) {
        ESP_LOGE(TAG, "❌ Невірний індекс слота для звільнення пристрою MSC");
        return;
    }

    if (msc_devices[slot]->vfs_handle) {
        ESP_ERROR_CHECK(msc_host_vfs_unregister(msc_devices[slot]->vfs_handle));
    }
    if (msc_devices[slot]->msc_device) {
        ESP_ERROR_CHECK(msc_host_uninstall_device(msc_devices[slot]->msc_device));
    }

    free(msc_devices[slot]);
    msc_devices[slot] = NULL;
}

/**
 * 🗑️ Звільнити всі підключені пристрої MSC
 */
static void free_all_msc_devices(void)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i]) {
            free_msc_device(i);
        }
    }
}

/**
 * 🔘 Зворотний виклик натискання кнопки BOOT
 */
static void gpio_cb(void *arg)
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

/**
 * 🔍 Знайти USB-адресу за дескриптором пристрою MSC
 */
static inline int8_t find_usb_addr_by_handle(msc_host_device_handle_t handle)
{
    for (int8_t i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i] && msc_devices[i]->msc_device == handle) {
            return msc_devices[i]->usb_addr;
        }
    }
    return -1;
}

/**
 * 📞 Зворотний виклик драйвера MSC
 */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    if (event->event == MSC_DEVICE_CONNECTED) {
        ESP_LOGI(TAG, "💾 Пристрій MSC підключено (usb_addr=%d)", event->device.address);
        app_message_t message = {
            .id = APP_DEVICE_CONNECTED,
            .data.new_dev_address = event->device.address,
        };
        xQueueSend(app_queue, &message, portMAX_DELAY);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        int usb_addr = find_usb_addr_by_handle(event->device.handle);
        if (usb_addr >= 0) {
            ESP_LOGI(TAG, "📤 Пристрій MSC відключено (usb_addr=%d)", usb_addr);
        } else {
            ESP_LOGW(TAG, "⚠️ Пристрій MSC відключено, але не вдалося отримати USB адресу");
        }
        app_message_t message = {
            .id = APP_DEVICE_DISCONNECTED,
            .data.device_handle = event->device.handle,
        };
        xQueueSend(app_queue, &message, portMAX_DELAY);
    }
}


/**
 * Запуск пайплайна для внешнего I2S входа (ESP32 -> этот ESP32), который напрямую переадресует
 * входящий PCM на I2S-выход (PCM5102).
 */
static esp_err_t start_external_input(void)
{
    if (g_i2s_passthrough_task_handle) {
        ESP_LOGW(TAG, "⚠️ External input task already running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "▶️ Запуск external input (низкоуровневый I2S passthrough)...");

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
        ESP_LOGE(TAG, "❌ Не вдалося створити i2s_passthrough_task");
        g_mode_external = false;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "✅ External input passthrough task started (I2S slave on I2S_NUM_1).");
    return ESP_OK;
}

static void stop_external_input(void)
{
    if (!g_i2s_passthrough_task_handle) {
        ESP_LOGW(TAG, "⚠️ External input task not running");
        // Ensure mode is false even if handle is already null
        g_mode_external = false; 
        return;
    }

    ESP_LOGI(TAG, "🛑 Зупиняємо external input (raw passthrough)...");

    // 1. Signal the task to stop by clearing the flag
    g_mode_external = false;

    // 2. Wait for the task to finish (it will set the handle to NULL and self-delete)
    int timeout_ms = 2000;
    while (g_i2s_passthrough_task_handle != NULL && timeout_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout_ms -= 100;
    }
    if (g_i2s_passthrough_task_handle != NULL) {
        ESP_LOGW(TAG, "Passthrough task did not stop gracefully, deleting manually.");
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

    ESP_LOGI(TAG, "✅ External input stopped.");
}

/**
 * 🔌 Завдання USB
 */
static void usb_task(void *args)
{
    const usb_host_config_t host_config = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_config));

    bool has_clients = true;
    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            has_clients = false;
            if (usb_host_device_free_all() == ESP_OK) {
                break;
            };
        }
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE && !has_clients) {
            break;
        }
    }

    vTaskDelay(10);
    ESP_LOGI(TAG, "🧹 Деініціалізація USB");
    ESP_ERROR_CHECK(usb_host_uninstall());
    vTaskDelete(NULL);
}

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
            buf_size += strlen(g_playlist[i]) + 4; // "name",
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
            char *src = g_playlist[i];
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
void player_control(const char* cmd, int track) {
    update_system_activity(); // Web control = system activity
    
    if (strcmp(cmd, "play") == 0) {
        if (track >= 0 && track < g_playlist_size) {
            g_current_track = track;
            g_skip_to_next = true;
            g_play_specific_track = true;
            ESP_LOGI(TAG, "🌐 Веб: відтворити трек %d", track);
        } else {
            g_is_paused = false;
            ESP_LOGI(TAG, "🌐 Веб: відновити відтворення");
        }
    } else if (strcmp(cmd, "pause") == 0) {
        g_is_paused = true;
        ESP_LOGI(TAG, "🌐 Веб: пауза");
    } else if (strcmp(cmd, "next") == 0) {
        g_skip_to_next = true;
        g_play_specific_track = false;
        ESP_LOGI(TAG, "🌐 Веб: наступний трек");
    } else if (strcmp(cmd, "prev") == 0) {
        g_skip_to_prev = true;
        ESP_LOGI(TAG, "🌐 Веб: попередній трек");
    } else if (strcmp(cmd, "switch_mode") == 0) {
        ESP_LOGI(TAG, "🌐 Web: switch_mode requested");
        // Надсилаємо повідомлення, як і для клавіші 'm'
        app_message_t msg;
        msg.id = APP_MODE_SWITCH;
        xQueueSend(app_queue, &msg, 0);
    } else if (strcmp(cmd, "toggle_sniffer") == 0) {
        if (!g_sniffer_enabled) {
            ESP_LOGI(TAG, "🌐 Web: enabling WiFi CAN Sniffer via web toggle");
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
                ESP_LOGI(TAG, "✅ Sniffer enabled (saved)");
            } else {
                ESP_LOGE(TAG, "❌ Failed to enable sniffer");
            }
        } else {
            ESP_LOGI(TAG, "🌐 Web: disabling WiFi CAN Sniffer via web toggle");
            wifi_can_sniffer_deinit();
            g_sniffer_enabled = false;
            save_sniffer_enabled(0);
            ESP_LOGI(TAG, "✅ Sniffer disabled (saved)");
        }
    }
    else if (strcmp(cmd, "set_sniffer") == 0) {
        // 'track' parameter is used as state: 1 = enable, 0 = disable
        if (track == 1) {
            if (!g_sniffer_enabled) {
                ESP_LOGI(TAG, "🌐 Web: setting WiFi CAN Sniffer = ON");
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
                    ESP_LOGI(TAG, "✅ Sniffer enabled (saved)");
                } else {
                    ESP_LOGE(TAG, "❌ Failed to enable sniffer");
                }
            }
        } else {
            if (g_sniffer_enabled) {
                ESP_LOGI(TAG, "🌐 Web: setting WiFi CAN Sniffer = OFF");
                wifi_can_sniffer_deinit();
                g_sniffer_enabled = false;
                save_sniffer_enabled(0);
                ESP_LOGI(TAG, "✅ Sniffer disabled (saved)");
            }
        }
    }
}

/**
 * 🎯 Встановити позицію відтворення через веб
 */
void player_set_position(int position_ms) {
    g_seek_position_ms = position_ms;
    ESP_LOGI(TAG, "🌐 Веб: встановити позицію %d мс", position_ms);
}

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
static void wifi_can_on_message_callback(const can_sniffer_frame_t *frame)
{
    if (!frame) return;

    ESP_LOGI(TAG, "📨 Wi-Fi CAN RX: ID=0x%03X, DLC=%d", frame->id, frame->length);

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
}

/**
 * @brief Callback для обработки команд управления из Wi-Fi
 */
static void wifi_can_on_control_callback(uint8_t cmd, const uint8_t *data, uint8_t len)
{
    ESP_LOGI(TAG, "🎛️ Wi-Fi CAN Control Command: 0x%02X, len=%d", cmd, len);

    switch (cmd) {
        case CAN_SNIFFER_CMD_DISCONNECT:
            ESP_LOGI(TAG, "🔌 Disconnect command received");
            break;

        case CAN_SNIFFER_CMD_CONNECT:
            if (len >= 2) {
                uint16_t bitrate = *(uint16_t *)data;
                ESP_LOGI(TAG, "🔌 Connect command: bitrate=%d kbps", bitrate);
                
                // 🔄 Попытка изменить скорость CAN
                esp_err_t ret = change_can_bitrate(bitrate);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "✅ CAN bitrate успешно изменена на %d kbps через Wi-Fi", bitrate);
                } else {
                    ESP_LOGE(TAG, "❌ Ошибка изменения CAN bitrate: %s", esp_err_to_name(ret));
                }
            }
            break;

        default:
            ESP_LOGW(TAG, "❓ Unknown control command: 0x%02X", cmd);
            break;
    }
}

void app_main(void)
{
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
    esp_log_level_set("Player", g_player_logs ? ESP_LOG_INFO : ESP_LOG_NONE);          // ℹ️ Наш код - інформаційний рівень (TAG = "Player")
    esp_log_level_set("WiFiCanSniffer", g_wifi_can_sniffer_logs ? ESP_LOG_INFO : ESP_LOG_NONE);  // ✅ Дозволяємо бачити логи Wi-Fi CAN sniffer
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
    
    // 🔍 Враховуємо останній вибраний режим: якщо external — запускаємо його навіть без USB
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
                    esp_err_t msc_res = allocate_new_msc_device(&msg, &slot);
                    
                    if (msc_res != ESP_OK) {
                        ESP_LOGE(TAG, "❌ Помилка монтування USB: %s", esp_err_to_name(msc_res));
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
                            // === КРИТЕРІЙ 13: Обробка відключення USB ===
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
static void terminal_commands_task(void *arg)
{
    if (g_verbose_logs) {
        ESP_LOGI(TAG, "⌨️ Terminal commands task started");
        ESP_LOGI(TAG, "   n - next track");
        ESP_LOGI(TAG, "   p - previous track");
        ESP_LOGI(TAG, "   m - mode switch");
    }
    
    char ch;
    app_message_t msg;
    
    while (1) {
        // Читаємо один символ з stdin (UART)
        ch = getchar();
        
        if (ch == 'n' || ch == 'N') {
            //ESP_LOGI(TAG, "⏭️ Terminal: Next track");
            msg.id = APP_NEXT_TRACK;
            xQueueSend(app_queue, &msg, 0);
        }
        else if (ch == 'p' || ch == 'P') {
            //ESP_LOGI(TAG, "⏮️ Terminal: Previous track");
            msg.id = APP_PREV_TRACK;
            xQueueSend(app_queue, &msg, 0);
        }
        else if (ch == 'm' || ch == 'M') {
            // Звичайне коротке натискання — перемикання режиму відтворення
            msg.id = APP_MODE_SWITCH;
            xQueueSend(app_queue, &msg, 0);
        }
        else if (ch == 's' || ch == 'S') {
            // Одноразове натискання 's' — відправляємо подію перемикання Wi-Fi у STA (DHCP)
            if (g_verbose_logs) ESP_LOGI(TAG, "🌐 Натискання 's' — запит на перемикання Wi-Fi у STA (DHCP)");
            msg.id = APP_WIFI_SWITCH_STA;
            xQueueSend(app_queue, &msg, 0);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
