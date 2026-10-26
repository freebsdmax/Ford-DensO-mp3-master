#include "elm327_wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "string.h"
#include "driver/twai.h"
#include "nvs_flash.h"
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "ELM327_WIFI";

// Shutdown flag for clean termination
bool elm327_shutdown = false;

// ELM327 protocol constants
#define ELM327_VERSION "ELM327 v1.5"
#define ELM327_PROMPT ">"
#define ELM327_DEFAULT_PORT 35000

// TCP server
static int server_sock = -1;
static int client_sock = -1;
static TaskHandle_t elm327_task_handle = NULL;
static QueueHandle_t cmd_queue = NULL;

// Command buffer
#define CMD_BUFFER_SIZE 256
static char cmd_buffer[CMD_BUFFER_SIZE];
static size_t cmd_len = 0;

// Response buffer
#define RSP_BUFFER_SIZE 512
static char rsp_buffer[RSP_BUFFER_SIZE];

// ELM327 state
static bool echo_enabled = false;
static bool linefeeds_enabled = false;
static bool spaces_enabled = true;
static uint8_t obd_protocol = 6; // ISO 15765-4 (CAN 11/125)
static uint16_t obd_timeout = 100; // 100ms
static uint8_t activity_monitor_count = 0;
static bool no_ecu = false; // Flag to avoid sending CAN if no ECU responses

// Logging configuration
static bool detailed_logging = false; // Set to true for verbose logs, false for compact

// Client connection status
static bool is_client_connected = false;

// Forward declarations
static void elm327_task(void *arg);
static esp_err_t process_command(const char *cmd, char *response, size_t max_len);
static esp_err_t obd_query(uint8_t mode, uint8_t pid, uint8_t *data, size_t *data_len);
static esp_err_t send_can_message(uint32_t id, const uint8_t *data, size_t len);
static esp_err_t receive_can_response(uint32_t expected_id, uint8_t *data, size_t *data_len, uint32_t timeout_ms);

static float read_battery_voltage(void)
{
    // TODO: Implement proper ADC reading when ADC driver conflict is resolved
    // For now, return a typical car battery voltage (12.6V)
    // GPIO 34 with 11:1 voltage divider should read ~1.15V for 12.6V battery
    
    ESP_LOGW(TAG, "ADC reading not implemented - returning mock battery voltage");
    
    // Return typical car battery voltage
    return 12.6f;
}

esp_err_t elm327_wifi_init(uint16_t port)
{
    ESP_LOGI(TAG, "Initializing ELM327 WiFi TCP server on port %d...", port);

    // Check if TWAI (CAN) driver is ready
    twai_status_info_t status_info;
    esp_err_t twai_check = twai_get_status_info(&status_info);
    if (twai_check != ESP_OK) {
        ESP_LOGE(TAG, "TWAI driver not ready: %s", esp_err_to_name(twai_check));
        ESP_LOGW(TAG, "ELM327 will work with mock data only");
    } else {
        ESP_LOGI(TAG, "TWAI driver ready - CAN communication enabled");
    }

    // Create command queue
    cmd_queue = xQueueCreate(10, sizeof(char) * CMD_BUFFER_SIZE);
    if (cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return ESP_FAIL;
    }

    // Create TCP server socket
    server_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_sock < 0) {
        ESP_LOGE(TAG, "Failed to create server socket");
        return ESP_FAIL;
    }

    // Set socket options
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind socket
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(server_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind server socket");
        close(server_sock);
        server_sock = -1;
        return ESP_FAIL;
    }

    // Listen
    if (listen(server_sock, 1) < 0) {
        ESP_LOGE(TAG, "Failed to listen on server socket");
        close(server_sock);
        server_sock = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ELM327 TCP server listening on port %d", port);

    // Create ELM327 task
    if (xTaskCreate(elm327_task, "elm327_task", 8192, NULL, 5, &elm327_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ELM327 task");
        close(server_sock);
        server_sock = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ELM327 WiFi TCP server initialized on port %d", port);
    return ESP_OK;
}

esp_err_t elm327_wifi_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing ELM327 WiFi TCP server...");

    // Set shutdown flag
    elm327_shutdown = true;

    // Delete task
    if (elm327_task_handle != NULL) {
        vTaskDelete(elm327_task_handle);
        elm327_task_handle = NULL;
        // Small delay to allow task to exit cleanly
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Delete queue
    if (cmd_queue != NULL) {
        vQueueDelete(cmd_queue);
        cmd_queue = NULL;
    }

    // Close sockets
    if (client_sock >= 0) {
        close(client_sock);
        client_sock = -1;
    }
    if (server_sock >= 0) {
        close(server_sock);
        server_sock = -1;
    }

    // Reset global state
    is_client_connected = false;
    cmd_len = 0;
    no_ecu = false;

    ESP_LOGI(TAG, "ELM327 WiFi TCP server deinitialized");
    return ESP_OK;
}

esp_err_t elm327_wifi_send(const uint8_t *data, size_t len)
{
    if (client_sock < 0) {
        ESP_LOGE(TAG, "❌ Немає підключеного клієнта для відправки даних");
        return ESP_FAIL;
    }

    ssize_t sent = send(client_sock, data, len, 0);
    if (sent < 0 || (size_t)sent != len) {
        ESP_LOGE(TAG, "❌ Помилка відправки даних: sent=%d, expected=%d", sent, len);
        return ESP_FAIL;
    }
    
    if (detailed_logging) {
        ESP_LOGI(TAG, "📤 Відправка %d байт клієнту: '%.*s' (HEX:", len, len, (char*)data);
        for (size_t i = 0; i < len; i++) {
            printf("%02X", data[i]);
        }
        printf(")\n");
        ESP_LOGD(TAG, "✅ Дані успішно відправлено клієнту");
    }
    
    return ESP_OK;
}

bool elm327_wifi_is_connected(void)
{
    return is_client_connected;
}

static void elm327_task(void *arg)
{
    ESP_LOGI(TAG, "ELM327 WiFi task started");
    fd_set read_fds;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (1) {
        // Check for shutdown
        if (elm327_shutdown) {
            ESP_LOGI(TAG, "ELM327 task shutting down");
            break;
        }
        
        // Accept new connections
        if (client_sock < 0) {
            ESP_LOGI(TAG, "Waiting for ELM327 TCP client connection...");
            client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &client_addr_len);
            if (client_sock >= 0) {
                ESP_LOGI(TAG, "🔗 НОВИЙ КЛІЄНТ ПІДКЛЮЧИВСЯ!");
                ESP_LOGI(TAG, "📍 IP адреса клієнта: %s", inet_ntoa(client_addr.sin_addr));
                ESP_LOGI(TAG, "🔌 Порт клієнта: %d", ntohs(client_addr.sin_port));
                ESP_LOGI(TAG, "🎯 ELM327 TCP клієнт готовий до роботи");
                cmd_len = 0; // Reset command buffer
                is_client_connected = true; // Set connection flag
                no_ecu = false; // Reset ECU detection flag for new client
                
                // Send initial prompt
                elm327_wifi_send((uint8_t *)ELM327_PROMPT, strlen(ELM327_PROMPT));
            }
        }

        // Handle client communication
        if (client_sock >= 0) {
            FD_ZERO(&read_fds);
            FD_SET(client_sock, &read_fds);

            struct timeval timeout = {0, 1000}; // 1ms timeout для ще швидшої реакції
            int ret = select(client_sock + 1, &read_fds, NULL, NULL, &timeout);

            if (ret > 0) {
                // Data available
                ESP_LOGD(TAG, "📡 select() повернув %d - дані доступні для читання", ret);
                char buffer[128];
                ssize_t len = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
                
                ESP_LOGD(TAG, "🔄 recv() повернув %d байт", len);

                if (len > 0) {
                    buffer[len] = '\0';
                    
                    if (detailed_logging) {
                        // Детальне логування отриманих даних
                        ESP_LOGI(TAG, "📥 Отримано %d байт від клієнта", len);
                        ESP_LOGI(TAG, "📄 Дані (ASCII): '%s'", buffer);
                        
                        // Показуємо в hex форматі для діагностики
                        ESP_LOGI(TAG, "🔍 Дані (HEX):");
                        for (int i = 0; i < len; i++) {
                            printf("%02X ", (uint8_t)buffer[i]);
                            if ((i + 1) % 16 == 0) printf("\n");
                        }
                        printf("\n");
                    }

                    // Add to command buffer
                    size_t copy_len = len;
                    if (cmd_len + copy_len >= CMD_BUFFER_SIZE) {
                        copy_len = CMD_BUFFER_SIZE - cmd_len - 1;
                    }

                    ESP_LOGD(TAG, "📝 Додавання %d байт до буфера команд (було %d, стане %d)", 
                            copy_len, cmd_len, cmd_len + copy_len);
                    
                    memcpy(cmd_buffer + cmd_len, buffer, copy_len);
                    cmd_len += copy_len;
                    cmd_buffer[cmd_len] = '\0';

                    ESP_LOGD(TAG, "🔄 Поточний буфер команд: '%s' (%d байт)", cmd_buffer, cmd_len);

                    // Check for command terminator (CR or LF)
                    char *terminator_pos = NULL;
                    char *cr_pos = strchr(cmd_buffer, '\r');
                    char *lf_pos = strchr(cmd_buffer, '\n');
                    
                    if (cr_pos != NULL && lf_pos != NULL) {
                        // Both found, use the first one
                        terminator_pos = (cr_pos < lf_pos) ? cr_pos : lf_pos;
                    } else if (cr_pos != NULL) {
                        terminator_pos = cr_pos;
                    } else if (lf_pos != NULL) {
                        terminator_pos = lf_pos;
                    }
                    
                    if (terminator_pos != NULL) {
                        ESP_LOGD(TAG, "🔍 Знайдено термінатор на позиції %d в буфері", (int)(terminator_pos - cmd_buffer));
                        *terminator_pos = '\0'; // Null terminate command
                        size_t cmd_length = terminator_pos - cmd_buffer;

                        // Check for monitor stop (space character)
                        if (cmd_length == 1 && cmd_buffer[0] == ' ') {
                            // Stop monitor mode
                            elm327_wifi_send((uint8_t *)"STOPPED\r", strlen("STOPPED\r"));
                            elm327_wifi_send((uint8_t *)ELM327_PROMPT, strlen(ELM327_PROMPT));
                        } else {
                            // Normal command processing
                            // Process command
                            char response[RSP_BUFFER_SIZE];
                            esp_err_t cmd_ret = process_command(cmd_buffer, response, sizeof(response));
                            if (cmd_ret == ESP_OK) {
                                // Compact log with hex
                                char hex_buf[512];
                                size_t hex_pos = 0;
                                for (size_t i = 0; i < strlen(response); i++) {
                                    if (hex_pos < sizeof(hex_buf) - 3) {
                                        hex_pos += sprintf(hex_buf + hex_pos, "%02X", (uint8_t)response[i]);
                                        if (i < strlen(response) - 1) hex_pos += sprintf(hex_buf + hex_pos, " ");
                                    }
                                }
                                hex_buf[hex_pos] = '\0';
                                
                                // Prepare response for logging (remove trailing \r)
                                char log_resp[256];
                                strncpy(log_resp, response, sizeof(log_resp)-1);
                                log_resp[sizeof(log_resp)-1] = '\0';
                                size_t resp_len = strlen(log_resp);
                                if (resp_len > 0 && log_resp[resp_len-1] == '\r') {
                                    log_resp[resp_len-1] = '\0';
                                }
                                
                                ESP_LOGI(TAG, "CMD: '%s' -> '%s' | HEX: %s", cmd_buffer, log_resp, hex_buf);
                                
                                if (detailed_logging) {
                                    ESP_LOGI(TAG, "✅ Команда оброблена успішно");
                                    ESP_LOGI(TAG, "📤 Відповідь сервера: '%s'", response);
                                    
                                    // Показуємо відповідь в hex
                                    ESP_LOGI(TAG, "🔍 Відповідь (HEX):");
                                    for (size_t i = 0; i < strlen(response); i++) {
                                        printf("%02X ", (uint8_t)response[i]);
                                        if ((i + 1) % 16 == 0) printf("\n");
                                    }
                                    printf("\n");
                                }
                                
                                // Append linefeed if enabled
                                if (linefeeds_enabled) {
                                    size_t len = strlen(response);
                                    if (len < sizeof(response) - 1) {
                                        response[len] = '\n';
                                        response[len + 1] = '\0';
                                    }
                                }
                                
                                // Echo command if enabled
                                if (echo_enabled) {
                                    elm327_wifi_send((uint8_t *)cmd_buffer, cmd_length);
                                    elm327_wifi_send((uint8_t *)"\r", 1);
                                    if (linefeeds_enabled) {
                                        elm327_wifi_send((uint8_t *)"\n", 1);
                                    }
                                }
                                
                                // Send response
                                size_t response_len = strlen(response);
                                if (elm327_wifi_send((uint8_t *)response, response_len) != ESP_OK) {
                                    ESP_LOGE(TAG, "Failed to send response");
                                }

                                // Send prompt always (not dependent on echo setting)
                                elm327_wifi_send((uint8_t *)ELM327_PROMPT, strlen(ELM327_PROMPT));
                            }
                        }

                        // Shift remaining data
                        size_t terminator_len = 1;
                        if (terminator_pos + 1 < cmd_buffer + cmd_len && 
                            ((terminator_pos[1] == '\r' && *terminator_pos == '\n') || 
                             (terminator_pos[1] == '\n' && *terminator_pos == '\r'))) {
                            terminator_len = 2; // CRLF or LFCR
                        }
                        size_t remaining = cmd_len - (terminator_pos - cmd_buffer + terminator_len);
                        if (remaining > 0) {
                            memmove(cmd_buffer, terminator_pos + terminator_len, remaining);
                        }
                        cmd_len = remaining;
                        cmd_buffer[cmd_len] = '\0';
                    } else {
                        // No command terminator found yet
                        ESP_LOGD(TAG, "⏳ Символ \\r не знайдено, чекаємо більше даних (буфер: %d байт)", cmd_len);
                    }
                } else {
                    // Client disconnected (normal closure)
                    ESP_LOGW(TAG, "🔌 КЛІЄНТ ВІДКЛЮЧИВСЯ НОРМАЛЬНО (len=0)!");
                    ESP_LOGI(TAG, "⏹️ З'єднання з ELM327 клієнтом закрито");
                    close(client_sock);
                    client_sock = -1;
                    is_client_connected = false; // Reset connection flag
                    cmd_len = 0; // Reset command buffer
                }
            } else if (ret == 0) {
                // Timeout - no data available
                ESP_LOGD(TAG, "⏰ select() таймаут - даних немає (буфер команд: %d байт)", cmd_len);
            } else {
                // Error in select
                ESP_LOGE(TAG, "❌ Помилка select(): %d", ret);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1)); // Small delay to prevent busy loop
    }
}

static esp_err_t process_command(const char *cmd, char *response, size_t max_len)
{
    ESP_LOGD(TAG, "🔧 process_command() викликано для: '%s'", cmd);
    
    char upper_cmd[CMD_BUFFER_SIZE];
    strncpy(upper_cmd, cmd, sizeof(upper_cmd));
    upper_cmd[sizeof(upper_cmd) - 1] = '\0';

    // Convert to uppercase
    for (char *p = upper_cmd; *p; ++p) {
        *p = toupper(*p);
    }

    // Handle AT commands
    if (strncmp(upper_cmd, "AT", 2) == 0) {
        const char *at_cmd = upper_cmd + 2;

        if (strcmp(at_cmd, "I") == 0) {
            // Version info
            snprintf(response, max_len, "%s\r", ELM327_VERSION);
        } else if (strcmp(at_cmd, "E0") == 0) {
            // Disable echo
            echo_enabled = false;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "E1") == 0) {
            // Enable echo
            echo_enabled = true;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "L0") == 0) {
            // Disable linefeeds
            linefeeds_enabled = false;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "L1") == 0) {
            // Enable linefeeds
            linefeeds_enabled = true;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "Z") == 0) {
            // Reset
            echo_enabled = false;
            linefeeds_enabled = false;
            spaces_enabled = true;
            strcpy(response, "\r\rELM327 v1.5\r>");
        } else if (strcmp(at_cmd, "RV") == 0) {
            // Read voltage - use ADC if available, otherwise mock
            float voltage = read_battery_voltage();
            if (voltage > 0) {
                snprintf(response, max_len, "%.1fV\r", voltage);
            } else {
                strcpy(response, "12.6V\r");
            }
        } else if (strcmp(at_cmd, "WS") == 0) {
            // Warm start
            strcpy(response, "\r\rELM327 v1.5\r>");
        } else if (strcmp(at_cmd, "IGN") == 0) {
            // Ignition status
            strcpy(response, "ON\r");
        } else if (strcmp(at_cmd, "SP") == 0) {
            // Set protocol - default to CAN
            obd_protocol = 6; // ISO 15765-4 (CAN 11/500)
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "SP", 2) == 0) {
            // Set specific protocol
            const char *proto_str = at_cmd + 2;
            int new_protocol = -1;
            if (strcmp(proto_str, "A") == 0) {
                new_protocol = 10; // SAE J1939 CAN (29 bit, 250 kbps)
            } else if (strcmp(proto_str, "B") == 0) {
                new_protocol = 11; // ISO 15765-4 CAN (11 bit, 125 kbps)
            } else if (strcmp(proto_str, "C") == 0) {
                new_protocol = 12; // ISO 15765-4 CAN (29 bit, 125 kbps)
            } else if (strcmp(proto_str, "D") == 0) {
                new_protocol = 14; // ISO 15765-4 CAN (11 bit, 1 Mbps)
            } else if (strcmp(proto_str, "E") == 0) {
                new_protocol = 15; // ISO 15765-4 CAN (29 bit, 1 Mbps)
            } else if (strcmp(proto_str, "F") == 0) {
                new_protocol = 13; // Ford MS-CAN
            } else if (strcmp(proto_str, "FF") == 0) {
                new_protocol = 13; // Ford DCL/CAN
            } else {
                new_protocol = atoi(proto_str);
            }
            
            if (new_protocol >= 0 && new_protocol <= 15) {
                // Test protocol support
                twai_status_info_t status_info;
                esp_err_t twai_check = twai_get_status_info(&status_info);
                bool can_ready = (twai_check == ESP_OK && status_info.state == TWAI_STATE_RUNNING);
                
                if (new_protocol == 0) {
                    // Automatic protocol selection - always OK
                    obd_protocol = new_protocol;
                    strcpy(response, "OK\r");
                } else if (new_protocol >= 1 && new_protocol <= 5) {
                    // Legacy protocols (PWM, VPW, ISO) - supported via mock data
                    obd_protocol = new_protocol;
                    strcpy(response, "OK\r");
                } else if (new_protocol >= 6 && new_protocol <= 15) {
                    // CAN protocols - require CAN to be ready
                    if (can_ready) {
                        obd_protocol = new_protocol;
                        strcpy(response, "OK\r");
                    } else {
                        strcpy(response, "NO CAN\r");
                    }
                } else {
                    // Non-CAN protocols - not supported, return NO DATA
                    strcpy(response, "NO DATA\r");
                }
            } else {
                strcpy(response, "?\r");
            }
        } else if (strcmp(at_cmd, "DP") == 0) {
            // Display protocol
            const char *protocol_names[] = {
                "Automatic", "SAE J1850 PWM", "SAE J1850 VPW", 
                "ISO 9141-2", "ISO 14230-4 KWP", "ISO 14230-4 KWP Fast",
                "ISO 15765-4 CAN", "ISO 15765-4 CAN", "ISO 15765-4 CAN", "ISO 15765-4 CAN",
                "SAE J1939 CAN", "ISO 15765-4 CAN (125 kbps 11-bit)", "ISO 15765-4 CAN (125 kbps 29-bit)", "Ford MS-CAN",
                "ISO 15765-4 CAN (1 Mbps 11-bit)", "ISO 15765-4 CAN (1 Mbps 29-bit)"
            };
            if (obd_protocol <= 15) {
                snprintf(response, max_len, "%s\r", protocol_names[obd_protocol]);
            } else {
                strcpy(response, "AUTO\r");
            }
        } else if (strcmp(at_cmd, "H0") == 0) {
            // Headers off
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "H1") == 0) {
            // Headers on
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "S0") == 0) {
            // Spaces off
            spaces_enabled = false;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "S1") == 0) {
            // Spaces on
            spaces_enabled = true;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "@1") == 0) {
            // Device description
            strcpy(response, "ELM327 Emulator\r");
        } else if (strcmp(at_cmd, "@2") == 0) {
            // Device description
            strcpy(response, "OBDII to RS232 Interpreter\r");
        } else if (strcmp(at_cmd, "@0") == 0) {
            // Output info
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "@3") == 0) {
            // Version info
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "@4") == 0) {
            // Undocumented
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "@5") == 0) {
            // Undocumented
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "@6") == 0) {
            // Undocumented
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "@7") == 0) {
            // Undocumented
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "@8") == 0) {
            // Undocumented
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "PP", 2) == 0) {
            // Programmable parameter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "SH000") == 0) {
            // Set header
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CRA7E8") == 0) {
            // Set receive address
            strcpy(response, "OK\r");
            // Warm start
            strcpy(response, "\r\rELM327 v1.5\r>");
        } else if (strcmp(at_cmd, "D") == 0) {
            // Reset settings to defaults
            echo_enabled = false;
            linefeeds_enabled = false;
            spaces_enabled = true;
            obd_protocol = 6;
            obd_timeout = 100;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "DPN") == 0) {
            // Display protocol number
            snprintf(response, max_len, "%d\r", obd_protocol);
        } else if (strcmp(at_cmd, "SI") == 0) {
            // ISO init
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IB96") == 0) {
            // ISO baud rate
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IB10") == 0) {
            // ISO baud rate
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "TP", 2) == 0) {
            // Test protocol
            uint8_t protocol = atoi(at_cmd + 2);
            if (protocol >= 1 && protocol <= 5) {
                // Legacy protocols - unable to connect
                strcpy(response, "UNABLE TO CONNECT\r");
            } else if (protocol >= 6 && protocol <= 15) {
                // CAN protocols - test by sending query
                if (no_ecu) {
                    strcpy(response, "UNABLE TO CONNECT\r");
                } else {
                    uint8_t test_data[8] = {0x02, 0x01, 0x00}; // Test PID 0x00
                    esp_err_t ret = send_can_message(0x7DF, test_data, 3);
                    if (ret == ESP_OK) {
                        uint8_t data[8];
                        size_t data_len;
                        ret = receive_can_response(0x7E8, data, &data_len, 1000);
                        if (ret == ESP_OK) {
                            strcpy(response, "OK\r");
                        } else {
                            no_ecu = true;
                            strcpy(response, "UNABLE TO CONNECT\r");
                        }
                    } else {
                        no_ecu = true;
                        strcpy(response, "UNABLE TO CONNECT\r");
                    }
                }
            } else {
                strcpy(response, "?\r");
            }
        } else if (strcmp(at_cmd, "STI") == 0) {
            // Set timeout to 55ms (or similar)
            obd_timeout = 55;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "VTI") == 0) {
            // Variable timeout
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CFC0") == 0) {
            // CAN flow control off
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CFC1") == 0) {
            // CAN flow control on
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CAF0") == 0) {
            // CAN auto formatting off
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CAF1") == 0) {
            // CAN auto formatting on
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "CM", 2) == 0) {
            // CAN mask
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "CP", 2) == 0) {
            // CAN pattern
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "CRA", 3) == 0) {
            // CAN receive address
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CSM0") == 0) {
            // CAN silent mode off
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CSM1") == 0) {
            // CAN silent mode on
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CSM2") == 0) {
            // CAN silent listen
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CTM5") == 0) {
            // Timing multiplier
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CEA") == 0) {
            // Extended addressing
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "ST", 2) == 0) {
            // Timeout
            obd_timeout = atoi(at_cmd + 2);
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AL") == 0) {
            // Allow long messages
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AT0") == 0) {
            // Adaptive timing off
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AT1") == 0) {
            // Adaptive timing auto1
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AT2") == 0) {
            // Adaptive timing auto2
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "BRD", 3) == 0) {
            // Baud rate divisor
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "BRT", 3) == 0) {
            // Baud rate timeout
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "PC") == 0) {
            // Protocol close
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "MR") == 0) {
            // Monitor for receiver - stream real CAN traffic
            twai_status_info_t status_info;
            if (twai_get_status_info(&status_info) == ESP_OK && status_info.state == TWAI_STATE_RUNNING) {
                strcpy(response, "OK\r");
                // Enter CAN monitoring mode - stream all received CAN messages
                while (true) {
                    // Check if client is still connected
                    if (!is_client_connected) {
                        ESP_LOGI(TAG, "📡 Клієнт відключений, виходимо з режиму моніторингу");
                        break;
                    }
                    twai_message_t message;
                    esp_err_t ret = twai_receive(&message, pdMS_TO_TICKS(100)); // 100ms timeout
                    if (ret == ESP_OK) {
                        // Format and send CAN message to client
                        char can_msg[64];
                        int len = sprintf(can_msg, "%03X ", (unsigned int)message.identifier);
                        for (size_t i = 0; i < message.data_length_code; i++) {
                            len += sprintf(can_msg + len, "%02X ", message.data[i]);
                        }
                        len += sprintf(can_msg + len - 1, "\r"); // Replace last space with \r
                        elm327_wifi_send((uint8_t *)can_msg, len);
                    }
                    // Check for stop command (space) - simplified check
                    if (cmd_len > 0 && cmd_buffer[0] == ' ') {
                        elm327_wifi_send((uint8_t *)"STOPPED\r", strlen("STOPPED\r"));
                        elm327_wifi_send((uint8_t *)ELM327_PROMPT, strlen(ELM327_PROMPT));
                        cmd_len = 0; // Clear buffer
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10)); // Small delay
                }
            } else {
                strcpy(response, "NO CAN\r");
            }
        } else if (strcmp(at_cmd, "MT") == 0) {
            // Monitor for transmitter - stream real CAN traffic (same as MR for now)
            twai_status_info_t status_info;
            if (twai_get_status_info(&status_info) == ESP_OK && status_info.state == TWAI_STATE_RUNNING) {
                strcpy(response, "OK\r");
                // Enter CAN monitoring mode - stream all received CAN messages
                while (true) {
                    // Check if client is still connected
                    if (!is_client_connected) {
                        ESP_LOGI(TAG, "📡 Клієнт відключений, виходимо з режиму моніторингу");
                        break;
                    }
                    twai_message_t message;
                    esp_err_t ret = twai_receive(&message, pdMS_TO_TICKS(100)); // 100ms timeout
                    if (ret == ESP_OK) {
                        // Format and send CAN message to client
                        char can_msg[64];
                        int len = sprintf(can_msg, "%03X ", (unsigned int)message.identifier);
                        for (size_t i = 0; i < message.data_length_code; i++) {
                            len += sprintf(can_msg + len, "%02X ", message.data[i]);
                        }
                        len += sprintf(can_msg + len - 1, "\r"); // Replace last space with \r
                        elm327_wifi_send((uint8_t *)can_msg, len);
                    }
                    // Check for stop command (space) - simplified check
                    if (cmd_len > 0 && cmd_buffer[0] == ' ') {
                        elm327_wifi_send((uint8_t *)"STOPPED\r", strlen("STOPPED\r"));
                        elm327_wifi_send((uint8_t *)ELM327_PROMPT, strlen(ELM327_PROMPT));
                        cmd_len = 0; // Clear buffer
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10)); // Small delay
                }
            } else {
                strcpy(response, "NO CAN\r");
            }
        } else if (strcmp(at_cmd, "MA") == 0) {
            // Monitor all - stream real CAN traffic
            twai_status_info_t status_info;
            if (twai_get_status_info(&status_info) == ESP_OK && status_info.state == TWAI_STATE_RUNNING) {
                strcpy(response, "OK\r");
                // Enter CAN monitoring mode - stream all received CAN messages
                while (true) {
                    // Check if client is still connected
                    if (!is_client_connected) {
                        ESP_LOGI(TAG, "📡 Клієнт відключений, виходимо з режиму моніторингу");
                        break;
                    }
                    twai_message_t message;
                    esp_err_t ret = twai_receive(&message, pdMS_TO_TICKS(100)); // 100ms timeout
                    if (ret == ESP_OK) {
                        // Format and send CAN message to client
                        char can_msg[64];
                        int len = sprintf(can_msg, "%03X ", (unsigned int)message.identifier);
                        for (size_t i = 0; i < message.data_length_code; i++) {
                            len += sprintf(can_msg + len, "%02X ", message.data[i]);
                        }
                        len += sprintf(can_msg + len - 1, "\r"); // Replace last space with \r
                        elm327_wifi_send((uint8_t *)can_msg, len);
                    }
                    // Check for stop command (space) - simplified check
                    if (cmd_len > 0 && cmd_buffer[0] == ' ') {
                        elm327_wifi_send((uint8_t *)"STOPPED\r", strlen("STOPPED\r"));
                        elm327_wifi_send((uint8_t *)ELM327_PROMPT, strlen(ELM327_PROMPT));
                        cmd_len = 0; // Clear buffer
                        break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(10)); // Small delay
                }
            } else {
                strcpy(response, "NO CAN\r");
            }
        } else if (strcmp(at_cmd, "AR") == 0) {
            // Auto receive address
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AF0") == 0) {
            // Auto formatting off
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AF1") == 0) {
            // Auto formatting on
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "SW") == 0) {
            // Set wait time
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "WT") == 0) {
            // Waiting time
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "M0") == 0) {
            // Memory save off
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "M1") == 0) {
            // Memory save on
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "BRD?") == 0) {
            // Query UART baud rate
            strcpy(response, "96\r");
        } else if (strcmp(at_cmd, "BRT?") == 0) {
            // Query bus baud rate
            strcpy(response, "48\r");
        } else if (strcmp(at_cmd, "AMC") == 0) {
            // Print Activity Monitor count
            snprintf(response, max_len, "%02X\r", activity_monitor_count);
        } else if (strcmp(at_cmd, "CS") == 0) {
            // Print CAN status counts
            twai_status_info_t status_info;
            if (twai_get_status_info(&status_info) == ESP_OK) {
                if (status_info.tx_error_counter > 255) {
                    strcpy(response, "OFF\r");
                } else {
                    snprintf(response, max_len, "%02lX %02lX\r", status_info.tx_error_counter, status_info.rx_error_counter);
                }
            } else {
                strcpy(response, "00 00\r");
            }
        } else if (strcmp(at_cmd, "SS") == 0) {
            // Show statistics
            strcpy(response, "Messages: 0\r");
        } else if (strcmp(at_cmd, "BI") == 0) {
            // Break
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "BD") == 0) {
            // Break disable
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CV") == 0) {
            // Calibration voltage
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CSM") == 0) {
            // Sleep mode
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FID") == 0) {
            // ID data
            strcpy(response, "ELM327_EMU_001\r");
        } else if (strcmp(at_cmd, "STI") == 0) {
            // STN info - protocol response
            strcpy(response, "STN1170 v4.0\r");
        } else if (strcmp(at_cmd, "STI BRP") == 0) {
            // Set Baud Rate Prescaler - stub
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "STI PWM") == 0) {
            // PWM operation - stub
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "STI VPW") == 0) {
            // VPW operation - stub
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "STI FMT") == 0) {
            // Message format - stub
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "STI RD") == 0) {
            // Read data - stub
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "STI WR") == 0) {
            // Write data - stub
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "VTI") == 0) {
            // VTI identification
            strcpy(response, "ELM327 v1.5 (VTI)\r");
        } else if (strcmp(at_cmd, "VTID") == 0) {
            // VTI detailed identification
            strcpy(response, "VTI327 v1.5b\rSN: XXXXXXX\r");
        } else if (strncmp(at_cmd, "VTIR ", 5) == 0) {
            // Read register x - return hex value
            strcpy(response, "00\r");
        } else if (strncmp(at_cmd, "VTIW ", 5) == 0) {
            // Write y to register x
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "VTI0") == 0 || strcmp(at_cmd, "VTI1") == 0 || strcmp(at_cmd, "VTI2") == 0 ||
                   strcmp(at_cmd, "VTI3") == 0 || strcmp(at_cmd, "VTI4") == 0 || strcmp(at_cmd, "VTI5") == 0 ||
                   strcmp(at_cmd, "VTI6") == 0 || strcmp(at_cmd, "VTI7") == 0 || strcmp(at_cmd, "VTI8") == 0 ||
                   strcmp(at_cmd, "VTI9") == 0) {
            // Test outputs 0-9
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "VTIC ", 5) == 0) {
            // Configuration
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "STW") == 0) {
            // STN warm reset
            strcpy(response, "STN1170 v4.0\r>");
        } else if (strcmp(at_cmd, "RD") == 0) {
            // Read data memory
            strcpy(response, "00 00 00 00\r");
        } else if (strcmp(at_cmd, "WR") == 0) {
            // Write data memory
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "ER") == 0) {
            // Erase data memory
            strcpy(response, "OK\r");
        } else if (strncmp(at_cmd, "TP", 2) == 0) {
            // Test protocol - perform actual testing
            int test_protocol = obd_protocol; // Default to current
            const char *tp_param = at_cmd + 2;
            if (*tp_param != '\0') {
                // Has parameter - test specific protocol
                if (strcmp(tp_param, "A") == 0) test_protocol = 10;
                else if (strcmp(tp_param, "B") == 0) test_protocol = 11;
                else if (strcmp(tp_param, "C") == 0) test_protocol = 12;
                else if (strcmp(tp_param, "D") == 0) test_protocol = 14;
                else if (strcmp(tp_param, "E") == 0) test_protocol = 15;
                else if (strcmp(tp_param, "F") == 0) test_protocol = 13;
                else if (strcmp(tp_param, "FF") == 0) test_protocol = 13;
                else test_protocol = atoi(tp_param);
            }
            
            if (test_protocol >= 6 && test_protocol <= 15) {
                // CAN protocol test
                twai_status_info_t status_info;
                if (twai_get_status_info(&status_info) == ESP_OK && status_info.state == TWAI_STATE_RUNNING) {
                    // Send test message and check response
                    uint8_t test_data[3] = {0x02, 0x01, 0x00}; // Test PID 00
                    esp_err_t ret = send_can_message(0x7DF, test_data, 3);
                    if (ret == ESP_OK) {
                        uint8_t resp_data[8];
                        size_t resp_len;
                        ret = receive_can_response(0x7E8, resp_data, &resp_len, 500);
                        if (ret == ESP_OK && resp_len >= 3) {
                            strcpy(response, "OK\r");
                        } else {
                            strcpy(response, "FAIL\r");
                        }
                    } else {
                        strcpy(response, "FAIL\r");
                    }
                } else {
                    strcpy(response, "NO CAN\r");
                }
            } else if (test_protocol >= 1 && test_protocol <= 5) {
                // Legacy protocol test - no hardware support
                strcpy(response, "UNABLE TO CONNECT\r");
            } else {
                strcpy(response, "?\r");
            }
        } else if (strcmp(at_cmd, "TA") == 0) {
            // Adaptive timing
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "TR") == 0) {
            // Trigger settings
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FTR") == 0) {
            // Filter triggers
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AT0") == 0) {
            // Adaptive timing off
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AT1") == 0) {
            // Adaptive timing auto1
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AT2") == 0) {
            // Adaptive timing auto2
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "D1") == 0) {
            // Default settings
            echo_enabled = false;
            linefeeds_enabled = false;
            spaces_enabled = true;
            obd_protocol = 6;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "D0") == 0) {
            // Default settings
            echo_enabled = false;
            linefeeds_enabled = false;
            spaces_enabled = true;
            obd_protocol = 6;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AMT20") == 0) {
            // Activity monitor timeout
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AMT30") == 0) {
            // Activity monitor timeout
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "AMT40") == 0) {
            // Activity monitor timeout
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FE") == 0) {
            // Forget events
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CEA70") == 0) {
            // CAN extended addressing
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CER70") == 0) {
            // CAN extended receive
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CF7E8") == 0) {
            // CAN filter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CF000007E8") == 0) {
            // CAN filter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CMFFF") == 0) {
            // CAN mask
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CM0000FFFF") == 0) {
            // CAN mask
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CP07") == 0) {
            // CAN pattern
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CTM1") == 0) {
            // Timing multiplier
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FCSD300000") == 0) {
            // Flow control
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FCSH7E1") == 0) {
            // Flow control header
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FCSH000007E1") == 0) {
            // Flow control header
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FCSM0") == 0) {
            // Flow control mode
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FCSM1") == 0) {
            // Flow control mode
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FCSM2") == 0) {
            // Flow control mode
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FCSM8") == 0) {
            // Flow control mode
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FCSM9") == 0) {
            // Flow control mode
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "FCSMA") == 0) {
            // Flow control mode
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IB10") == 0) {
            // ISO baud rate
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IB12") == 0) {
            // ISO baud rate
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IB96") == 0) {
            // ISO baud rate
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IB15") == 0) {
            // ISO baud rate
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IB18") == 0) {
            // ISO baud rate
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IB48") == 0) {
            // ISO baud rate
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IFRS") == 0) {
            // ISO filter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IFRH") == 0) {
            // ISO filter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IFR6") == 0) {
            // ISO filter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IFR4") == 0) {
            // ISO filter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IFR5") == 0) {
            // ISO filter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IFR2") == 0) {
            // ISO filter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IFR0") == 0) {
            // ISO filter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IFR1") == 0) {
            // ISO filter
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "IIA13") == 0) {
            // ISO init address
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "JS") == 0) {
            // J1939
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "JE") == 0) {
            // J1939
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "JHF0") == 0) {
            // J1939 header format
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "JHF1") == 0) {
            // J1939 header format
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "JTM5") == 0) {
            // J1939 timing
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "JTM1") == 0) {
            // J1939 timing
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "KW") == 0) {
            // Keyword
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "KW0") == 0) {
            // Keyword
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "KW1") == 0) {
            // Keyword
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "NL") == 0) {
            // No linefeeds
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "R0") == 0) {
            // Responses off
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "R1") == 0) {
            // Responses on
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CV1200") == 0) {
            // Calibration voltage
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "CV0000") == 0) {
            // Calibration voltage
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "SH000007E1") == 0) {
            // Set header
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "SH7E1") == 0) {
            // Set header
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "SH8113F0") == 0) {
            // Set header
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "ST20") == 0) {
            // Set timeout
            obd_timeout = 20;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "ST32") == 0) {
            // Set timeout
            obd_timeout = 32;
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "SW00") == 0) {
            // Set wait time
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "SW32") == 0) {
            // Set wait time
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "SW92") == 0) {
            // Set wait time
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "TAF0") == 0) {
            // Test adaptive framing
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "V1") == 0) {
            // Variable DLC on
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "V0") == 0) {
            // Variable DLC off
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "WM3E") == 0) {
            // Wakeup message
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "WM2100") == 0) {
            // Wakeup message
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "WM210000") == 0) {
            // Wakeup message
            strcpy(response, "OK\r");
        } else if (strcmp(at_cmd, "WM8113F03E") == 0) {
            // Wakeup message
            strcpy(response, "OK\r");
        } else {
            // Unknown AT command
            strcpy(response, "?\r");
        }
    } else {
        // OBD-II command
        uint8_t mode, pid;
        if (sscanf(upper_cmd, "%2hhx%2hhx", &mode, &pid) == 2) {
            uint8_t obd_data[8];
            size_t obd_len = 0;

            esp_err_t ret = obd_query(mode, pid, obd_data, &obd_len);
            if (ret == ESP_OK && obd_len > 0) {
                // Format response as hex
                size_t pos = 0;
                for (size_t i = 0; i < obd_len && pos < max_len - 4; ++i) {
                    if (i > 0 && spaces_enabled) {
                        pos += snprintf(response + pos, max_len - pos, " ");
                    }
                    pos += snprintf(response + pos, max_len - pos, "%02X", obd_data[i]);
                }
                strcat(response, "\r");
            } else {
                strcpy(response, "NO DATA\r");
            }
        } else {
            strcpy(response, "?\r");
        }
    }

    return ESP_OK;
}

static esp_err_t obd_query(uint8_t mode, uint8_t pid, uint8_t *data, size_t *data_len)
{
    // Check if TWAI (CAN) is ready
    twai_status_info_t status_info;
    esp_err_t twai_check = twai_get_status_info(&status_info);
    bool can_ready = (twai_check == ESP_OK && status_info.state == TWAI_STATE_RUNNING);
    
    ESP_LOGI(TAG, "OBD-II query: mode=0x%02X, pid=0x%02X, CAN ready: %s", mode, pid, can_ready ? "yes" : "no");
    
    // Check if client is still connected
    if (!is_client_connected || elm327_shutdown) {
        ESP_LOGW(TAG, "Client disconnected or shutting down, skipping OBD query");
        return ESP_FAIL;
    }
    
    if (!can_ready) {
        // Use mock data when CAN is not available
        ESP_LOGI(TAG, "Using mock OBD-II data (CAN not ready)");
        goto mock_fallback;
    }
    
    if (no_ecu) {
        // Skip CAN sending if no ECU detected
        ESP_LOGI(TAG, "Skipping CAN send (no ECU detected)");
        goto mock_fallback;
    }
    
    // Create OBD-II request message
    uint8_t request_data[8];
    request_data[0] = 0x02; // Single frame, 2 bytes of data
    request_data[1] = mode;
    request_data[2] = pid;
    
    // Send request to ECU (functional address 0x7DF)
    esp_err_t ret = send_can_message(0x7DF, request_data, 3);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send CAN message to ECU");
        no_ecu = true; // Mark no ECU if send fails
        return ESP_FAIL; // No ECU present
    }
    
    // Wait for response (typically from 0x7E8 for ECU #1)
    ret = receive_can_response(0x7E8, data, data_len, 1000); // 1 second timeout
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "No CAN response from ECU");
        no_ecu = true; // Mark no ECU if no response
        return ESP_FAIL; // No ECU present
    }
    
    // Validate response format
    if (*data_len < 3) {
        ESP_LOGW(TAG, "Invalid CAN response length: %d", *data_len);
        no_ecu = true; // Mark no ECU if invalid response
        return ESP_FAIL; // Invalid response
    }
    
    // Check if response matches our request (mode + 0x40, same pid)
    if (data[1] != (mode + 0x40) || data[2] != pid) {
        ESP_LOGW(TAG, "CAN response mismatch: got mode=0x%02X pid=0x%02X, expected mode=0x%02X pid=0x%02X", 
                data[1], data[2], mode + 0x40, pid);
        no_ecu = true; // Mark no ECU if mismatch
        return ESP_FAIL; // Invalid response
    }
    
    // Extract actual data (skip PCI byte, mode response byte, pid byte)
    if (*data_len > 3) {
        memmove(data, data + 3, *data_len - 3);
        *data_len -= 3;
    } else {
        *data_len = 0;
    }
    
    // Consolidated log
    ESP_LOGI(TAG, "OBD: CMD=%02X%02X SENT=7DF:%02X %02X %02X RECV=7E8:%02X %02X %02X... RESP=",
             mode, pid, request_data[0], request_data[1], request_data[2], 
             (data[0] >= 3 ? data[0] : 0), (data[1] >= 3 ? data[1] : 0), (data[2] >= 3 ? data[2] : 0));
    for (size_t i = 0; i < *data_len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
    return ESP_OK;
    
mock_fallback:
    
    switch (mode) {
        case 0x01: // Show current data
            switch (pid) {
                case 0x00: // Supported PIDs 01-20
                    data[0] = 0xBF; data[1] = 0x3E; data[2] = 0xB8; data[3] = 0x13;
                    *data_len = 4;
                    break;
                case 0x01: // Monitor status since DTCs cleared
                    data[0] = 0x00; data[1] = 0x00; data[2] = 0x00; data[3] = 0x00; // No MIL, no DTCs
                    *data_len = 4;
                    break;
                case 0x02: // Freeze DTC
                    data[0] = 0x00; // No DTCs
                    *data_len = 1;
                    break;
                case 0x03: // Fuel system status
                    data[0] = 0x01; // Open loop due to insufficient engine temperature
                    *data_len = 1;
                    break;
                case 0x05: // Engine coolant temperature
                    data[0] = 90; // 90°C
                    *data_len = 1;
                    break;
                case 0x0C: // Engine RPM
                    data[0] = 0x1A; data[1] = 0xF8; // 1750 RPM
                    *data_len = 2;
                    break;
                case 0x0D: // Vehicle speed
                    data[0] = 60; // 60 km/h
                    *data_len = 1;
                    break;
                case 0x0F: // Intake air temperature
                    data[0] = 25; // 25°C
                    *data_len = 1;
                    break;
                case 0x10: // MAF air flow rate
                    data[0] = 0x02; data[1] = 0x58; // 6.0 g/s
                    *data_len = 2;
                    break;
                case 0x11: // Throttle position
                    data[0] = 20; // 20%
                    *data_len = 1;
                    break;
                case 0x04: // Calculated engine load
                    data[0] = 25; // 25%
                    *data_len = 1;
                    break;
                case 0x06: // Short term fuel trim - Bank 1
                    data[0] = 128; // 0%
                    *data_len = 1;
                    break;
                case 0x07: // Long term fuel trim - Bank 1
                    data[0] = 128; // 0%
                    *data_len = 1;
                    break;
                case 0x0B: // Intake manifold absolute pressure
                    data[0] = 100; // 100 kPa
                    *data_len = 1;
                    break;
                case 0x0E: // Timing advance
                    data[0] = 10; // 10°
                    *data_len = 1;
                    break;
                case 0x1C: // OBD standards this vehicle conforms to
                    data[0] = 0x07; // OBD-II
                    *data_len = 1;
                    break;
                case 0x1F: // Run time since engine start
                    data[0] = 0x00; data[1] = 0x0A; // 10 seconds
                    *data_len = 2;
                    break;
                case 0x20: // Supported PIDs 21-40
                    data[0] = 0xBF; data[1] = 0x3E; data[2] = 0xB8; data[3] = 0x13;
                    *data_len = 4;
                    break;
                case 0x21: // Distance traveled with MIL on
                    data[0] = 0x00; data[1] = 0x00; // 0 km
                    *data_len = 2;
                    break;
                case 0x2C: // Commanded EGR
                    data[0] = 0; // 0%
                    *data_len = 1;
                    break;
                case 0x2D: // EGR Error
                    data[0] = 128; // 0%
                    *data_len = 1;
                    break;
                case 0x2E: // Commanded evaporative purge
                    data[0] = 0; // 0%
                    *data_len = 1;
                    break;
                case 0x2F: // Fuel Tank Level Input
                    data[0] = 50; // 50%
                    *data_len = 1;
                    break;
                case 0x30: // Warm-ups since codes cleared
                    data[0] = 5; // 5 times
                    *data_len = 1;
                    break;
                case 0x31: // Distance traveled since codes cleared
                    data[0] = 0x00; data[1] = 0x64; // 100 km
                    *data_len = 2;
                    break;
                case 0x33: // Barometric pressure
                    data[0] = 100; // 100 kPa
                    *data_len = 1;
                    break;
                case 0x42: // Control module voltage
                    data[0] = 0x0F; data[1] = 0xA0; // 14.88V
                    *data_len = 2;
                    break;
                case 0x43: // Absolute load value
                    data[0] = 0x00; data[1] = 0x19; // 2.5%
                    *data_len = 2;
                    break;
                case 0x44: // Fuel/Air commanded equivalence ratio
                    data[0] = 0x80; data[1] = 0x00; // 1.0
                    *data_len = 2;
                    break;
                case 0x46: // Ambient air temperature
                    data[0] = 20; // 20°C
                    *data_len = 1;
                    break;
            }
            break;

        case 0x03: // Show stored DTCs
            // No DTCs stored
            *data_len = 0;
            break;

        case 0x04: // Clear DTCs and stored values
            // Clear DTCs - return OK (no data)
            *data_len = 0;
            break;

        case 0x07: // Show pending DTCs
            // No pending DTCs
            *data_len = 0;
            break;

        case 0x0A: // Show permanent DTCs
            // No permanent DTCs
            *data_len = 0;
            break;

        case 0x09: // Vehicle information
            switch (pid) {
                case 0x01: // VIN message count
                    data[0] = 0x01; // 1 message
                    *data_len = 1;
                    break;
                case 0x02: // VIN
                    memcpy(data, "1FTFW1ET3DFC12345", 17); // Mock VIN
                    *data_len = 17;
                    break;
                default:
                    *data_len = 0;
                    break;
            }
            break;

        default:
            return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OBD: CMD=%02X%02X MOCK RESP=", mode, pid);
    for (size_t i = 0; i < *data_len; i++) {
        printf("%02X ", data[i]);
    }
    printf("\n");
    return ESP_OK;
}

static esp_err_t send_can_message(uint32_t id, const uint8_t *data, size_t len)
{
    if (!is_client_connected || elm327_shutdown) {
        ESP_LOGW(TAG, "📤 CAN TX BLOCKED: Client not connected or shutting down");
        return ESP_FAIL;
    }
    
    if (len > 8) {
        return ESP_ERR_INVALID_SIZE;
    }

    twai_message_t message = {
        .identifier = id,
        .data_length_code = len,
        .data = {0}
    };
    memcpy(message.data, data, len);

    esp_err_t ret = twai_transmit(&message, pdMS_TO_TICKS(100));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📤 CAN TX: ID=0x%03X, DLC=%d, Data=", id, len);
        for (size_t i = 0; i < len; i++) {
            printf("%02X ", data[i]);
        }
        printf("\n");
    } else {
        ESP_LOGE(TAG, "❌ CAN TX FAILED: ID=0x%03X, error=%s", id, esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t receive_can_response(uint32_t expected_id, uint8_t *data, size_t *data_len, uint32_t timeout_ms)
{
    twai_message_t message;
    esp_err_t ret = twai_receive(&message, pdMS_TO_TICKS(timeout_ms));

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "📥 CAN RX: ID=0x%03X, DLC=%d, Data=", message.identifier, message.data_length_code);
        for (size_t i = 0; i < message.data_length_code; i++) {
            printf("%02X ", message.data[i]);
        }
        printf("\n");
        if (message.identifier == expected_id && message.data_length_code > 0) {
            *data_len = message.data_length_code;
            memcpy(data, message.data, *data_len);
            return ESP_OK;
        }
    } else if (ret != ESP_ERR_TIMEOUT) {
        ESP_LOGE(TAG, "❌ CAN RX FAILED: error=%s", esp_err_to_name(ret));
    }
    return ESP_FAIL;
}