#include "player_nvs.h"
// Minimal NVS helpers to optimize play_music-master.c
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include <inttypes.h>
#include <string.h>

esp_err_t nvs_write_i32(const char *ns, const char *key, int32_t value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(h, key, value);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_read_i32_def(const char *ns, const char *key, int32_t def_val, int32_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = def_val;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    int32_t v = 0;
    err = nvs_get_i32(h, key, &v);
    nvs_close(h);
    if (err == ESP_OK) {
        *out = v;
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    return err;
}

esp_err_t nvs_write_str(const char *ns, const char *key, const char *str)
{
    if (!str) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, str);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t nvs_read_str_def(const char *ns, const char *key, char *buf, size_t buf_len, const char *def_str)
{
    if (!buf || buf_len == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    if (def_str) {
        strncpy(buf, def_str, buf_len - 1);
        buf[buf_len - 1] = '\0';
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_OK;
    size_t len = buf_len;
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err == ESP_OK) {
        buf[buf_len - 1] = '\0';
        return ESP_OK;
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (def_str) {
        strncpy(buf, def_str, buf_len - 1);
        buf[buf_len - 1] = '\0';
    }
    return ESP_OK;
}

static const char *TAG_NVS = "Player";
esp_err_t save_playback_position(int32_t track_index, int32_t position_ms) {
    // 🚨 ВАЖНО: Проверяем что позиция корректная
    if (position_ms < 0 || position_ms > 3600000) { // Больше 1 часа - ошибка
        ESP_LOGW(TAG_NVS, "⚠️ Подозрительная позиция для сохранения: %d мс", position_ms);
        position_ms = 0;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "💾 save_playback_position: Помилка відкриття NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    ESP_LOGI(TAG_NVS, "💾 ЗБЕРЕЖЕННЯ: трек=%d, позиція=%d мс", track_index, position_ms);
    
    // 🎯 Сохраняем также время сохранения для отладки
    uint32_t save_time = (uint32_t)(esp_timer_get_time() / 1000);
    err = nvs_set_u32(nvs_handle, "save_time_ms", save_time);
    
    err = nvs_set_i32(nvs_handle, "track_index", track_index);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "💾 save_playback_position: Помилка track_index: %s", esp_err_to_name(err));  // ИСПРАВЛЕНО: err вместо ret
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_set_i32(nvs_handle, "position_ms", position_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "💾 save_playback_position: Помилка position_ms: %s", esp_err_to_name(err));  // ИСПРАВЛЕНО: err вместо ret
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG_NVS, "✅ Позиція збережена (час: %"PRIu32" мс)", save_time);
    } else {
        ESP_LOGE(TAG_NVS, "❌ Помилка коміту NVS: %s", esp_err_to_name(err));
    }
    
    return err;
}

esp_err_t load_playback_position(int32_t *track_index, int32_t *position_ms)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "💾 load_playback_position: Помилка відкриття NVS: %s", esp_err_to_name(err));
        return err;
    }
    
    err = nvs_get_i32(nvs_handle, "track_index", track_index);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_NVS, "💾 load_playback_position: track_index не знайдено: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_get_i32(nvs_handle, "position_ms", position_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_NVS, "💾 load_playback_position: position_ms не знайдено: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }
    
    // 🚨 ВАЖНО: Проверяем корректность загруженных данных
    if (*position_ms < 0 || *position_ms > 3600000) { // > 1 часа
        ESP_LOGW(TAG_NVS, "⚠️ Завантажена підозріла позиція: %d мс, скидаємо до 0", *position_ms);
        *position_ms = 0;
    }
    
    // Проверяем время сохранения
    uint32_t save_time = 0;
    nvs_get_u32(nvs_handle, "save_time_ms", &save_time);  // Эта функция может вернуть ошибку, игнорируем
    uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000);
    
    if (save_time > 0) {
        uint32_t time_diff = current_time - save_time;
        ESP_LOGI(TAG_NVS, "💾 Позиція збережена %d секунд тому", time_diff / 1000);
    }
    
    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG_NVS, "📥 ЗАВАНТАЖЕНО: трек=%" PRId32 ", позиція=%" PRId32 " мс", 
             *track_index, *position_ms);
    
    return ESP_OK;
}

esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    return ret;
}

esp_err_t clear_playback_position(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_erase_key(nvs_handle, "track_index");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_erase_key(nvs_handle, "position_ms");
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        return err;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG_NVS, "🗑️ Збережену позицію очищено");
        g_saved_track_index = -1;
        g_saved_position_ms = 0;
    }
    
    return err;
}

esp_err_t save_wifi_mode_pref(int mode)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(nvs_handle, NVS_KEY_WIFI_MODE, mode);
    if (err == ESP_OK) err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG_NVS, "💾 Збережено wifi_mode=%d (0=AP,1=STA)", mode);
    }
    return err;
}

esp_err_t load_wifi_mode_pref(int *mode)
{
    if (!mode) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) { *mode = 0; return err; }
    int32_t val = 0;
    err = nvs_get_i32(nvs_handle, NVS_KEY_WIFI_MODE, &val);
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        *mode = (val != 0) ? 1 : 0;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *mode = 0; // default AP
        err = ESP_OK;
    } else {
        *mode = 0;
    }
    ESP_LOGI(TAG_NVS, "📥 Завантажено wifi_mode=%d (0=AP,1=STA)", *mode);
    return err;
}

esp_err_t save_sniffer_enabled(int enabled)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;
    err = nvs_set_i32(nvs_handle, NVS_KEY_SNIFFER_ENABLED, enabled ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    if (err == ESP_OK) ESP_LOGI(TAG_NVS, "💾 Збережено sniffer_enabled=%d", enabled ? 1 : 0);
    return err;
}

esp_err_t load_sniffer_enabled(int *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) { *enabled = 0; return err; }
    int32_t val = 0;
    err = nvs_get_i32(nvs_handle, NVS_KEY_SNIFFER_ENABLED, &val);
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        *enabled = (val != 0) ? 1 : 0;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *enabled = 0; // default disabled
        err = ESP_OK;
    } else {
        *enabled = 0;
    }
    ESP_LOGI(TAG_NVS, "📥 Завантажено sniffer_enabled=%d", *enabled);
    return err;
}

esp_err_t save_last_source_pref(int source)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_NVS, "💾 save_last_source_pref: Помилка відкриття NVS: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_i32(nvs_handle, NVS_KEY_LAST_SOURCE, source);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG_NVS, "💾 Збережено last_source=%d (0=USB,1=EXT)", source);
    } else {
        ESP_LOGE(TAG_NVS, "❌ Не вдалося зберегти last_source: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t load_last_source_pref(int *source)
{
    if (!source) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG_NVS, "ℹ️ load_last_source_pref: NVS не відкрито (використаємо дефолт USB): %s", esp_err_to_name(err));
        *source = 0;
        return err;
    }
    int32_t val = 0;
    err = nvs_get_i32(nvs_handle, NVS_KEY_LAST_SOURCE, &val);
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        *source = (val != 0) ? 1 : 0;
        ESP_LOGI(TAG_NVS, "📥 Завантажено last_source=%d (0=USB,1=EXT)", *source);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        *source = 0; // default USB
        ESP_LOGI(TAG_NVS, "📥 Ключ last_source відсутній — дефолт USB");
        err = ESP_OK;
    } else {
        ESP_LOGW(TAG_NVS, "⚠️ Неможливо отримати last_source: %s (дефолт USB)", esp_err_to_name(err));
        *source = 0;
    }
    return err;
}

esp_err_t save_sta_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_str(nvs_handle, NVS_KEY_STA_SSID, ssid);
    if (err != ESP_OK) goto cleanup;
    
    err = nvs_set_str(nvs_handle, NVS_KEY_STA_PASS, password);
    if (err != ESP_OK) goto cleanup;
    
    err = nvs_commit(nvs_handle);
    
cleanup:
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG_NVS, "💾 Збережено STA credentials: SSID='%s'", ssid);
    }
    return err;
}

esp_err_t load_sta_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        // Use defaults
        strncpy(ssid, WIFI_STA_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
        strncpy(password, WIFI_STA_PASSWORD, pass_len - 1);
        password[pass_len - 1] = '\0';
        return ESP_OK;
    }
    
    size_t len = ssid_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_STA_SSID, ssid, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(ssid, WIFI_STA_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    } else if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    len = pass_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_STA_PASS, password, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(password, WIFI_STA_PASSWORD, pass_len - 1);
        password[pass_len - 1] = '\0';
        err = ESP_OK;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG_NVS, "📥 Завантажено STA credentials: SSID='%s'", ssid);
    return err;
}

esp_err_t save_ap_credentials(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) return err;
    
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, ssid);
    if (err != ESP_OK) goto cleanup;
    
    err = nvs_set_str(nvs_handle, NVS_KEY_AP_PASS, password);
    if (err != ESP_OK) goto cleanup;
    
    err = nvs_commit(nvs_handle);
    
cleanup:
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG_NVS, "💾 Збережено AP credentials: SSID='%s'", ssid);
    }
    return err;
}

esp_err_t load_ap_credentials(char *ssid, size_t ssid_len, char *password, size_t pass_len)
{
    if (!ssid || !password) return ESP_ERR_INVALID_ARG;
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        // Use defaults
        strncpy(ssid, WIFI_AP_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
        strncpy(password, WIFI_AP_PASSWORD, pass_len - 1);
        password[pass_len - 1] = '\0';
        return ESP_OK;
    }
    
    size_t len = ssid_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, ssid, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(ssid, WIFI_AP_SSID, ssid_len - 1);
        ssid[ssid_len - 1] = '\0';
    } else if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }
    
    len = pass_len;
    err = nvs_get_str(nvs_handle, NVS_KEY_AP_PASS, password, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        strncpy(password, WIFI_AP_PASSWORD, pass_len - 1);
        password[pass_len - 1] = '\0';
        err = ESP_OK;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG_NVS, "📥 Завантажено AP credentials: SSID='%s'", ssid);
    return err;
}
