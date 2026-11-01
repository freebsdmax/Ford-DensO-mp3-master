#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * Профілі аудіо-буферів (I2S RX, режим passthrough зі Slave).
 *
 * 0 — низька затримка  (4 x 256  ≈ 23 мс)
 * 1 — збалансований    (6 x 512  ≈ 70 мс)  ← типово, як було
 * 2 — стійкий          (8 x 1024 ≈ 186 мс)
 *
 * Значення застосовуються при наступному створенні I2S-каналу
 * (перемикання режиму або перезавантаження).
 */

#define AUDIO_PROFILE_LOW      0
#define AUDIO_PROFILE_BALANCED 1
#define AUDIO_PROFILE_ROBUST   2

void player_audio_cfg_load(void);
uint8_t player_audio_cfg_profile(void);
esp_err_t player_audio_cfg_set_profile(uint8_t profile);

/** DMA-параметри для I2S RX (passthrough). */
void player_audio_cfg_rx(uint32_t *desc_num, uint32_t *frame_num);
