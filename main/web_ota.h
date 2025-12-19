#pragma once
#include "esp_err.h"

esp_err_t web_ota_start(void);
void web_ota_stop(void);

// I2C slave OTA API
esp_err_t web_ota_i2c_flash(const uint8_t *data, size_t len);
