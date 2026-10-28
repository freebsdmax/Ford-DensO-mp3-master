#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int sleep_mode;
    int idle_timeout_sec;
} sleep_settings_t;

extern sleep_settings_t g_sleep_settings;
extern uint32_t g_last_system_activity_time;

void update_system_activity(void);
esp_err_t get_sleep_settings(int *sleep_mode, int *idle_timeout_sec);
esp_err_t set_sleep_settings(int sleep_mode, int idle_timeout_sec);
esp_err_t load_sleep_settings(sleep_settings_t *settings);
esp_err_t save_sleep_settings(const sleep_settings_t *settings);
bool should_enter_sleep(void);
void enter_configured_sleep(void);
