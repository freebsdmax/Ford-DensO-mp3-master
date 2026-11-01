#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_element.h"

/** Software volume: 0..100 %, applied to 16-bit stereo PCM in place. */

void player_volume_load(void);
int player_volume_get(void);
void player_volume_set(int percent);

/** Applies the current volume to a 16-bit signed stereo buffer (in place). */
void player_volume_apply_i16(void *buf, size_t bytes);

/** ADF element that applies the volume between the decoder and the I2S writer. */
audio_element_handle_t volume_stream_init(void);
