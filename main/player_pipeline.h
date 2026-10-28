#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_element.h"
#include "board.h"
#include "driver/i2s_std.h"
#include <stdint.h>

void debug_audio_state(audio_element_handle_t el, const char *name);
void debug_file_position(audio_element_handle_t el, const char *context);
void flush_audio_buffers(void);
void cleanup_i2s_driver(void);
void safe_i2s_cleanup(void);
int get_current_position(audio_element_handle_t fatfs_stream_reader);
void player_set_position(int position_ms);
void set_volume(int volume_percent);
void volume_up(void);
void volume_down(void);
void stop_playback(void);
void play_next_track(void);
void play_prev_track(void);
void stop_external_input(void);

extern volatile bool g_mode_external;
extern audio_element_handle_t g_external_writer;
extern TaskHandle_t g_i2s_passthrough_task_handle;
extern TaskHandle_t g_player_task_handle;
extern bool g_skip_to_next;
extern bool g_skip_to_prev;
extern audio_board_handle_t g_board_handle;
extern bool g_can_sleep_mode;
extern uint32_t g_last_can_activity_time;
extern int g_last_saved_position;
extern volatile bool g_external_i2s_flush_requested;
extern int g_volume;
extern int32_t g_seek_position_ms;
#define EXT_I2S_DIN_PIN 40
#define EXT_I2S_BCLK_PIN 41
#define EXT_I2S_WS_PIN 39
extern volatile uint32_t g_external_i2s_rx_overflows;
extern volatile uint32_t g_external_i2s_rx_overflow_bytes;
void analyze_mp3_file(const char *filename);
esp_err_t audio_event_cb(audio_element_handle_t el, audio_event_iface_msg_t *event, void *ctx);
esp_err_t set_playback_position(audio_element_handle_t fatfs_stream_reader, int position_ms);
esp_err_t start_external_input(void);
void stop_playback_and_services(void);
void wakeup_from_sleep(void);
void player_control(const char *cmd, int track);
esp_err_t get_slave_fw_version(char *buf, size_t len);
esp_err_t get_slave_version_extended(char *main_date, size_t main_date_len, char *main_time, size_t main_time_len, char *crc_date, size_t crc_date_len, char *crc_time, size_t crc_time_len, char *uart_date, size_t uart_date_len, char *uart_time, size_t uart_time_len);
void i2s_passthrough_task(void *pvParameters);
void player_task(void *pvParam);
void play_test_tone(audio_element_handle_t i2s_writer, int duration_ms);


#define WIFI_SSID        "ESP32-CAN-Sniffer"
#define WIFI_PASSWORD    "12345678"
#define PC_IP_ADDR       "192.168.4.255"
#define UDP_PORT         0xAA55
extern bool g_play_specific_track;
extern bool g_restore_playback;
extern size_t g_stall_last_bytes;
extern uint32_t g_last_position_save_time;
#define POSITION_SAVE_INTERVAL_MS 60000
#define MIN_POSITION_CHANGE_MS 10000
#define PLAYBACK_STALL_TIMEOUT_MS 8000
extern int64_t g_stall_last_ts_ms;
extern int32_t g_current_track_duration_ms;
extern bool g_is_paused;
extern bool g_sniffer_enabled;

extern i2s_chan_handle_t g_i2s_tx_handle;
