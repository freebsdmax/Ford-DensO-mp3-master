#include "elm327_wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"
#include "driver/twai.h"
#include "driver/gpio.h"
#include <ctype.h>

static const char *TAG = "ELM327_WIFI";

// Глобальний прапор для детальних логів (false - приховати зайві логи)
extern bool g_verbose_logs;
extern bool g_elm327_logs;

// --- ELM327 Configuration ---
#define ELM327_VERSION "ELM327 v2.1e2"
#define ELM327_PROMPT ">"

// --- TCP Server Configuration ---
#define MAX_CLIENTS 1
static int server_sock = -1;
static int client_sock = -1;
static TaskHandle_t elm327_server_task_handle = NULL;

// --- Command Processing ---
#define CMD_BUFFER_SIZE 256
#define RSP_BUFFER_SIZE 512
static char cmd_buffer[CMD_BUFFER_SIZE];
static size_t cmd_len = 0;

// --- ELM327 Internal State ---
typedef enum {
    STATE_IDLE,
    STATE_CAN_MONITOR
} elm327_mode_t;

static elm327_mode_t current_mode = STATE_IDLE;
static bool linefeeds_enabled = true;
static bool echo_enabled = false;
static bool spaces_enabled = true;

// --- CAN/OBD State ---
// NOTE: Must be modified for your specific hardware.
// Common pins are GPIO 5/4, 21/22.
#define TWAI_TX_GPIO_NUM GPIO_NUM_4
#define TWAI_RX_GPIO_NUM GPIO_NUM_15

static int current_protocol = 6; // Default to ISO 15765-4 (CAN 11/500)
static uint16_t obd_timeout = 200; // Default timeout 200ms (per ELM327 datasheet)
static uint16_t voltage_threshold = 1200; // 12.00V default
static uint32_t can_header = 0x7DF; // Default CAN header
static uint32_t can_filter = 0x000; // Default CAN filter (accept all)
static uint32_t can_mask = 0x000;   // Default CAN mask (accept all)
static twai_timing_config_t can_timing = TWAI_TIMING_CONFIG_500KBITS(); // Default to 500kbit/s

static const char *protocol_names[] = {
    "AUTO", "SAE J1850 PWM", "SAE J1850 VPW", "ISO 9141-2", "ISO 14230-4 (KWP 5BAUD)",
    "ISO 14230-4 (KWP FAST)", "ISO 15765-4 (CAN 11/500)", "ISO 15765-4 (CAN 29/500)",
    "ISO 15765-4 (CAN 11/250)", "ISO 15765-4 (CAN 29/250)", "SAE J1939 (CAN 29/250)",
    "USER1 (CAN 11/125)", "USER2 (CAN 11/500)"
};

// Forward declarations
static esp_err_t configure_twai_driver();
static void reset_elm_defaults();
static void elm327_server_task(void *arg);
static void process_incoming_data(const char *data, size_t len);
static void process_command(const char *cmd);
static esp_err_t send_response(const char *response);
static esp_err_t send_prompt();
static esp_err_t send_can_frame_to_client(const twai_message_t *message);
static esp_err_t obd_query(const char *cmd_hex, char *response_buffer, size_t max_len, char *can_info, size_t can_info_len);
static esp_err_t send_can_message(const twai_message_t *message);
static void handle_client_disconnection();


// --- CAN Driver Configuration ---
static esp_err_t configure_twai_driver() {
    // Stop and uninstall the driver if it's already running
    twai_stop();
    twai_driver_uninstall();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_GPIO_NUM, TWAI_RX_GPIO_NUM, TWAI_MODE_NORMAL);
    g_config.alerts_enabled = TWAI_ALERT_NONE;
    
    twai_filter_config_t f_config;
    f_config.single_filter = true;

    // The ELM327 mask is a "must match" mask. The ESP-IDF mask is a "don't care" mask (1 = don't care).
    // Therefore, we must invert the mask. We also need to shift for Standard vs Extended IDs.
    bool is_29bit = (current_protocol == 7 || current_protocol == 9 || current_protocol == 10);
    
    if (is_29bit) {
        f_config.acceptance_code = can_filter & 0x1FFFFFFF;
        f_config.acceptance_mask = (~can_mask) & 0x1FFFFFFF;
    } else { // 11-bit ID
        f_config.acceptance_code = (can_filter & 0x7FF) << 21;
        f_config.acceptance_mask = (~(can_mask & 0x7FF)) << 21;
    }

    if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "Reconfiguring TWAI driver. Protocol=%d, Filter=0x%08X, Mask=0x%08X", current_protocol, (unsigned int)f_config.acceptance_code, (unsigned int)f_config.acceptance_mask);

    if (twai_driver_install(&g_config, &can_timing, &f_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TWAI driver");
        return ESP_FAIL;
    }
    if (twai_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start TWAI driver");
        return ESP_FAIL;
    }

    vTaskDelay(pdMS_TO_TICKS(50)); // Allow TWAI to stabilize after reconfiguration

    if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "TWAI driver configured and started successfully.");
    return ESP_OK;
}

static void reset_elm_defaults() {
    linefeeds_enabled = true;
    echo_enabled = false;
    spaces_enabled = true;
    current_protocol = 6;
    obd_timeout = 200;
    can_header = 0x7DF;
    can_filter = 0x000;
    can_mask = 0x000;
    twai_timing_config_t timing_500k = TWAI_TIMING_CONFIG_500KBITS();
    can_timing = timing_500k;
    configure_twai_driver();
}


// --- Public API ---

esp_err_t elm327_wifi_init(uint16_t port) {
    if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "Initializing ELM327 WiFi Server on port %d...", port);
    
    if (configure_twai_driver() != ESP_OK) {
        ESP_LOGE(TAG, "Initial TWAI configuration failed. Aborting.");
        return ESP_FAIL;
    }

    server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "Failed to create server socket");
        return ESP_FAIL;
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind server socket: %s", strerror(errno));
        close(server_sock);
        server_sock = -1;
        return ESP_FAIL;
    }

    if (listen(server_sock, MAX_CLIENTS) < 0) {
        ESP_LOGE(TAG, "Failed to listen on server socket: %s", strerror(errno));
        close(server_sock);
        server_sock = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(elm327_server_task, "elm327_server", 8192, NULL, 5, &elm327_server_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ELM327 server task");
        close(server_sock);
        server_sock = -1;
        return ESP_FAIL;
    }

    if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "ELM327 WiFi Server initialized.");
    return ESP_OK;
}

esp_err_t elm327_wifi_deinit(void) {
    if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "Deinitializing ELM327 WiFi Server...");

    if (elm327_server_task_handle) {
        vTaskDelete(elm327_server_task_handle);
        elm327_server_task_handle = NULL;
    }

    if (client_sock >= 0) {
        close(client_sock);
        client_sock = -1;
    }

    if (server_sock >= 0) {
        close(server_sock);
        server_sock = -1;
    }

    twai_stop();
    twai_driver_uninstall();

    if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "ELM327 WiFi Server deinitialized.");
    return ESP_OK;
}

bool elm327_wifi_is_connected(void) {
    return client_sock >= 0;
}

// --- Private Functions ---

static void elm327_server_task(void *arg) {
    if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "ELM327 server task started");

    while (1) {
        if (client_sock < 0) {
            if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "Waiting for new ELM327 client connection...");
            struct sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);
            client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);

            if (client_sock < 0) {
                ESP_LOGE(TAG, "Failed to accept new client: %s", strerror(errno));
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "New ELM327 client connected from %s:%d", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            
            int flags = fcntl(client_sock, F_GETFL, 0);
            fcntl(client_sock, F_SETFL, flags | O_NONBLOCK);

            cmd_len = 0;
            current_mode = STATE_IDLE;
            send_response(ELM327_VERSION);
            send_prompt();
        }

        char buffer[128];
        ssize_t len = recv(client_sock, buffer, sizeof(buffer), 0);

        if (len > 0) {
            process_incoming_data(buffer, len);
        } else if (len == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            ESP_LOGW(TAG, "Client disconnected or recv error (len=%zd, errno=%d)", len, errno);
            handle_client_disconnection();
            continue;
        }

        if (current_mode == STATE_CAN_MONITOR) {
            twai_message_t can_message;
            if (twai_receive(&can_message, 0) == ESP_OK) {
                if (send_can_frame_to_client(&can_message) != ESP_OK) {
                    continue;
                }
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void handle_client_disconnection() {
    if (client_sock < 0) return;

    if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "Handling client disconnection.");
    shutdown(client_sock, SHUT_RDWR);
    close(client_sock);
    
    client_sock = -1;
    current_mode = STATE_IDLE;
    cmd_len = 0; 
}

static void process_incoming_data(const char *data, size_t len) {
    if (cmd_len + len >= CMD_BUFFER_SIZE) {
        ESP_LOGE(TAG, "Command buffer overflow, discarding incoming data.");
        cmd_len = 0;
        return;
    }

    memcpy(cmd_buffer + cmd_len, data, len);
    cmd_len += len;
    cmd_buffer[cmd_len] = '\0';

    if (current_mode == STATE_CAN_MONITOR) {
        bool stop_monitoring = false;
        for (size_t i = 0; i < len; ++i) {
            if (data[i] != '\r' && data[i] != '\n') {
                stop_monitoring = true;
                break;
            }
        }
        if (stop_monitoring) {
            current_mode = STATE_IDLE;
            send_response("STOPPED\r");
            if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "CAN monitoring stopped by new command.");
        }
    }

    char *cmd_start = cmd_buffer;
    char *terminator;
    while ((terminator = strpbrk(cmd_start, "\r\n")) != NULL) {
        *terminator = '\0';
        
        if (strlen(cmd_start) > 0) {
            if (echo_enabled) {
                char echo_buf[CMD_BUFFER_SIZE + 2];
                snprintf(echo_buf, sizeof(echo_buf), "%s\r", cmd_start);
                send_response(echo_buf);
            }
            process_command(cmd_start);
        }

        cmd_start = terminator + 1;
    }

    size_t remaining_len = strlen(cmd_start);
    memmove(cmd_buffer, cmd_start, remaining_len);
    cmd_len = remaining_len;
    cmd_buffer[cmd_len] = '\0';
}

static void process_command(const char *cmd) {
    char response[RSP_BUFFER_SIZE] = "?\r";
    char upper_cmd[CMD_BUFFER_SIZE];
    
    size_t j = 0;
    for (size_t i = 0; cmd[i] && j < sizeof(upper_cmd) - 1; ++i) {
        if (!isspace((unsigned char)cmd[i])) {
            upper_cmd[j++] = toupper((unsigned char)cmd[i]);
        }
    }
    upper_cmd[j] = '\0';
    
    if (strlen(upper_cmd) == 0) {
        send_prompt();
        return;
    }
    
    char can_info[128] = "";
    if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "⚙️ AT: %s", upper_cmd);

    if (strncmp(upper_cmd, "AT", 2) == 0) {
        const char *at_cmd = upper_cmd + 2;
        if (strcmp(at_cmd, "I") == 0) snprintf(response, sizeof(response), "%s\r", ELM327_VERSION);
        else if (strcmp(at_cmd, "Z") == 0) {
             reset_elm_defaults();
             snprintf(response, sizeof(response), "\r\r%s\r", ELM327_VERSION);
        }
        else if (strcmp(at_cmd, "E0") == 0) { echo_enabled = false; strcpy(response, "OK\r"); }
        else if (strcmp(at_cmd, "E1") == 0) { echo_enabled = true; strcpy(response, "OK\r"); }
        else if (strcmp(at_cmd, "L0") == 0) { linefeeds_enabled = false; strcpy(response, "OK\r"); }
        else if (strcmp(at_cmd, "L1") == 0) { linefeeds_enabled = true; strcpy(response, "OK\r"); }
        else if (strcmp(at_cmd, "S0") == 0) { spaces_enabled = false; strcpy(response, "OK\r"); }
        else if (strcmp(at_cmd, "S1") == 0) { spaces_enabled = true; strcpy(response, "OK\r"); }
        else if (strcmp(at_cmd, "D") == 0) { reset_elm_defaults(); strcpy(response, "OK\r"); }
        else if (strcmp(at_cmd, "DP") == 0) snprintf(response, sizeof(response), "%s\r", protocol_names[current_protocol]);
        else if (strcmp(at_cmd, "DPN") == 0) snprintf(response, sizeof(response), "%X\r", current_protocol);
        else if (strncmp(at_cmd, "SP", 2) == 0) {
            int proto = -1;
            if (strlen(at_cmd + 2) > 0) proto = (int)strtol(at_cmd + 2, NULL, 16);

            if (proto >= 6 && proto <= 12) {
                current_protocol = proto;
                if (proto == 6 || proto == 7 || proto == 12) {
                    twai_timing_config_t timing_500k = TWAI_TIMING_CONFIG_500KBITS();
                    can_timing = timing_500k;
                } else if (proto == 8 || proto == 9 || proto == 10) {
                    twai_timing_config_t timing_250k = TWAI_TIMING_CONFIG_250KBITS();
                    can_timing = timing_250k;
                } else if (proto == 11) {
                    twai_timing_config_t timing_125k = TWAI_TIMING_CONFIG_125KBITS();
                    can_timing = timing_125k;
                }
                
                if (configure_twai_driver() == ESP_OK) strcpy(response, "OK\r");
            }
        }
        else if (strcmp(at_cmd, "MA") == 0) {
            twai_status_info_t status;
            if (twai_get_status_info(&status) == ESP_OK && status.state == TWAI_STATE_RUNNING) {
                current_mode = STATE_CAN_MONITOR;
                if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "Starting CAN monitoring.");
                return;
            } else {
                strcpy(response, "NO CAN\r");
            }
        }
        else if (strncmp(at_cmd, "SH", 2) == 0 && strlen(at_cmd) > 2) {
            long val = strtol(at_cmd + 2, NULL, 16);
            can_header = val;
            if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "🔧 Встановлено CAN header: 0x%08lX", val);
            strcpy(response, "OK\r");
        }
        else if (strcmp(at_cmd, "CRA") == 0) { // Reset filters
             can_filter = 0x0;
             can_mask = 0x0;
             if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "🔧 Скинуто CAN фільтри");
             if(configure_twai_driver() == ESP_OK) strcpy(response, "OK\r");
        }
        else if (strncmp(at_cmd, "CRA", 3) == 0 && strlen(at_cmd) > 3) {
            long val = strtol(at_cmd + 3, NULL, 16);
            can_filter = val;
            can_mask = (current_protocol == 7 || current_protocol == 9 || current_protocol == 10) ? 0x1FFFFFFF : 0x7FF;
            if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "🔧 Встановлено CAN receive address: 0x%08lX", val);
            if(configure_twai_driver() == ESP_OK) strcpy(response, "OK\r");
        }
        else if (strncmp(at_cmd, "CF", 2) == 0 && strlen(at_cmd) > 2) {
            long val = strtol(at_cmd + 2, NULL, 16);
            can_filter = val;
            if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "🔧 Встановлено CAN filter: 0x%08lX", can_filter);
            if(configure_twai_driver() == ESP_OK) strcpy(response, "OK\r");
        }
        else if (strncmp(at_cmd, "CM", 2) == 0 && strlen(at_cmd) > 2) {
            long val = strtol(at_cmd + 2, NULL, 16);
            can_mask = val;
            if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "🔧 Встановлено CAN mask: 0x%08lX", can_mask);
            if(configure_twai_driver() == ESP_OK) strcpy(response, "OK\r");
        }
        else if (strncmp(at_cmd, "ST", 2) == 0 && strlen(at_cmd) == 4) {
            char *endptr;
            long val = strtol(at_cmd + 2, &endptr, 16);
            if (*endptr == '\0' && val >= 0 && val <= 255) {
                obd_timeout = val * 4;
                if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "⏱️ Встановлено таймаут: %d мс", obd_timeout);
                strcpy(response, "OK\r");
            } else {
                strcpy(response, "?\r");
            }
        }
        else if (strncmp(at_cmd, "VT", 2) == 0 && strlen(at_cmd) > 2) {
            char *endptr;
            long val = strtol(at_cmd + 2, &endptr, 10);
            if (*endptr == '\0' && val >= 0 && val <= 2000) {
                voltage_threshold = val;
                if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "🔋 Встановлено поріг напруги: %d.%02d V", val / 100, val % 100);
                strcpy(response, "OK\r");
            } else {
                strcpy(response, "?\r");
            }
        }
        else if (strcmp(at_cmd, "RV") == 0) strcpy(response, "14.2V\r");
        else if (strcmp(at_cmd, "@1") == 0) strcpy(response, "OBDII to RS232 v2.1 Mod:e2\r");
        else {
             strcpy(response, "OK\r"); // Default to OK for many setup commands to satisfy software
        }
    } else {
        bool is_hex = true;
        for (size_t i = 0; upper_cmd[i]; ++i) {
            if (!isxdigit((unsigned char)upper_cmd[i])) {
                is_hex = false;
                break;
            }
        }
        if (is_hex && strlen(upper_cmd) >= 2 && (strlen(upper_cmd) % 2) == 0) {
            char can_info[128] = "";
            obd_query(upper_cmd, response, sizeof(response), can_info, sizeof(can_info));
        } else {
            strcpy(response, "?\r");
        }
    }

    char log_response[100];
    snprintf(log_response, sizeof(log_response), "%.*s", (int)strcspn(response, "\r\n"), response);
    if (g_verbose_logs && g_elm327_logs) ESP_LOGI(TAG, "✅ %s > %s%s", upper_cmd, log_response, can_info);

    send_response(response);
    send_prompt();
}

static esp_err_t obd_query(const char *cmd_hex, char *response_buffer, size_t max_len, char *can_info, size_t can_info_len) {
    ESP_LOGI(TAG, "🔍 OBD запит: %s (протокол %d)", cmd_hex, current_protocol);
    
    can_info[0] = '\0';
    
    if (current_protocol != 0 && (current_protocol < 6 || current_protocol > 12)) {
        ESP_LOGW(TAG, "❌ Протокол %d не підтримується CAN-апаратним забезпеченням", current_protocol);
        snprintf(response_buffer, max_len, "NO DATA\r");
        return ESP_FAIL;
    }
    
    twai_status_info_t status;
    if (twai_get_status_info(&status) != ESP_OK || status.state != TWAI_STATE_RUNNING) {
        ESP_LOGW(TAG, "❌ CAN шина не працює");
        snprintf(response_buffer, max_len, "NO DATA\r");
        return ESP_FAIL;
    }

    uint8_t request_bytes[8];
    int cmd_len_bytes = strlen(cmd_hex) / 2;
    if (cmd_len_bytes < 1 || cmd_len_bytes > 7) { // OBD commands are 1 to 7 bytes payload
        snprintf(response_buffer, max_len, "?\r"); // Invalid length for an OBD command
        return ESP_ERR_INVALID_ARG;
    }

    // Per ISO 15765-4, the first byte of a CAN frame is the length of the OBD data that follows.
    twai_message_t tx_msg = {
        .identifier = can_header,
        .data_length_code = cmd_len_bytes + 1,
        .data = { (uint8_t)cmd_len_bytes }
    };
    for (int i = 0; i < cmd_len_bytes; i++) {
        sscanf(&cmd_hex[i * 2], "%2hhx", &tx_msg.data[i+1]);
    }
    
    // Flush any pending received messages before sending
    twai_message_t dummy;
    while (twai_receive(&dummy, 0) == ESP_OK);

    if (send_can_message(&tx_msg) != ESP_OK) {
        ESP_LOGW(TAG, "❌ CAN BUSY");
        snprintf(response_buffer, max_len, "BUS BUSY\r");
        return ESP_FAIL;
    }

    twai_message_t rx_msg;
    esp_err_t ret = twai_receive(&rx_msg, pdMS_TO_TICKS(obd_timeout));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "⏰ TIMEOUT (%d мс): NO DATA", obd_timeout);
        snprintf(response_buffer, max_len, "NO DATA\r");
        return ret;
    }
    
    snprintf(can_info, can_info_len, " | RX:%03lX %02X%02X%02X%02X%02X%02X%02X%02X", 
             (unsigned long)rx_msg.identifier, rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3], 
             rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);

    if (rx_msg.identifier < 0x7E8 || rx_msg.identifier > 0x7EF || rx_msg.data_length_code < 3) {
        snprintf(response_buffer, max_len, "NO DATA\r");
        return ESP_FAIL;
    }

    size_t pos = 0;
    // Format response: 41 0C 1A F8 -> 410C1AF8
    // data[0] is PCI (length), data[1] is mode+40, data[2] is PID, etc.
    uint8_t pci = rx_msg.data[0];
    uint8_t data_len = pci & 0x0F;
    if (data_len == 0 || data_len > 7) { // Should have at least mode/pid
        snprintf(response_buffer, max_len, "DATA ERROR\r");
        return ESP_FAIL;
    }
    
    // The actual response data starts from data[1]
    for (int i = 1; i < data_len + 1 && i < rx_msg.data_length_code; ++i) {
        pos += snprintf(response_buffer + pos, max_len - pos, "%02X%s", rx_msg.data[i], (spaces_enabled ? " " : ""));
    }
    
    // Remove trailing space if it exists
    if (spaces_enabled && pos > 0) {
        response_buffer[pos-1] = '\0';
    }

    strcat(response_buffer, "\r");

    ESP_LOGI(TAG, "✅ Відповідь: %s", response_buffer);
    return ESP_OK;
}

static esp_err_t send_can_message(const twai_message_t *message) {
    if (!elm327_wifi_is_connected()) {
        ESP_LOGE(TAG, "Blocked CAN send, client not connected.");
        return ESP_FAIL;
    }
    return twai_transmit(message, pdMS_TO_TICKS(100));
}

static esp_err_t send_response(const char *response) {
    if (client_sock < 0 || response == NULL) {
        return ESP_FAIL;
    }

    size_t len = strlen(response);
    size_t sent = 0;
    while (sent < len) {
        ssize_t ret = send(client_sock, response + sent, len - sent, 0);
        if (ret < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            handle_client_disconnection();
            return ESP_FAIL;
        }
        sent += ret;
    }
    return ESP_OK;
}

static esp_err_t send_prompt() {
    return send_response(ELM327_PROMPT);
}

static esp_err_t send_can_frame_to_client(const twai_message_t *message) {
    char frame_buffer[64];
    int len = snprintf(frame_buffer, sizeof(frame_buffer), "%03X", (unsigned int)message->identifier);
    if (spaces_enabled) len += snprintf(frame_buffer + len, sizeof(frame_buffer) - len, " ");
    
    for (int i = 0; i < message->data_length_code; ++i) {
        len += snprintf(frame_buffer + len, sizeof(frame_buffer) - len, "%02X", message->data[i]);
        if (spaces_enabled && i < message->data_length_code - 1) {
             len += snprintf(frame_buffer + len, sizeof(frame_buffer) - len, " ");
        }
    }
    len += snprintf(frame_buffer + len, sizeof(frame_buffer) - len, "\r");
    
    return send_response(frame_buffer);
}