#ifndef ELM327_EMU_H
#define ELM327_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ELM327 emulator configuration
 */
typedef struct {
    bool enabled;       // Master enable
    bool wifi_enabled;  // Enable Wi-Fi TCP server on port 35000
    bool bt_enabled;    // Enable Bluetooth Classic SPP
} elm327_config_t;

/**
 * @brief Initialize and start ELM327 emulator
 * @param config Configuration structure
 * @return ESP_OK on success
 */
esp_err_t elm327_emu_start(const elm327_config_t *config);

/**
 * @brief Stop ELM327 emulator
 * @return ESP_OK on success
 */
esp_err_t elm327_emu_stop(void);

/**
 * @brief Get current configuration
 * @param config Output configuration structure
 * @return ESP_OK on success
 */
esp_err_t elm327_emu_get_config(elm327_config_t *config);

/**
 * @brief Set and apply new configuration (saves to NVS)
 * @param config New configuration
 * @return ESP_OK on success
 */
esp_err_t elm327_emu_set_config(const elm327_config_t *config);

/**
 * @brief Load configuration from NVS
 * @param config Output configuration structure
 * @return ESP_OK on success
 */
esp_err_t elm327_emu_load_config(elm327_config_t *config);

#ifdef __cplusplus
}
#endif

#endif // ELM327_EMU_H
