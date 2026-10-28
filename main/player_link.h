#pragma once
#include "esp_err.h"
#include "driver/uart.h"
#include "uart_ota_protocol.h"
#include <stdint.h>
#include <stdbool.h>

#define MASTER_UART_PORT           UART_NUM_1
#define MASTER_UART_TX_PIN         8
#define MASTER_UART_RX_PIN         9
#define MASTER_UART_BAUD           115200
#define MASTER_UART_RX_BUF         256
#define MASTER_UART_TX_BUF         256
/**
 * Serialises the UART with the slave: an OTA transfer and the version polling
 * must never interleave, otherwise each side eats the other's replies.
 */
bool uart_link_lock(uint32_t timeout_ms);
void uart_link_unlock(void);
bool uart_link_ota_active(void);
void uart_link_set_ota_active(bool active);

#define LINK_CMD_NEXT_TRACK        UART_CMD_NEXT_TRACK
#define LINK_CMD_PREV_TRACK        UART_CMD_PREV_TRACK

void gpio_cb(void *arg);
esp_err_t send_i2c_command(uint8_t cmd);
esp_err_t init_i2c_master(void);
