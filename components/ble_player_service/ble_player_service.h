#ifndef BLE_PLAYER_SERVICE_H
#define BLE_PLAYER_SERVICE_H

#include <esp_err.h>
#include <esp_gap_ble_api.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Status структура для BLE
 */
typedef struct {
    int current_track;
    int position_ms;
    int duration_ms;
    int is_paused;
    int playlist_size;
    int mode_external;
    int sniffer_enabled;
} ble_player_status_t;

/**
 * @brief Callback тип для команд з BLE клієнта
 */
typedef void (*ble_command_callback_t)(const char *command, const char *params);

/**
 * @brief Callback тип для отримання статусу
 */
typedef void (*ble_status_callback_t)(ble_player_status_t *status);
typedef size_t (*ble_playlist_callback_t)(char *buffer, size_t buffer_size);
typedef size_t (*ble_rpc_callback_t)(const char *request, char *response, size_t response_size);

/**
 * @brief Binary OTA frame: [0xA5][0x01][offset:4 LE][len:2 LE][data]
 *
 * Sent with Write Without Response, so no GATT reply is produced.
 * @return true when the frame was stored.
 */
typedef bool (*ble_ota_frame_callback_t)(uint32_t offset, const uint8_t *data, size_t len);

/**
 * @brief Ініціалізація BLE Player Service
 * @return ESP_OK на успіх
 */
esp_err_t ble_player_service_init(void);

/**
 * @brief Встановити callback для обробки команд
 */
void ble_player_set_command_handler(ble_command_callback_t callback);

/**
 * @brief Запустити періодичну відправку статусу
 */
void ble_player_start_periodic_status(ble_status_callback_t callback, int interval_ms);

/** @brief Установить callback для получения текущего плейлиста */
void ble_player_set_playlist_callback(ble_playlist_callback_t callback);
void ble_player_set_rpc_callback(ble_rpc_callback_t callback);
void ble_player_set_ota_frame_handler(ble_ota_frame_callback_t callback);

// Advertising параметри
#define PROFILE_NUM 1
#define GATTS_DEMO_CHAR_VAL_LEN_MAX 500

static const uint8_t primary_service_uuid[16] = { 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };
static const uint8_t character_declaration_uuid[16] = { 0x03, 0x28, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };
static const uint8_t character_client_config_uuid[16] = { 0x02, 0x29, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb };

#define adv_config_flag      (1 << 0)
#define scan_rsp_config_flag (1 << 1)

#endif // BLE_PLAYER_SERVICE_H
