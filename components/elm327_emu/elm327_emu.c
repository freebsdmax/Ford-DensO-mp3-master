#include "elm327_emu.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "driver/twai.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

static const char *TAG = "ELM327_EMU";

#define ELM327_NVS_NAMESPACE "elm327"
#define ELM327_TCP_PORT 35000
#define ELM327_BUFFER_SIZE 256
#define ELM327_MAX_CLIENTS 2

// NVS keys
#define NVS_KEY_ENABLED "enabled"
#define NVS_KEY_WIFI_ENABLED "wifi_en"
#define NVS_KEY_BT_ENABLED "bt_en"

// Global state
static bool s_elm327_running = false;
static elm327_config_t s_config = {
    .enabled = false,
    .wifi_enabled = false,
    .bt_enabled = false
};

static TaskHandle_t s_tcp_server_task = NULL;
static TaskHandle_t s_bt_task = NULL;

// SPP state
static uint32_t s_spp_server_handle = 0;
static bool s_spp_connected = false;
static uint32_t s_spp_handle = 0;
static uint8_t s_spp_sec_mask = ESP_SPP_SEC_NONE;
static uint8_t s_spp_role_slave = ESP_SPP_ROLE_SLAVE;

// Forward declarations
static void elm327_tcp_server_task(void *pvParameters);
static void elm327_process_line(char *line, int client_sock, bool is_bt);
static void elm327_handle_at_command(char *cmd, char *response, size_t resp_size);
static void elm327_handle_obd_request(char *req, char *response, size_t resp_size);
static void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
static void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

/**
 * @brief Load configuration from NVS
 */
esp_err_t elm327_emu_load_config(elm327_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(ELM327_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS namespace not found, using defaults (all disabled)");
        config->enabled = false;
        config->wifi_enabled = false;
        config->bt_enabled = false;
        return ESP_OK;
    }

    uint8_t enabled = 0;
    uint8_t wifi_en = 0;
    uint8_t bt_en = 0;

    nvs_get_u8(nvs_handle, NVS_KEY_ENABLED, &enabled);
    nvs_get_u8(nvs_handle, NVS_KEY_WIFI_ENABLED, &wifi_en);
    nvs_get_u8(nvs_handle, NVS_KEY_BT_ENABLED, &bt_en);

    nvs_close(nvs_handle);

    config->enabled = enabled != 0;
    config->wifi_enabled = wifi_en != 0;
    config->bt_enabled = bt_en != 0;

    ESP_LOGI(TAG, "Loaded config from NVS: enabled=%d, wifi=%d, bt=%d", 
             config->enabled, config->wifi_enabled, config->bt_enabled);

    return ESP_OK;
}

/**
 * @brief Save configuration to NVS
 */
static esp_err_t elm327_emu_save_config(const elm327_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(ELM327_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(nvs_handle, NVS_KEY_ENABLED, config->enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs_handle, NVS_KEY_WIFI_ENABLED, config->wifi_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(nvs_handle, NVS_KEY_BT_ENABLED, config->bt_enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs_handle);

    nvs_close(nvs_handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved config to NVS: enabled=%d, wifi=%d, bt=%d", 
                 config->enabled, config->wifi_enabled, config->bt_enabled);
    } else {
        ESP_LOGE(TAG, "Failed to save config: %s", esp_err_to_name(err));
    }

    return err;
}

/**
 * @brief Get current configuration
 */
esp_err_t elm327_emu_get_config(elm327_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    *config = s_config;
    return ESP_OK;
}

/**
 * @brief Set and apply configuration
 */
esp_err_t elm327_emu_set_config(const elm327_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    bool was_running = s_elm327_running;
    
    // Stop if running
    if (s_elm327_running) {
        elm327_emu_stop();
    }

    s_config = *config;
    esp_err_t err = elm327_emu_save_config(config);
    if (err != ESP_OK) {
        return err;
    }

    // Start if enabled
    if (config->enabled && (config->wifi_enabled || config->bt_enabled)) {
        err = elm327_emu_start(config);
    }

    return ESP_OK;
}

/**
 * @brief Initialize Bluetooth Classic SPP
 */
static esp_err_t elm327_init_bt_spp(void)
{
    if (!s_config.bt_enabled) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Bluetooth Classic SPP for ELM327 emulation");

    // BT already initialized in ble_player_service? Check if BT controller is enabled
    esp_err_t ret = esp_spp_register_callback(spp_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPP register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_spp_init(ESP_SPP_MODE_CB);

    esp_bt_gap_register_callback(bt_gap_callback);
    esp_bt_gap_set_device_name("OBDLink-EX");
    
    // Make device discoverable and connectable
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    ret = esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, "SPP_SERVER");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPP start server failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Bluetooth Classic SPP initialized, device name: OBDLink-EX");
    return ESP_OK;
}

/**
 * @brief SPP callback
 */
static void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
        case ESP_SPP_INIT_EVT:
            ESP_LOGI(TAG, "SPP initialized");
            break;
        case ESP_SPP_START_EVT:
            ESP_LOGI(TAG, "SPP server started, handle=%d", param->start.status);
            s_spp_server_handle = param->start.handle;
            break;
        case ESP_SPP_SRV_OPEN_EVT:
            ESP_LOGI(TAG, "SPP connection opened, handle=%d", param->srv_open.handle);
            s_spp_connected = true;
            s_spp_handle = param->srv_open.handle;
            break;
        case ESP_SPP_CLOSE_EVT:
            ESP_LOGI(TAG, "SPP connection closed");
            s_spp_connected = false;
            s_spp_handle = 0;
            break;
        case ESP_SPP_DATA_IND_EVT:
            if (param->data_ind.len > 0) {
                char line[ELM327_BUFFER_SIZE];
                size_t len = param->data_ind.len;
                if (len >= ELM327_BUFFER_SIZE) len = ELM327_BUFFER_SIZE - 1;
                memcpy(line, param->data_ind.data, len);
                line[len] = '\0';
                
                // Remove CR/LF
                for (int i = 0; i < len; i++) {
                    if (line[i] == '\r' || line[i] == '\n') {
                        line[i] = '\0';
                        break;
                    }
                }
                
                if (strlen(line) > 0) {
                    elm327_process_line(line, -1, true); // BT mode
                }
            }
            break;
        default:
            break;
    }
}

/**
 * @brief BT GAP callback
 */
static void bt_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            ESP_LOGI(TAG, "BT authentication completed");
            break;
        default:
            break;
    }
}

/**
 * @brief Handle AT commands
 */
static void elm327_handle_at_command(char *cmd, char *response, size_t resp_size)
{
    // Convert to uppercase for comparison
    char upper_cmd[ELM327_BUFFER_SIZE];
    size_t len = strlen(cmd);
    if (len >= ELM327_BUFFER_SIZE) len = ELM327_BUFFER_SIZE - 1;
    for (size_t i = 0; i < len; i++) {
        upper_cmd[i] = toupper((unsigned char)cmd[i]);
    }
    upper_cmd[len] = '\0';

    if (strcmp(upper_cmd, "Z") == 0 || strncmp(upper_cmd, "ATZ", 2) == 0) {
        snprintf(response, resp_size, "ELM327 v1.5\r\r>");
        return;
    }
    if (strcmp(upper_cmd, "I") == 0 || strncmp(upper_cmd, "ATI", 2) == 0) {
        snprintf(response, resp_size, "ELM327 v1.5\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "ATE", 2) == 0) {
        // Echo on/off - acknowledge
        snprintf(response, resp_size, "OK\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "ATL", 2) == 0) {
        snprintf(response, resp_size, "OK\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "ATH", 2) == 0) {
        snprintf(response, resp_size, "OK\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "ATS", 2) == 0) {
        snprintf(response, resp_size, "OK\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "ATSP", 3) == 0) {
        snprintf(response, resp_size, "OK\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "ATDP", 3) == 0) {
        snprintf(response, resp_size, "AUTO, ISO 15765-4 (CAN 11/29 bit ID, 500 kbaud)\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "ATDPN", 4) == 0) {
        snprintf(response, resp_size, "A6\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "ATCAF", 4) == 0) {
        snprintf(response, resp_size, "OK\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "ATRV", 3) == 0) {
        snprintf(response, resp_size, "14.2V\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "ATD", 2) == 0 && strlen(upper_cmd) == 2) {
        snprintf(response, resp_size, "OK\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "AT@1", 3) == 0 || strncmp(upper_cmd, "AT@2", 3) == 0) {
        snprintf(response, resp_size, "OBDLink-EX\r\r>");
        return;
    }
    if (strncmp(upper_cmd, "AT", 2) == 0) {
        snprintf(response, resp_size, "OK\r\r>");
        return;
    }

    snprintf(response, resp_size, "?\r\r>");
}

/**
 * @brief Handle OBD requests
 */
static void elm327_handle_obd_request(char *req, char *response, size_t resp_size)
{
    // Basic OBD-II request handling
    // For FordScan compatibility - mostly pass through or return NO DATA
    // Since user said "I won't search, I'll use FordScan for diagnostics"
    // We'll try to handle common PIDs minimally
    
    // Remove spaces if any
    char clean_req[ELM327_BUFFER_SIZE];
    int j = 0;
    for (int i = 0; req[i] != '\0' && j < ELM327_BUFFER_SIZE - 1; i++) {
        if (req[i] != ' ') {
            clean_req[j++] = toupper((unsigned char)req[i]);
        }
    }
    clean_req[j] = '\0';

    // Mode 01 - Current Data
    if (strncmp(clean_req, "01", 2) == 0 && strlen(clean_req) >= 4) {
        // For now return NO DATA - FordScan will handle via CAN translation if needed
        // This is a basic implementation
        snprintf(response, resp_size, "NO DATA\r\r>");
        return;
    }

    // Mode 09 - Vehicle Information
    if (strncmp(clean_req, "09", 2) == 0) {
        snprintf(response, resp_size, "NO DATA\r\r>");
        return;
    }

    // UDS/extended requests
    snprintf(response, resp_size, "NO DATA\r\r>");
}

/**
 * @brief Process a single line from client
 */
static void elm327_process_line(char *line, int client_sock, bool is_bt)
{
    char response[ELM327_BUFFER_SIZE];
    response[0] = '\0';

    if (strlen(line) == 0) {
        snprintf(response, sizeof(response), "\r>");
        goto send_response;
    }

    // Check if it's an AT command
    if (line[0] == 'A' && (line[1] == 'T' || line[1] == 't')) {
        elm327_handle_at_command(line, response, sizeof(response));
    } else {
        // Assume OBD request
        elm327_handle_obd_request(line, response, sizeof(response));
    }

send_response:
    if (strlen(response) == 0) {
        snprintf(response, sizeof(response), "?\r\r>");
    }

    if (is_bt && s_spp_connected && s_spp_handle != 0) {
        /* BT SPP not supported in this build */
    } else if (client_sock >= 0) {
        send(client_sock, response, strlen(response), 0);
    }
}

/**
 * @brief TCP server task
 */
static void elm327_tcp_server_task(void *pvParameters)
{
    (void)pvParameters;

    struct sockaddr_in server_addr, client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int server_sock = -1;
    int client_sock = -1;

    server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "Failed to create TCP socket");
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(ELM327_TCP_PORT);

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind TCP socket");
        close(server_sock);
        vTaskDelete(NULL);
        return;
    }

    if (listen(server_sock, ELM327_MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "Failed to listen on TCP socket");
        close(server_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "ELM327 TCP server started on port %d", ELM327_TCP_PORT);

    while (s_elm327_running && s_config.wifi_enabled) {
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(server_sock, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int activity = select(server_sock + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) {
            break;
        }
        if (activity == 0) {
            continue;
        }

        if (FD_ISSET(server_sock, &readfds)) {
            client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &addr_len);
            if (client_sock >= 0) {
                ESP_LOGI(TAG, "ELM327 client connected from %s:%d", 
                         inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                
                char buffer[ELM327_BUFFER_SIZE];
                while (s_elm327_running && s_config.wifi_enabled) {
                    int len = recv(client_sock, buffer, ELM327_BUFFER_SIZE - 1, 0);
                    if (len <= 0) {
                        break;
                    }
                    buffer[len] = '\0';
                    
                    // Process line by line
                    char *line = strtok(buffer, "\r\n");
                    while (line != NULL) {
                        if (strlen(line) > 0) {
                            elm327_process_line(line, client_sock, false);
                        }
                        line = strtok(NULL, "\r\n");
                    }
                }
                
                ESP_LOGI(TAG, "ELM327 client disconnected");
                close(client_sock);
                client_sock = -1;
            }
        }
    }

    if (client_sock >= 0) close(client_sock);
    if (server_sock >= 0) close(server_sock);
    s_tcp_server_task = NULL;
    vTaskDelete(NULL);
}

/**
 * @brief Start ELM327 emulator
 */
esp_err_t elm327_emu_start(const elm327_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_elm327_running) {
        ESP_LOGW(TAG, "ELM327 emulator already running");
        return ESP_OK;
    }

    s_config = *config;

    if (!s_config.enabled || (!s_config.wifi_enabled && !s_config.bt_enabled)) {
        ESP_LOGI(TAG, "ELM327 emulator disabled or no transports enabled");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting ELM327 emulator...");

    if (s_config.wifi_enabled) {
        xTaskCreatePinnedToCore(elm327_tcp_server_task, "elm327_tcp", 4096, NULL, 5, &s_tcp_server_task, 0);
        ESP_LOGI(TAG, "Wi-Fi TCP server task started on port %d", ELM327_TCP_PORT);
    }
    if (s_config.bt_enabled) {
        ESP_LOGW(TAG, "BT Classic SPP not supported in this build");
    }

    s_elm327_running = true;
    ESP_LOGI(TAG, "ELM327 emulator started successfully");
    return ESP_OK;
}

/**
 * @brief Stop ELM327 emulator
 */
esp_err_t elm327_emu_stop(void)
{
    if (!s_elm327_running) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping ELM327 emulator...");
    s_elm327_running = false;

    if (s_tcp_server_task != NULL) {
        vTaskDelete(s_tcp_server_task);
        s_tcp_server_task = NULL;
    }

    if (s_spp_connected && s_spp_handle != 0) {
        s_spp_connected = false;
        s_spp_handle = 0;
    }

    ESP_LOGI(TAG, "ELM327 emulator stopped");
    return ESP_OK;
}
