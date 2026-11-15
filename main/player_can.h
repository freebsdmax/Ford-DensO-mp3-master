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
#define CAN_COMMAND_DEBOUNCE_MS 500   /* типове значення, змінюється в налаштуваннях */
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
void player_can_load_settings(void);
bool player_can_get_settings(uint16_t *bitrate, uint32_t *id,
                             uint8_t *mn, uint8_t *mp, uint8_t *mm, uint16_t *debounce);
esp_err_t player_can_set_settings(uint16_t bitrate, uint32_t id,
                                  uint8_t mn, uint8_t mp, uint8_t mm, uint16_t debounce);

/*
 * Навчання CAN-кнопок.
 *
 * 1) action: 0 = NEXT, 1 = PREV, 2 = MODE.
 * 2) Перші `seconds` секунд збираємо "фон" — усі (ID, старший нібл data[1]),
 *    які постійно бігають по шині.
 * 3) Далі чекаємо до 30 с на кадр, якого НЕ було у фоні — це і є натискання.
 * 4) Знайдений ID і маска зберігаються в NVS (can_id / can_mn / can_mp / can_mm).
 */
typedef struct __attribute__((packed)) {
    uint32_t id;      /* CAN ID */
    uint8_t  byte;    /* індекс байта даних (0..7) */
    uint8_t  mask;    /* 0 = достатньо збігу ID */
    uint8_t  value;   /* (data[byte] & mask) == value */
    uint8_t  action;  /* 0 = NEXT, 1 = PREV, 2 = MODE */
} can_binding_t;

#define CAN_KEYS_MAX 24

int  player_can_keys_get(can_binding_t *out, int max);
esp_err_t player_can_keys_add(const can_binding_t *b);
esp_err_t player_can_keys_remove(int index);
esp_err_t player_can_keys_clear(void);

esp_err_t player_can_learn_start(int action, int seconds);
void player_can_learn_cancel(void);
/* state: 0 idle, 1 background, 2 waiting, 3 done, 4 failed */
void player_can_learn_status(int *state, int *action, int *secs_left,
                             uint32_t *id, uint8_t *data1);
void can_task(void *arg);
void can_sleep_monitor_task(void *arg);
void process_can_message(twai_message_t *message);
void send_can_event(uint8_t event_type, uint8_t data1, uint8_t data2);
void send_can_status(void);
esp_err_t change_can_bitrate(uint16_t bitrate);
void wifi_can_on_message_callback(const can_sniffer_frame_t *frame);
void wifi_can_on_control_callback(uint8_t cmd, const uint8_t *data, uint8_t len);
