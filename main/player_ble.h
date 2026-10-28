#pragma once
#include "ble_player_service.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

void ble_get_player_status(ble_player_status_t *status);
size_t ble_get_player_playlist(char *buffer, size_t buffer_size);
void ble_restart_task(void *arg);
size_t ble_rpc_handler(const char *request, char *response, size_t response_size);
bool ble_file_ota_frame(uint32_t offset, const uint8_t *data, size_t len);
void ble_command_handler(const char *command, const char *params);
const char *ble_rpc_string(cJSON *root, const char *name, const char *fallback);
