#include "player_audio_cfg.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "AudioCfg";

static volatile uint8_t s_profile = AUDIO_PROFILE_BALANCED;

void player_audio_cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open("audio_player", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = AUDIO_PROFILE_BALANCED;
        if (nvs_get_u8(h, "audio_profile", &v) == ESP_OK && v <= AUDIO_PROFILE_ROBUST) {
            s_profile = v;
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "audio profile: %u", (unsigned)s_profile);
}

uint8_t player_audio_cfg_profile(void)
{
    return s_profile;
}

esp_err_t player_audio_cfg_set_profile(uint8_t profile)
{
    if (profile > AUDIO_PROFILE_ROBUST) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open("audio_player", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, "audio_profile", profile);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        s_profile = profile;
        ESP_LOGI(TAG, "audio profile set to %u", (unsigned)profile);
    }
    return err;
}

void player_audio_cfg_rx(uint32_t *desc_num, uint32_t *frame_num)
{
    uint32_t desc = 6;
    uint32_t frame = 512;

    switch (s_profile) {
    case AUDIO_PROFILE_LOW:
        desc = 4;
        frame = 256;
        break;
    case AUDIO_PROFILE_ROBUST:
        desc = 8;
        frame = 1024;
        break;
    default:
        desc = 6;
        frame = 512;
        break;
    }

    if (desc_num) *desc_num = desc;
    if (frame_num) *frame_num = frame;
}
