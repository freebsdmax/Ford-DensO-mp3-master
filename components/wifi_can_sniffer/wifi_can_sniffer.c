#include "wifi_can_sniffer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "lwip/api.h"
#include <string.h>

static const char *TAG = "WiFiCanSniffer";

// Глобальний прапор для детальних логів (false - приховати зайві логи)
extern bool g_verbose_logs;
// Прапор для логів компонента
extern bool g_wifi_can_sniffer_logs;

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
// Флаг, показывающий, что сніфер був іниціалізований (включений) через wifi_can_sniffer_init()
static bool g_enabled = false;

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
static void start_sniffer_now(void);
static void refresh_destination(void);
static bool g_started = false;

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
        refresh_destination();
        // Start sniffer now if not already started and sniffer was explicitly enabled
        if (g_enabled) start_sniffer_now();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "STA connected to AP");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "STA disconnected; retrying...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "STA got IP: " IPSTR ", netmask: " IPSTR ", gw: " IPSTR,
                 IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.netmask), IP2STR(&ev->ip_info.gw));

        refresh_destination();
        // Start sniffer now if not already started and sniffer was explicitly enabled
        if (g_enabled) start_sniffer_now();
    }
}

/**
 * @brief Инициализация Wi-Fi в режиме AP
 */
static esp_err_t wifi_init_ap(const char *ssid, const char *password)
{
    if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "🔧 Initializing Wi-Fi AP...");
    
    // NOTE: TCP/IP stack and default event loop must be initialized by the application
    // (call esp_netif_init() and esp_event_loop_create_default() once in app_main).
    if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "ℹ️ Assuming netif and event loop already initialized by app");

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
        if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "✅ AP netif created");
    } else {
        if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "ℹ️ Using existing AP netif");
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

    // Ensure sniffer uses AP broadcast (192.168.4.255) immediately
    g_pc_addr.sin_family = AF_INET;
    g_pc_addr.sin_port = htons(g_config.udp_port);
    g_pc_addr.sin_addr.s_addr = inet_addr("192.168.4.255");
    g_pc_connected = true;
    ESP_LOGI(TAG, "Broadcast address set to: 192.168.4.255:%d", ntohs(g_pc_addr.sin_port));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret;
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    // РЕЄСТРАЦІЯ ОБРОБНИКІВ ПОДІЙ — робимо це тільки якщо сніфер був явнo ініціалізований
    // (щоб виклики wifi_can_switch_to_sta/ap не запускали сніфер автоматично)
    if (g_enabled) {
        ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) return ret;

        ret = esp_event_handler_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &wifi_event_handler, NULL);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) return ret;
    }

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

    if (g_verbose_logs) {
        ESP_LOGI(TAG, "Wi-Fi AP initialized");
        ESP_LOGI(TAG, "   SSID: %s", ssid);
        ESP_LOGI(TAG, "   Password: %s", password);
    }

    // AP is up — start sniffer only if it was explicitly enabled
    if (g_enabled) start_sniffer_now();

    return ESP_OK;
}

/**
 * @brief Перемикання Wi-Fi у режим STA та підключення до мережі з DHCP
 */
esp_err_t wifi_can_switch_to_sta(const char *ssid, const char *password)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "🔧 Switching Wi-Fi to STA mode...");

    esp_err_t ret;
    // Initialize netif and events (if not already initialized)
    // TCP/IP stack and default event loop should already be initialized by the application
    if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "ℹ️ Assuming netif and event loop already initialized by app");

    // Створити STA netif якщо відсутній
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) {
        if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "ℹ️ STA netif вже існує, використовуємо існуючий");
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
    if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "✅ Wi-Fi initialized (or already was)");

    // Реєстрація обробників подій
    // Register event handlers only if sniffer was enabled via init
    if (g_enabled) {
        ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) return ret; // ESP_ERR_INVALID_ARG means already registered
        ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_ARG) return ret;
    }

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

    if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "📶 STA connecting to SSID: %s", ssid);
    return ESP_OK;
}

/**
 * @brief Перемкнути Wi-Fi у режим AP з заданим SSID/паролем
 */
esp_err_t wifi_can_switch_to_ap(const char *ssid, const char *password)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;
    if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "🔧 Switching Wi-Fi to AP mode...");

    // Повністю деініціалізувати Wi-Fi перед переключенням
    esp_wifi_stop();
    esp_wifi_deinit();
    if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "🔧 Wi-Fi deinitialized");

    // Небольшая задержка для полной очистки
    vTaskDelay(pdMS_TO_TICKS(100));

    // Перейти в AP
    esp_err_t ret = wifi_init_ap(ssid, password);
    if (ret == ESP_OK) {
        if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "📶 AP started: %s", ssid);
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
    // If sniffer disabled — do nothing silently
    if (!g_enabled) return;

    if (g_udp_socket < 0) {
        // socket closed or not initialized — silently drop
        g_udp_out_pos = g_udp_out_buffer;
        g_last_flush_time = xTaskGetTickCount();
        return;
    }

    // If no known peer — drop buffered data
    if (!g_pc_connected) {
        g_udp_out_pos = g_udp_out_buffer;
        g_last_flush_time = xTaskGetTickCount();
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
 * @brief Pick the UDP destination.
 *
 *   1. STA subnet broadcast  (device joined a home network -> PC is there);
 *   2. AP subnet broadcast   192.168.4.255;
 *   3. configured pc_ip_str.
 * The address used to be hard-coded to 192.168.4.255, so in STA mode the
 * datagrams never reached the PC.
 */
static void refresh_destination(void)
{
    esp_netif_ip_info_t info;

    g_pc_addr.sin_family = AF_INET;
    g_pc_addr.sin_port = htons(g_config.udp_port);

    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta && esp_netif_get_ip_info(sta, &info) == ESP_OK &&
        info.ip.addr != 0 && info.netmask.addr != 0) {
        uint32_t ip = ntohl(info.ip.addr);
        uint32_t mask = ntohl(info.netmask.addr);
        g_pc_addr.sin_addr.s_addr = htonl((ip & mask) | ~mask);
        ESP_LOGI(TAG, "UDP destination: STA broadcast %s:%u",
                 inet_ntoa(g_pc_addr.sin_addr), ntohs(g_pc_addr.sin_port));
        return;
    }

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap && esp_netif_get_ip_info(ap, &info) == ESP_OK && info.ip.addr != 0) {
        g_pc_addr.sin_addr.s_addr = inet_addr("192.168.4.255");
        ESP_LOGI(TAG, "UDP destination: AP broadcast %s:%u",
                 inet_ntoa(g_pc_addr.sin_addr), ntohs(g_pc_addr.sin_port));
        return;
    }

    g_pc_addr.sin_addr.s_addr = inet_addr(g_config.pc_ip_str ? g_config.pc_ip_str : "192.168.4.255");
    ESP_LOGI(TAG, "UDP destination: configured %s:%u",
             inet_ntoa(g_pc_addr.sin_addr), ntohs(g_pc_addr.sin_port));
}

static void start_sniffer_now(void)
{
    /* destination depends on the network we are actually on (AP or STA) */
    refresh_destination();
    if (g_started) return;
    g_started = true;

    ESP_LOGI(TAG, "🔌 Starting sniffer now (event-driven)");

    if (create_udp_socket(g_config.udp_port) != ESP_OK) {
        ESP_LOGE(TAG, "❌ UDP socket creation failed in start_sniffer_now");
        g_started = false;
        return;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(
        wifi_can_sniffer_task,
        "wifi_can_sniffer",
        4096,
        NULL,
        4,
        &g_wifi_task_handle,
        0
    );

    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "❌ Failed to create Wi-Fi CAN-Sniffer listener task");
        close(g_udp_socket);
        g_udp_socket = -1;
        g_started = false;
        return;
    }

    g_stat_time = xTaskGetTickCount();
    g_last_flush_time = xTaskGetTickCount();
    ESP_LOGI(TAG, "✅ Wi-Fi CAN-Sniffer READY (event-driven)");
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
    memcpy(&g_config, config, sizeof(wifi_can_sniffer_config_t));

    // Инициализация Wi-Fi: не создаём свою AP, используем AP, созданную плеером
    ESP_LOGI(TAG, "📡 Step 1: Using player's AP (will NOT create its own AP)...");
    esp_err_t ret = ESP_OK;
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        ESP_LOGI(TAG, "ℹ️ Found existing AP netif; will use existing AP");
    } else {
        ESP_LOGW(TAG, "⚠️ AP netif not found. The player is expected to create the AP later.");
    }

    // TCP/IP stack and default event loop must be initialized by the main application
    if (g_verbose_logs && g_wifi_can_sniffer_logs) ESP_LOGI(TAG, "ℹ️ Assuming esp_netif and default event loop are already initialized by app_main");

    // Register for IP events so we can start when IP is obtained (STA) or AP assigns IP
    esp_err_t r = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (r != ESP_OK && r != ESP_ERR_INVALID_ARG) {
        ESP_LOGW(TAG, "⚠️ Failed to register IP event handler: %s", esp_err_to_name(r));
    }

    // If AP already exists, set broadcast and start immediately
    if (ap_netif) {
        refresh_destination();
        g_pc_connected = true;
        ESP_LOGI(TAG, "AP present at init; sniffer will start only if enabled via web/init");
    } else {
        ESP_LOGI(TAG, "ℹ️ AP will be created by player; sniffer will start when IP event occurs");
    }

    // Mark as enabled now that init completed
    g_enabled = true;

    // If AP already existed at init, start sniffer now (we've just been enabled)
    if (ap_netif) {
        start_sniffer_now();
    }

    return ESP_OK;
}

/**
 * @brief Откладенный стартер: ждём 15 секунд, затем инициализируем сокет и слушатель.
 */
static void wifi_can_sniffer_startup_task(void *pvParameters)
{
    (void)pvParameters;
    // This function no longer used; kept to satisfy any references but does nothing.
    vTaskDelete(NULL);
}

esp_err_t wifi_can_sniffer_send(uint32_t can_id, uint8_t dlc, const uint8_t *data)
{
    if (dlc > 8 || !data) {
        ESP_LOGW(TAG, "Invalid args: dlc=%d, data=%p", dlc, data);
        return ESP_ERR_INVALID_ARG;
    }
    // If sniffer disabled, silently fail
    if (!g_enabled) return ESP_FAIL;
    if (g_udp_socket < 0) {
        // socket not initialized — silently fail
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
    // Mark disabled so event handler won't restart sniffer
    g_enabled = false;
    g_started = false;
    g_pc_connected = false;

    // Clear outgoing buffer
    memset(g_udp_out_buffer, 0, UDP_BUFFER_SIZE);
    g_udp_out_pos = g_udp_out_buffer;

    // Unregister event handlers if they were registered
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);

    if (g_wifi_task_handle) {
        vTaskDelete(g_wifi_task_handle);
        g_wifi_task_handle = NULL;
    }

    if (g_udp_socket >= 0) {
        close(g_udp_socket);
        g_udp_socket = -1;
    }

    // Do NOT stop system Wi-Fi here (it may be used by the main application)
    ESP_LOGI(TAG, "Wi-Fi CAN-Sniffer deinitialized");
}
