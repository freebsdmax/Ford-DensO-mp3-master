#pragma once
#include "esp_err.h"
#include "driver/twai.h"
#include "wifi_can_sniffer.h"
#include <stdint.h>

#define CAN_ID_MAIN 0x2D5
#define CMD_NEXT_TRACK 0x49
#define CMD_PREV_TRACK 0x89
#define CMD_MODE_SWITCH 0x19
#define CAN_DEFAULT_BITRATE 125
#define CAN_ALERT_FLAGS TWAI_ALERT_ALL
#define CAN_MODE TWAI_MODE_NORMAL
#define CAN_TX_GPIO GPIO_NUM_4
#define CAN_RX_GPIO GPIO_NUM_15
#define CAN_COMMAND_DEBOUNCE_MS 500
#define CAN_SLEEP_CHECK_PERIOD 1000
extern uint32_t g_last_next_command_time;
extern uint32_t g_last_prev_command_time;
extern bool g_playback_was_active_before_sleep;
twai_timing_config_t get_can_timing_config(uint16_t bitrate_kbps);

extern bool g_is_paused;
extern uint32_t g_sleep_start_time;
#define CAN_WAKEUP_DELAY_MS 5000

extern uint16_t g_can_bitrate;
extern uint16_t g_can_frames_per_second;
extern uint16_t g_can_bytes_per_second;
extern uint64_t g_can_total_frames;
extern uint64_t g_can_total_bytes;

esp_err_t init_can(void);
void can_task(void *arg);
void can_sleep_monitor_task(void *arg);
void process_can_message(twai_message_t *message);
void send_can_event(uint8_t event_type, uint8_t data1, uint8_t data2);
void send_can_status(void);
esp_err_t change_can_bitrate(uint16_t bitrate);
void wifi_can_on_message_callback(const can_sniffer_frame_t *frame);
void wifi_can_on_control_callback(uint8_t cmd, const uint8_t *data, uint8_t len);
