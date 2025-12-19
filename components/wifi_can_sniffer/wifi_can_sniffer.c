#include "wifi_can_sniffer.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "lwip/api.h"
#include <string.h>

static const char *TAG = "WiFiCanSniffer";

// Хелпер для розворачивання байтів (little-endian ↔ big-endian)
static inline uint32_t swap_bytes_uint32(uint32_t val)
{
    return ((val & 0xFF) << 24) | 
           ((val & 0xFF00) << 8) | 
           ((val & 0xFF0000) >> 8) | 
           ((val & 0xFF000000) >> 24);
}

// Глобальные переменные
static int g_udp_socket = -1;
static struct sockaddr_in g_pc_addr = {0};
static bool g_pc_connected = false;
static TaskHandle_t g_wifi_task_handle = NULL;
static wifi_can_sniffer_config_t g_config = {0};

// Буфер для накопления пакетов перед отправкой
static uint8_t g_udp_out_buffer[UDP_BUFFER_SIZE] = {0};
static uint8_t *g_udp_out_pos = g_udp_out_buffer;
static uint32_t g_last_flush_time = 0;

// Статистика
static uint16_t g_frames_per_second = 0;
static uint16_t g_bytes_per_second = 0;
static uint32_t g_stat_time = 0;

// Forward declaration
static void flush_udp_buffer(void);

/**
 * @brief Event handler для Wi-Fi
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station connected MAC: " MACSTR, MAC2STR(ev->mac));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station disconnected MAC: " MACSTR, MAC2STR(ev->mac));
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED) {
        ip_event_ap_staipassigned_t *ev = (ip_event_ap_staipassigned_t *)event_data;
        ESP_LOGI(TAG, "IP assigned: " IPSTR, IP2STR(&ev->ip));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA connected to AP");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected; retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

/**
 * @brief Инициализация Wi-Fi в режиме AP
 */
static esp_err_t wifi_init_ap(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "🔧 Initializing Wi-Fi AP...");
    
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "❌ esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "✅ Netif initialized (or already was)");

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "❌ esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "✅ Event loop created (or already was)");

    // Спробувати створити AP netif, але якщо вже існує - використати існуючий
    esp_netif_t *ap_netif = NULL;

    // Спочатку спробуємо отримати існуючий AP netif
    ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) {
        // Якщо не існує, створюємо новий
        ap_netif = esp_netif_create_default_wifi_ap();
        if (!ap_netif) {
            ESP_LOGE(TAG, "❌ Failed to create AP netif");
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "✅ AP netif created");
    } else {
        ESP_LOGI(TAG, "ℹ️ Using existing AP netif");
    }
    
    // Настройка IP адресов для AP
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);       // IP ESP32: 192.168.4.1
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);       // Gateway: 192.168.4.1
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0); // Netmask: 255.255.255.0
    
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);
    
    ESP_LOGI(TAG, "AP IP: 192.168.4.1");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) return ret;

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &wifi_event_handler, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) return ret;

    wifi_config_t wifi_config = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .required = false,
            },
        },
    };

    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid) - 1);
    strncpy((char *)wifi_config.ap.password, password, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.ssid_len = strlen(ssid);

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) return ret;

    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Wi-Fi AP initialized");
    ESP_LOGI(TAG, "   SSID: %s", ssid);
    ESP_LOGI(TAG, "   Password: %s", password);

    return ESP_OK;
}

/**
 * @brief Перемикання Wi-Fi у режим STA та підключення до мережі з DHCP
 */
esp_err_t wifi_can_switch_to_sta(const char *ssid, const char *password)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "🔧 Switching Wi-Fi to STA mode...");

    esp_err_t ret;
    // Ініціалізація netif та подій (якщо ще не ініціалізовано)
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    // Створити STA netif якщо відсутній
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        ESP_LOGI(TAG, "ℹ️ STA netif вже існує, використовуємо існуючий");
        // Отримуємо існуючий STA netif
        sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (!sta_netif) {
            ESP_LOGE(TAG, "❌ Не вдалося отримати існуючий STA netif");
            return ESP_FAIL;
        }
    }

    // Ініціалізація Wi-Fi (якщо не було)
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "❌ esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "✅ Wi-Fi initialized (or already was)");

    // Реєстрація обробників подій
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) return ret; // ESP_ERR_INVALID_ARG means already registered
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) return ret;

    // Перейти у режим STA
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;

    // Налаштування STA
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    if (password) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) return ret;

    // Запустити Wi-Fi та DHCP клієнт
    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    if (sta_netif) {
        esp_netif_dhcpc_stop(sta_netif);
        esp_netif_dhcpc_start(sta_netif);
    }

    // Підключитись
    ret = esp_wifi_connect();
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "📶 STA connecting to SSID: %s", ssid);
    return ESP_OK;
}

/**
 * @brief Перемкнути Wi-Fi у режим AP з заданим SSID/паролем
 */
esp_err_t wifi_can_switch_to_ap(const char *ssid, const char *password)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "🔧 Switching Wi-Fi to AP mode...");

    // Повністю деініціалізувати Wi-Fi перед переключенням
    esp_wifi_stop();
    esp_wifi_deinit();
    ESP_LOGI(TAG, "🔧 Wi-Fi deinitialized");

    // Небольшая задержка для полной очистки
    vTaskDelay(pdMS_TO_TICKS(100));

    // Перейти в AP
    esp_err_t ret = wifi_init_ap(ssid, password);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📶 AP started: %s", ssid);
    } else {
        ESP_LOGE(TAG, "❌ Failed to start AP: %s", esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief Создание UDP сокета
 */
static esp_err_t create_udp_socket(uint16_t port)
{
    ESP_LOGI(TAG, "Creating UDP socket...");
    
    g_udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_udp_socket < 0) {
        ESP_LOGE(TAG, "❌ Failed to create UDP socket: errno=%d", errno);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "✅ Socket created: fd=%d", g_udp_socket);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    ESP_LOGI(TAG, "Binding to 0.0.0.0:%d (0x%04X)...", port, port);
    
    if (bind(g_udp_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "❌ Failed to bind UDP socket: errno=%d", errno);
        close(g_udp_socket);
        g_udp_socket = -1;
        return ESP_FAIL;
    }

    // Увімкнення broadcast для відправки на 192.168.4.255
    int broadcast_enable = 1;
    if (setsockopt(g_udp_socket, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)) < 0) {
        ESP_LOGW(TAG, "⚠️ Failed to enable broadcast: errno=%d", errno);
    } else {
        ESP_LOGI(TAG, "✅ Broadcast enabled");
    }

    ESP_LOGI(TAG, "✅ UDP socket bound successfully");
    return ESP_OK;
}

/**
 * @brief Task для прослушивания UDP пакетов
 */
static void wifi_can_sniffer_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UDP listener started");
    
    uint8_t udp_in_buffer[UDP_BUFFER_SIZE] = {0};
    struct sockaddr_in client_addr = {0};
    socklen_t client_addr_len = sizeof(client_addr);

    while (1) {
        int recv_len = recvfrom(g_udp_socket, udp_in_buffer, sizeof(udp_in_buffer), 0,
                               (struct sockaddr *)&client_addr, &client_addr_len);

        if (recv_len > 0) {
            ESP_LOGI(TAG, "Received %d bytes from %s:%d", recv_len, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            // Hex dump
            char hexbuf[256] = {0};
            int hexlen = 0;
            for (int i = 0; i < recv_len && hexlen < 240; i++) {
                hexlen += sprintf(hexbuf + hexlen, "%02X ", udp_in_buffer[i]);
            }
            ESP_LOGI(TAG, "UDP RAW: %s", hexbuf);
            g_pc_connected = true;

            // Проверяем сигнатуру и длину (минимум 19 байт для CANFrameOut от ПК)
            // Формат PC→ESP32: [sig(4)][id(4)][reserved(2)][len(1)][data(8)] = 19 байт
            if (recv_len >= 19) {
                uint32_t signature = *(uint32_t *)&udp_in_buffer[0];
                
                // Проверяем сигнатуру (может быть в другом порядке байтов)
                if (signature == CAN_SNIFFER_SIGNATURE || signature == 0x55AA55AA) {
                    // CANFrameOut от ПК: всегда 8 байт данных
                    can_sniffer_frame_t frame = {0};
                    frame.id = *(uint32_t *)&udp_in_buffer[4];
                    frame.interval = *(uint16_t *)&udp_in_buffer[8];  // reserved в CANFrameOut
                    frame.length = udp_in_buffer[10];
                    if (frame.length > 8) frame.length = 8;
                    memcpy(frame.data, &udp_in_buffer[11], 8);  // Всегда 8 байт
                    
                    ESP_LOGI(TAG, "📥 CAN RX: id=0x%03X, len=%d", frame.id, frame.length);
                    char databuf[32] = {0};
                    int dlen = 0;
                    for (int i = 0; i < frame.length; i++) {
                        dlen += sprintf(databuf + dlen, "%02X ", frame.data[i]);
                    }
                    ESP_LOGI(TAG, "   Data: %s", databuf);

                    // Сервис-команда (id == 0x7FF)
                    if (frame.id == CAN_SNIFFER_SERVICE_ID && frame.length > 0) {
                        ESP_LOGI(TAG, "🎛️ Control command: 0x%02X", frame.data[0]);
                        if (g_config.on_control_command) {
                            g_config.on_control_command(frame.data[0], &frame.data[1], frame.length-1);
                        }
                    } else {
                        if (g_config.on_can_message) {
                            g_config.on_can_message(&frame);
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "⚠️ Signature mismatch: expected 0xAA55AA55, got 0x%08X", signature);
                }
            } else {
                ESP_LOGW(TAG, "⚠️ Packet too short: %d bytes (need 19)", recv_len);
            }
        } else if (recv_len < 0) {
            ESP_LOGW(TAG, "UDP receive error: %d", errno);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief Отправить накопленные в буфере пакеты
 */
static void flush_udp_buffer(void)
{
    if (g_udp_socket < 0) {
        ESP_LOGE(TAG, "❌ Socket not initialized!");
        return;
    }
    
    if (g_udp_out_pos != g_udp_out_buffer) {
        size_t buffer_size = g_udp_out_pos - g_udp_out_buffer;
        
        // Hex dump для диагностики
        //ESP_LOGI(TAG, "📤 Sending %d bytes to %s:%d (socket=%d)", 
        //         buffer_size, inet_ntoa(g_pc_addr.sin_addr), ntohs(g_pc_addr.sin_port), g_udp_socket);
        char hexbuf[256] = {0};
        int hexlen = 0;
        for (int i = 0; i < buffer_size && i < 64 && hexlen < 240; i++) {
            hexlen += sprintf(hexbuf + hexlen, "%02X ", g_udp_out_buffer[i]);
        }
        //ESP_LOGI(TAG, "   HEX: %s%s", hexbuf, buffer_size > 64 ? "..." : "");
        
        int sent = sendto(g_udp_socket, g_udp_out_buffer, 
                         buffer_size, 0,
                         (struct sockaddr *)&g_pc_addr, sizeof(g_pc_addr));
        
        if (sent > 0) {
            //ESP_LOGI(TAG, "✅ Sent %d bytes successfully", sent);
        } else {
            ESP_LOGE(TAG, "❌ Send failed: errno=%d (%s)", errno, strerror(errno));
        }
        
        // Сбрасываем и очищаем буфер от старых данных
        memset(g_udp_out_buffer, 0, UDP_BUFFER_SIZE);
        g_udp_out_pos = g_udp_out_buffer;
        g_last_flush_time = xTaskGetTickCount();
    }
}

/**
 * @brief Буферизирующая отправка пакета
 * Формат wxCAN-Sniffer (ESP32→PC): [signature(4)] [id(4)] [interval(2)] [length(1)] [data[0..length]]
 * Размер: 4 + 4 + 2 + 1 + length = 11 + length байт
 */
static esp_err_t buffered_send(const can_sniffer_packet_t *packet)
{
    // Размер пакета: 4 (sig) + 4 (id) + 2 (interval) + 1 (len) + length
    size_t packet_size = 11 + packet->frame.length;

    // Проверяем, поместится ли пакет в буфер
    if ((g_udp_out_pos - g_udp_out_buffer) + packet_size >= UDP_BUFFER_SIZE - UDP_BUFFER_MINIMUM) {
        // Отправляем текущий буфер
        flush_udp_buffer();
    }

    // Формируем пакет: копируем поля структуры
    memcpy(g_udp_out_pos, &packet->signature, 4);              // Signature (4)
    memcpy(g_udp_out_pos + 4, &packet->frame.id, 4);           // ID (4)
    memcpy(g_udp_out_pos + 8, &packet->frame.interval, 2);     // Interval (2)
    memcpy(g_udp_out_pos + 10, &packet->frame.length, 1);      // Length (1)
    memcpy(g_udp_out_pos + 11, packet->frame.data, packet->frame.length); // Data (0..length)
    
    g_udp_out_pos += packet_size;

    // Обновляем статистику
    g_frames_per_second++;
    g_bytes_per_second += packet_size;
    
    //ESP_LOGI(TAG, "📦 Buffered: ID=0x%03X, len=%d, size=%d, buf_used=%d", 
    //         packet->frame.id, packet->frame.length, packet_size,
    //         (int)(g_udp_out_pos - g_udp_out_buffer));
    
    // ВАЖНО: Отправляем сразу, не ждем таймера!
    // Для реального трафика можно вернуть буферизацию
    flush_udp_buffer();

    return ESP_OK;
}

// ============================================================================
// Публичные функции
// ============================================================================

esp_err_t wifi_can_sniffer_init(const wifi_can_sniffer_config_t *config)
{
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    ESP_LOGI(TAG, "🚀 Wi-Fi CAN-Sniffer Initialization START");
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    ESP_LOGI(TAG, "🔨 Build: %s %s", __DATE__, __TIME__);

    if (!config || !config->wifi_ssid || !config->wifi_password || !config->pc_ip_str) {
        ESP_LOGE(TAG, "❌ Invalid config!");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "📋 Config: SSID='%s', PC_IP='%s', Port=0x%04X", 
             config->wifi_ssid, config->pc_ip_str, config->udp_port);

    memcpy(&g_config, config, sizeof(wifi_can_sniffer_config_t));

    // Инициализация Wi-Fi
    ESP_LOGI(TAG, "📡 Step 1: Initializing Wi-Fi AP...");
    esp_err_t ret = wifi_init_ap(config->wifi_ssid, config->wifi_password);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ Wi-Fi init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "✅ Wi-Fi AP initialized");

    // Создание UDP сокета
    ESP_LOGI(TAG, "🔌 Step 2: Creating UDP socket...");
    ret = create_udp_socket(config->udp_port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "❌ UDP socket creation failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "✅ UDP socket created");

    // Парсим IP адрес PC
    ESP_LOGI(TAG, "🎯 Step 3: Configuring PC address...");
    g_pc_addr.sin_family = AF_INET;
    g_pc_addr.sin_port = htons(config->udp_port);
    g_pc_addr.sin_addr.s_addr = inet_addr(config->pc_ip_str);

    if (g_pc_addr.sin_addr.s_addr == INADDR_NONE) {
        ESP_LOGE(TAG, "❌ Invalid IP address: %s", config->pc_ip_str);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "📍 Target IP: %s (raw: 0x%08X)", config->pc_ip_str, g_pc_addr.sin_addr.s_addr);
    ESP_LOGI(TAG, "📍 Target Port: %d (0x%04X, network order: 0x%04X)", 
             config->udp_port, config->udp_port, g_pc_addr.sin_port);
    
    // Розбір IP на октети для перевірки
    uint8_t *ip_bytes = (uint8_t *)&g_pc_addr.sin_addr.s_addr;
    ESP_LOGI(TAG, "📍 IP bytes: %d.%d.%d.%d", ip_bytes[0], ip_bytes[1], ip_bytes[2], ip_bytes[3]);

    // Запуск слушающей задачи
        ESP_LOGI(TAG, "🔄 Step 4: Creating listener task...");
    BaseType_t task_created = xTaskCreatePinnedToCore(
        wifi_can_sniffer_task, 
        "wifi_can_sniffer", 
        4096, 
        NULL, 
        4,  // приоритет
        &g_wifi_task_handle,
        0   // CPU0
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "❌ Failed to create Wi-Fi CAN-Sniffer task");
            ESP_LOGI(TAG, "✅ Listener task created");
        return ESP_FAIL;
    }

    g_stat_time = xTaskGetTickCount();
    g_last_flush_time = xTaskGetTickCount();
    g_pc_connected = true;  // В режимі AP завжди готові відправляти

    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    ESP_LOGI(TAG, "✅ Wi-Fi CAN-Sniffer READY");
    ESP_LOGI(TAG, "📡 AP: %s (192.168.4.1)", config->wifi_ssid);
    ESP_LOGI(TAG, "🎯 Target: %s:%d", config->pc_ip_str, config->udp_port);
    ESP_LOGI(TAG, "═══════════════════════════════════════════════");
    return ESP_OK;
}

esp_err_t wifi_can_sniffer_send(uint32_t can_id, uint8_t dlc, const uint8_t *data)
{
    if (dlc > 8 || !data) {
        ESP_LOGW(TAG, "Invalid args: dlc=%d, data=%p", dlc, data);
        return ESP_ERR_INVALID_ARG;
    }
    
    if (g_udp_socket < 0) {
        ESP_LOGW(TAG, "UDP socket not initialized");
        return ESP_FAIL;
    }

    can_sniffer_packet_t packet = {
        .signature = swap_bytes_uint32(CAN_SNIFFER_SIGNATURE),
        .frame = {
            .id = can_id,
            .interval = 0,  // Интервал 0 для отдельных пакетов
            .length = dlc,
        },
    };

    // Копируем только нужные данные (без заполнения остального)
    memcpy(packet.frame.data, data, dlc);
    
    //ESP_LOGI(TAG, "📤 Sending CAN: ID=0x%03X, DLC=%d", can_id, dlc);
    
    return buffered_send(&packet);
}

esp_err_t wifi_can_sniffer_send_frame(const can_sniffer_frame_t *frame)
{
    if (!frame) {
        return ESP_ERR_INVALID_ARG;
    }

    can_sniffer_packet_t packet = {
        .signature = swap_bytes_uint32(CAN_SNIFFER_SIGNATURE),
        .frame = *frame,
    };

    return buffered_send(&packet);
}

esp_err_t wifi_can_sniffer_send_statistics(uint16_t fps, uint16_t bps)
{
    uint8_t data[4];
    *(uint16_t *)&data[0] = fps;
    *(uint16_t *)&data[2] = bps;
    
    return wifi_can_sniffer_send(CAN_SNIFFER_SERVICE_ID, 4, data);
}

bool wifi_can_sniffer_is_connected(void)
{
    return g_pc_connected;
}

void wifi_can_sniffer_flush(void)
{
    flush_udp_buffer();
}

void wifi_can_sniffer_deinit(void)
{
    if (g_wifi_task_handle) {
        vTaskDelete(g_wifi_task_handle);
        g_wifi_task_handle = NULL;
    }

    if (g_udp_socket >= 0) {
        close(g_udp_socket);
        g_udp_socket = -1;
    }

    esp_wifi_stop();
    ESP_LOGI(TAG, "Wi-Fi CAN-Sniffer deinitialized");
}
