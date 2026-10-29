#include "player_media.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "player_link.h"
#include "uart_ota_protocol.h"

static const char *TAG = "MEDIA";

static player_media_t s_state;
static SemaphoreHandle_t s_lock = NULL;
static volatile bool s_started = false;

static void media_lock(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void media_unlock(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

void player_media_get(player_media_t *out)
{
    if (!out) return;
    media_lock();
    *out = s_state;
    media_unlock();
}

bool player_media_available(void)
{
    bool ok;
    media_lock();
    ok = s_state.valid;
    media_unlock();
    return ok;
}

static void copy_field(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    if (!dst || cap == 0) return;
    if (len > cap - 1) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
    /* Strip control bytes so the JSON/UI stays valid */
    for (char *p = dst; *p; ++p) {
        if ((unsigned char)*p < 0x20 || *p == 0x7F) *p = ' ';
    }
}

static bool parse_payload(const uint8_t *buf, size_t len, player_media_t *out)
{
    if (len < 11) return false;

    size_t i = 0;
    uint8_t flags = buf[i++];
    uint32_t pos = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8) |
                   ((uint32_t)buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
    i += 4;
    uint32_t dur = (uint32_t)buf[i] | ((uint32_t)buf[i + 1] << 8) |
                   ((uint32_t)buf[i + 2] << 16) | ((uint32_t)buf[i + 3] << 24);
    i += 4;

    size_t tlen = buf[i++];
    if (i + tlen + 1 > len) return false;
    copy_field(out->title, sizeof(out->title), &buf[i], tlen);
    i += tlen;

    size_t alen = buf[i++];
    if (i + alen > len) return false;
    copy_field(out->artist, sizeof(out->artist), &buf[i], alen);

    out->pos_ms = pos;
    out->dur_ms = dur;
    out->playing = (flags & 0x02) != 0;
    out->valid = (flags & 0x01) != 0;
    return true;
}

static bool poll_once(player_media_t *out)
{
    if (uart_link_ota_active()) return false;
    if (!uart_link_lock(300)) return false;

    bool ok = false;
    uart_flush_input(MASTER_UART_PORT);

    const uint8_t req[2] = { UART_CMD_PREFIX_NORMAL, UART_CMD_GET_MEDIA };
    if (uart_write_bytes(MASTER_UART_PORT, (const char *)req, 2) == 2) {
        uint8_t hdr[3];
        if (uart_read_bytes(MASTER_UART_PORT, hdr, 3, pdMS_TO_TICKS(500)) == 3 &&
            hdr[0] == UART_CMD_PREFIX_NORMAL && hdr[1] == UART_CMD_MEDIA_STATE) {
            size_t n = hdr[2];
            uint8_t payload[192];
            if (n <= sizeof(payload) &&
                uart_read_bytes(MASTER_UART_PORT, payload, n, pdMS_TO_TICKS(500)) == (int)n) {
                ok = parse_payload(payload, n, out);
            }
        }
    }

    uart_link_unlock();
    return ok;
}

static void media_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(4000));
    int misses = 0;
    while (true) {
        player_media_t fresh;
        memset(&fresh, 0, sizeof(fresh));

        if (poll_once(&fresh)) {
            media_lock();
            s_state = fresh;
            media_unlock();
            misses = 0;
            if (fresh.valid) {
                ESP_LOGD(TAG, "media: '%s' / '%s' %lu/%lu ms %s",
                         fresh.title, fresh.artist,
                         (unsigned long)fresh.pos_ms, (unsigned long)fresh.dur_ms,
                         fresh.playing ? "playing" : "paused");
            }
        } else {
            if (++misses >= 3) {
                media_lock();
                if (s_state.valid) {
                    ESP_LOGW(TAG, "slave stopped reporting media");
                }
                s_state.valid = false;
                s_state.playing = false;
                media_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void player_media_start(void)
{
    if (s_started) return;
    s_started = true;
    media_lock();
    memset(&s_state, 0, sizeof(s_state));
    media_unlock();
    xTaskCreate(media_task, "media_poll", 4096, NULL, 3, NULL);
}

bool player_media_seek_by(int32_t delta_ms)
{
    if (delta_ms == 0) return false;
    if (uart_link_ota_active()) return false;
    if (!uart_link_lock(500)) return false;

    uint8_t pkt[6];
    pkt[0] = UART_CMD_PREFIX_NORMAL;
    pkt[1] = UART_CMD_SEEK;
    uint32_t d = (uint32_t)delta_ms;
    pkt[2] = (uint8_t)(d & 0xFF);
    pkt[3] = (uint8_t)((d >> 8) & 0xFF);
    pkt[4] = (uint8_t)((d >> 16) & 0xFF);
    pkt[5] = (uint8_t)((d >> 24) & 0xFF);
    int w = uart_write_bytes(MASTER_UART_PORT, (const char *)pkt, sizeof(pkt));
    uart_link_unlock();

    if (w != (int)sizeof(pkt)) {
        ESP_LOGW(TAG, "seek command not sent (%d bytes)", w);
        return false;
    }
    ESP_LOGI(TAG, "seek %+ld ms sent to slave", (long)delta_ms);
    return true;
}

bool player_media_seek_to(uint32_t target_ms)
{
    player_media_t cur;
    player_media_get(&cur);
    int32_t delta = (int32_t)target_ms - (int32_t)cur.pos_ms;
    return player_media_seek_by(delta);
}
