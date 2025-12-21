#ifndef ELM327_WIFI_H
#define ELM327_WIFI_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize ELM327 WiFi TCP server
 * @param port TCP port to listen on (default 35000)
 * @return ESP_OK on success, ESP_FAIL otherwise
 */
esp_err_t elm327_wifi_init(uint16_t port);

/**
 * @brief Deinitialize ELM327 WiFi TCP server
 * @return ESP_OK on success, ESP_FAIL otherwise
 */
esp_err_t elm327_wifi_deinit(void);

/**
 * @brief Send data to connected TCP client
 * @param data Data to send
 * @param len Length of data
 * @return ESP_OK on success, ESP_FAIL otherwise
 */
esp_err_t elm327_wifi_send(const uint8_t *data, size_t len);

/**
 * @brief Check if TCP client is connected
 * @return true if connected, false otherwise
 */
bool elm327_wifi_is_connected(void);

// Shutdown flag for clean termination
extern bool elm327_shutdown;

#ifdef __cplusplus
}
#endif

#endif // ELM327_WIFI_H