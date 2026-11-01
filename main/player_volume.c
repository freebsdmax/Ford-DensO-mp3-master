#include "player_volume.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "audio_element.h"
#include "audio_mem.h"

static const char *TAG = "Volume";
static const char *NVS_NS = "audio_player";
static const char *NVS_KEY = "volume";

#define VOLUME_DEFAULT 80

static volatile int s_volume = VOLUME_DEFAULT;

void player_volume_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, NVS_KEY, &v) == ESP_OK && v <= 100) {
            s_volume = v;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Volume: %d%%", s_volume);
}

int player_volume_get(void)
{
    return s_volume;
}

void player_volume_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_volume = percent;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY, (uint8_t)percent);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "Volume set to %d%%", percent);
}

void player_volume_apply_i16(void *buf, size_t bytes)
{
    if (!buf || bytes < 2) return;
    const int vol = s_volume;
    if (vol >= 100) return;   /* nothing to do - keeps the fast path */

    int16_t *s = (int16_t *)buf;
    size_t count = bytes / 2;
    if (vol == 0) {
        memset(buf, 0, bytes);
        return;
    }
    for (size_t i = 0; i < count; i++) {
        s[i] = (int16_t)(((int32_t)s[i] * vol) / 100);
    }
}

/* ---------------- ADF element ---------------- */

static int _vol_open(audio_element_handle_t self)
{
    (void)self;
    return ESP_OK;
}

static int _vol_close(audio_element_handle_t self)
{
    (void)self;
    return ESP_OK;
}

static esp_err_t _vol_destroy(audio_element_handle_t self)
{
    (void)self;
    return ESP_OK;
}

static int _vol_process(audio_element_handle_t self, char *in_buf, int in_len)
{
    int r = audio_element_input(self, in_buf, in_len);
    if (r > 0) {
        player_volume_apply_i16(in_buf, (size_t)r);
    }
    return r;
}

audio_element_handle_t volume_stream_init(void)
{
    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = _vol_open;
    cfg.close = _vol_close;
    cfg.process = _vol_process;
    cfg.destroy = _vol_destroy;
    cfg.tag = "volume";
    cfg.task_stack = 3072;
    cfg.task_core = 1;              /* audio core */
    cfg.out_rb_size = 4 * 1024;
    cfg.buffer_len = 1024;

    audio_element_handle_t el = audio_element_init(&cfg);
    if (!el) {
        ESP_LOGE(TAG, "volume element init failed");
    }
    return el;
}
