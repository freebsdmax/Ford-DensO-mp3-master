#pragma once
#include <stdbool.h>
#include <stdint.h>

/**
 * «Чорна скринька»: періодично дописує нові рядки логів у /storage/log.txt,
 * щоб після збою/перезавантаження було видно, що сталося.
 * Ротація: log.txt -> log.old (максимум ~2 x 256 КБ).
 */
void player_logfile_start(void);

bool player_logfile_enabled(void);
void player_logfile_set_enabled(bool enabled);
