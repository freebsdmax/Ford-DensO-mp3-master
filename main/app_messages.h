#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "usb/msc_host.h"
#include <stdint.h>

extern QueueHandle_t app_queue;

typedef struct {
    enum {
        APP_QUIT,
        APP_DEVICE_CONNECTED,
        APP_DEVICE_DISCONNECTED,
        APP_MODE_SWITCH,
        APP_NEXT_TRACK,
        APP_PREV_TRACK,
        APP_SAVE_POSITION,
        APP_WIFI_SWITCH_STA,
        APP_WIFI_SWITCH_AP,
        APP_TOGGLE_PAUSE,
        APP_PLAY_TRACK,
        APP_SEEK_POSITION
    } id;
    union {
        uint8_t new_dev_address;
        msc_host_device_handle_t device_handle;
        int int_param;
    } data;
} app_message_t;
