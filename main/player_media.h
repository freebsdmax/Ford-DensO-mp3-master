#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/** What the phone (via the Bluetooth slave) is currently playing. */
typedef struct {
    bool valid;        /* slave answered at least once */
    bool playing;
    uint32_t pos_ms;
    uint32_t dur_ms;
    char title[80];
    char artist[48];
} player_media_t;

/** Starts the background task that polls the slave once per second. */
void player_media_start(void);

/** Thread-safe copy of the last known state. */
void player_media_get(player_media_t *out);

/** True when the slave reports a valid (non-empty) media session. */
bool player_media_available(void);

/** Asks the slave to seek to an absolute position (approximated with FF/REW). */
bool player_media_seek_to(uint32_t target_ms);

/** Forward a relative seek (positive/negative milliseconds). */
bool player_media_seek_by(int32_t delta_ms);
