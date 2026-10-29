#include "player_ble.h"
#include "player_pipeline.h"
#include "player_playlist.h"
#include "player_nvs.h"
#include "player_sleep.h"
#include "player_logs.h"
#include "player_slave.h"
#include "player_media.h"
#include "player_slave_ota.h"
#include "player_file_ota.h"
#include "player_ble_ota.h"
#include "player_can.h"
#include "app_messages.h"
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "web_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG_BLE = "Player";
void player_control(const char *cmd, int track);


extern bool g_verbose_logs;
extern bool g_player_logs;
extern bool g_wifi_can_sniffer_logs;
extern bool g_web_ota_logs;
extern bool g_uart_ota_logs;
extern bool g_elm327_logs;
extern int32_t g_current_track_duration_ms;
extern bool g_sniffer_enabled;
extern int g_volume;
const char *ble_rpc_string(cJSON *root, const char *name, const char *fallback);
esp_err_t set_wifi_settings(const char *sta_ssid, const char *sta_password, const char *ap_ssid, const char *ap_password, int mode);
esp_err_t get_wifi_settings(char *sta_ssid, size_t sta_ssid_len, char *sta_password, size_t sta_pass_len, char *ap_ssid, size_t ap_ssid_len, char *ap_password, size_t ap_pass_len, int *mode);


void ble_get_player_status(ble_player_status_t *status) {
    if (status == NULL) return;
    
    status->current_track = g_current_track;
    status->position_ms = g_last_saved_position;
    status->duration_ms = g_current_track_duration_ms;
    status->is_paused = g_is_paused;
    status->playlist_size = g_playlist_size;
    status->mode_external = g_mode_external;
    status->sniffer_enabled = g_sniffer_enabled;
}

size_t ble_get_player_playlist(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    size_t used = 0;
    buffer[0] = '\0';
    for (int i = 0; i < g_playlist_size; i++) {
        char name[256];
        playlist_get_display_name(i, name, sizeof(name));
        size_t name_length = strlen(name);
        size_t separator_length = used > 0 ? 1 : 0;
        if (name_length + separator_length >= buffer_size - used) {
            break;
        }

        if (separator_length) {
            buffer[used++] = '|';
        }
        memcpy(buffer + used, name, name_length);
        used += name_length;
        buffer[used] = '\0';
    }

    return used;
}

size_t ble_rpc_return_json(cJSON *root, char *response, size_t response_size)
{
    if (root == NULL || response == NULL || response_size == 0) {
        cJSON_Delete(root);
        return 0;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return 0;
    }

    size_t length = strlen(json);
    if (length >= response_size) {
        length = response_size - 1;
    }
    memcpy(response, json, length);
    response[length] = '\0';
    cJSON_free(json);
    return length;
}

void ble_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(750));
    esp_restart();
}

bool ble_rpc_save_component_logs(cJSON *root)
{
    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(root, "groups");
    if (cJSON_IsObject(groups)) {
        for (int g = 0; g < LOG_GROUP_COUNT; g++) {
            const cJSON *it = cJSON_GetObjectItemCaseSensitive(groups, player_logs_group_key(g));
            if (cJSON_IsNumber(it)) player_logs_group_set(g, it->valueint != 0);
        }
        player_logs_save();
        player_logs_apply();
        esp_log_level_set("Player", g_player_logs ? ESP_LOG_INFO : ESP_LOG_NONE);
        esp_log_level_set("WiFiCanSniffer", g_wifi_can_sniffer_logs ? ESP_LOG_INFO : ESP_LOG_NONE);
        esp_log_level_set("web_ota", g_web_ota_logs ? ESP_LOG_INFO : ESP_LOG_NONE);
        return true;
    }

    const char *keys[] = {"player", "wifi_can_sniffer", "web_ota", "uart_ota", "elm327"};
    const char *nvs_keys[] = {"player_logs", "wifi_logs", "web_ota_logs", "uart_ota_logs", "elm327_logs"};
    bool *flags[] = {&g_player_logs, &g_wifi_can_sniffer_logs, &g_web_ota_logs,
                     &g_uart_ota_logs, &g_elm327_logs};
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }

    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
        if (cJSON_IsNumber(value)) {
            *flags[i] = value->valueint != 0;
        }
        if (err == ESP_OK) {
            err = nvs_set_u8(handle, nvs_keys[i], *flags[i] ? 1 : 0);
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    const char *mod_keys[] = {"pipeline","can","usb","ble","wifi","sleep","playlist","console","link","nvs"};
    for (size_t i = 0; i < LOG_MOD_COUNT; i++) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, mod_keys[i]);
        if (cJSON_IsNumber(value)) {
            *player_logs_flag_ptr((int)i) = value->valueint != 0;
        }
    }
    for (int g = 0; g < LOG_GROUP_COUNT; g++) {
        char gkey[24];
        snprintf(gkey, sizeof(gkey), "group_%s", player_logs_group_key(g));
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, gkey);
        if (cJSON_IsNumber(it)) player_logs_group_set(g, it->valueint != 0);
    }

    player_logs_save();
    player_logs_apply();

    esp_log_level_set("Player", g_player_logs ? ESP_LOG_INFO : ESP_LOG_NONE);
    esp_log_level_set("WiFiCanSniffer", g_wifi_can_sniffer_logs ? ESP_LOG_INFO : ESP_LOG_NONE);
    esp_log_level_set("web_ota", g_web_ota_logs ? ESP_LOG_INFO : ESP_LOG_NONE);
    return err == ESP_OK;
}

bool ble_file_ota_frame(uint32_t offset, const uint8_t *data, size_t len)
{
    /* Binary OTA frame from the GATT write callback: no RPC round trip. */
    return file_ota_write_at((size_t)offset, data, len) == ESP_OK;
}

size_t ble_rpc_handler(const char *request, char *response, size_t response_size)
{
    if (request == NULL || response == NULL || response_size == 0) {
        return 0;
    }

    char request_copy[512];
    strlcpy(request_copy, request, sizeof(request_copy));
    char *payload = strchr(request_copy, ':');
    if (payload) {
        *payload++ = '\0';
    }

    if (strcmp(request_copy, "logs.get") == 0) {
        size_t length = web_ota_copy_logs(response, response_size);
        if (length == 0 && response_size > 1) {
            response[0] = '\n';
            response[1] = '\0';
            return 1;
        }
        return length;
    }

    if (strcmp(request_copy, "wifi.get") == 0) {
        char sta_ssid[32] = "", sta_password[64] = "";
        char ap_ssid[32] = "", ap_password[64] = "";
        int mode = 0;
        if (get_wifi_settings(sta_ssid, sizeof(sta_ssid), sta_password, sizeof(sta_password),
                              ap_ssid, sizeof(ap_ssid), ap_password, sizeof(ap_password), &mode) != ESP_OK) {
            return 0;
        }
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddStringToObject(root, "sta_ssid", sta_ssid);
        cJSON_AddStringToObject(root, "sta_password", "");
        cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);
        cJSON_AddStringToObject(root, "ap_password", "");
        cJSON_AddNumberToObject(root, "mode", mode);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "wifi.set") == 0 && payload) {
        cJSON *root = cJSON_Parse(payload);
        if (!root) return 0;
        char sta_ssid[32] = "", sta_password[64] = "";
        char ap_ssid[32] = "", ap_password[64] = "";
        int mode = 0;
        esp_err_t err = get_wifi_settings(sta_ssid, sizeof(sta_ssid), sta_password, sizeof(sta_password),
                                          ap_ssid, sizeof(ap_ssid), ap_password, sizeof(ap_password), &mode);
        if (err == ESP_OK) {
            const char *value = ble_rpc_string(root, "sta_ssid", sta_ssid);
            strlcpy(sta_ssid, value, sizeof(sta_ssid));
            value = ble_rpc_string(root, "ap_ssid", ap_ssid);
            strlcpy(ap_ssid, value, sizeof(ap_ssid));
            value = ble_rpc_string(root, "sta_password", "");
            if (value[0]) strlcpy(sta_password, value, sizeof(sta_password));
            value = ble_rpc_string(root, "ap_password", "");
            if (value[0]) strlcpy(ap_password, value, sizeof(ap_password));
            const cJSON *mode_value = cJSON_GetObjectItemCaseSensitive(root, "mode");
            if (cJSON_IsNumber(mode_value)) mode = mode_value->valueint;
            if (mode < 0 || mode > 1) err = ESP_ERR_INVALID_ARG;
            else err = set_wifi_settings(sta_ssid, sta_password, ap_ssid, ap_password, mode);
        }
        cJSON_Delete(root);
        cJSON *result = cJSON_CreateObject();
        if (!result) return 0;
        cJSON_AddBoolToObject(result, "success", err == ESP_OK);
        if (err != ESP_OK) cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
        return ble_rpc_return_json(result, response, response_size);
    }

    if (strcmp(request_copy, "sleep.get") == 0) {
        int mode = 0, timeout = 30;
        if (get_sleep_settings(&mode, &timeout) != ESP_OK) return 0;
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddNumberToObject(root, "sleep_mode", mode);
        cJSON_AddNumberToObject(root, "idle_timeout_sec", timeout);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "sleep.set") == 0 && payload) {
        cJSON *root = cJSON_Parse(payload);
        if (!root) return 0;
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "sleep_mode");
        const cJSON *timeout = cJSON_GetObjectItemCaseSensitive(root, "idle_timeout_sec");
        esp_err_t err = cJSON_IsNumber(mode) && cJSON_IsNumber(timeout)
                      ? set_sleep_settings(mode->valueint, timeout->valueint) : ESP_ERR_INVALID_ARG;
        cJSON_Delete(root);
        cJSON *result = cJSON_CreateObject();
        if (!result) return 0;
        cJSON_AddBoolToObject(result, "success", err == ESP_OK);
        if (err != ESP_OK) cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
        return ble_rpc_return_json(result, response, response_size);
    }

    if (strcmp(request_copy, "log_level.get") == 0) {
        int length = snprintf(response, response_size, "%d", esp_log_level_get("*"));
        return length > 0 && (size_t)length < response_size ? (size_t)length : 0;
    }
    if (strcmp(request_copy, "log_level.set") == 0 && payload) {
        int level = atoi(payload);
        if (level < ESP_LOG_NONE || level > ESP_LOG_VERBOSE) return 0;
        esp_log_level_set("*", level);
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_u8(handle, "log_level", level);
            nvs_commit(handle);
            nvs_close(handle);
        }
        strlcpy(response, "OK", response_size);
        return strlen(response);
    }

    if (strcmp(request_copy, "verbose.get") == 0) {
        strlcpy(response, g_verbose_logs ? "1" : "0", response_size);
        return strlen(response);
    }
    if (strcmp(request_copy, "verbose.set") == 0 && payload) {
        g_verbose_logs = atoi(payload) != 0;
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_u8(handle, "verbose_logs", g_verbose_logs ? 1 : 0);
            nvs_commit(handle);
            nvs_close(handle);
        }
        strlcpy(response, "OK", response_size);
        return strlen(response);
    }

    if (strcmp(request_copy, "component_logs.get") == 0 ||
        strcmp(request_copy, "component_logs.set") == 0) {
        cJSON *root = strcmp(request_copy, "component_logs.set") == 0 && payload
                    ? cJSON_Parse(payload) : cJSON_CreateObject();
        if (!root) return 0;
        if (strcmp(request_copy, "component_logs.set") == 0 && !ble_rpc_save_component_logs(root)) {
            cJSON_Delete(root);
            return 0;
        }
        cJSON_Delete(root);
        root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON *gobj = cJSON_CreateObject();
        if (gobj) {
            for (int g = 0; g < LOG_GROUP_COUNT; g++) {
                int on = player_logs_group_get(g) ? 1 : 0;
                cJSON_AddNumberToObject(gobj, player_logs_group_key(g), on);
                char gkey[24];
                snprintf(gkey, sizeof(gkey), "group_%s", player_logs_group_key(g));
                cJSON_AddNumberToObject(root, gkey, on);
            }
            cJSON_AddItemToObject(root, "groups", gobj);
        }
        cJSON_AddNumberToObject(root, "player", g_player_logs ? 1 : 0);
        cJSON_AddNumberToObject(root, "wifi_can_sniffer", g_wifi_can_sniffer_logs ? 1 : 0);
        cJSON_AddNumberToObject(root, "web_ota", g_web_ota_logs ? 1 : 0);
        cJSON_AddNumberToObject(root, "uart_ota", g_uart_ota_logs ? 1 : 0);
        cJSON_AddNumberToObject(root, "elm327", g_elm327_logs ? 1 : 0);
        const char *mod_names[] = {"pipeline","can","usb","ble","wifi","sleep","playlist","console","link","nvs"};
        for (int i = 0; i < LOG_MOD_COUNT; i++) {
            cJSON_AddNumberToObject(root, mod_names[i], *player_logs_flag_ptr(i) ? 1 : 0);
        }
        cJSON_AddBoolToObject(root, "success", true);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "ota.begin") == 0 && payload) {
        cJSON *j = cJSON_Parse(payload);
        const cJSON *sz = j ? cJSON_GetObjectItemCaseSensitive(j, "size") : NULL;
        esp_err_t err = cJSON_IsNumber(sz) ? ble_ota_begin((size_t)sz->valueint) : ESP_ERR_INVALID_ARG;
        if (j) cJSON_Delete(j);
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddStringToObject(root, "error", err == ESP_OK ? "" : esp_err_to_name(err));
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "ota.data") == 0 && payload) {
        cJSON *j = cJSON_Parse(payload);
        if (!j) {
            ESP_LOGE("ble_ota", "JSON parse failed, payload len=%u", (unsigned)strlen(payload));
        }
        const cJSON *d = j ? cJSON_GetObjectItemCaseSensitive(j, "data") : NULL;
        const cJSON *o = j ? cJSON_GetObjectItemCaseSensitive(j, "offset") : NULL;
        size_t off = cJSON_IsNumber(o) ? (size_t)o->valueint : 0;
        esp_err_t err = ESP_FAIL;
        if (cJSON_IsString(d)) {
            unsigned char buf[512];
            size_t out_len = 0;
            int r = mbedtls_base64_decode(buf, sizeof(buf), &out_len,
                                          (const unsigned char *)d->valuestring,
                                          strlen(d->valuestring));
            if (r == 0) {
                err = ble_ota_write_at(off, buf, out_len);
            }
        }
        if (j) cJSON_Delete(j);
        if (err != ESP_OK) {
            ESP_LOGW("ble_ota", "chunk reject at off=%u: %s", (unsigned)off, esp_err_to_name(err));
        }
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddNumberToObject(root, "written", (double)ble_ota_written());
        cJSON_AddStringToObject(root, "error", err == ESP_OK ? "" : esp_err_to_name(err));
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "ota.end") == 0) {
        esp_err_t err = ble_ota_end();
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddStringToObject(root, "error", err == ESP_OK ? "" : esp_err_to_name(err));
        size_t n = ble_rpc_return_json(root, response, response_size);
        if (err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        return n;
    }

    if (strcmp(request_copy, "ota.abort") == 0) {
        ble_ota_abort();
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", true);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "ota.status") == 0) {
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "active", ble_ota_active());
        cJSON_AddNumberToObject(root, "written", (double)ble_ota_written());
        cJSON_AddNumberToObject(root, "total", (double)ble_ota_total());
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "playlist.get") == 0) {
        int offset = 0;
        int count = 3;
        if (payload) {
            cJSON *j = cJSON_Parse(payload);
            if (j) {
                const cJSON *o = cJSON_GetObjectItemCaseSensitive(j, "offset");
                const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "count");
                if (cJSON_IsNumber(o)) offset = o->valueint;
                if (cJSON_IsNumber(c)) count = c->valueint;
                cJSON_Delete(j);
            }
        }
        if (offset < 0) offset = 0;
        if (count < 1) count = 1;
        if (count > 4) count = 4;

        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddNumberToObject(root, "total", g_playlist_size);
        cJSON *arr = cJSON_AddArrayToObject(root, "items");
        int budget = (int)response_size - 80;
        if (budget < 64) budget = 64;
        int est = 40;
        int i = offset;
        for (; i < g_playlist_size && i < offset + count; i++) {
            char name[256];
            playlist_get_display_name(i, name, sizeof(name));
            int nl = (int)strlen(name);
            if (est + nl + 6 > budget) {
                if (i == offset && nl > 0) {
                    // Single very long name: truncate on a UTF-8 boundary
                    int maxlen = budget - 46;
                    if (maxlen < 8) maxlen = 8;
                    if (nl > maxlen) {
                        int cut = maxlen;
                        while (cut > 0 && ((unsigned char)name[cut] & 0xC0) == 0x80) cut--;
                        name[cut] = '\0';
                        nl = cut;
                    }
                    cJSON_AddItemToArray(arr, cJSON_CreateString(name));
                    i++;
                }
                break;
            }
            est += nl + 6;
            cJSON_AddItemToArray(arr, cJSON_CreateString(name));
        }
        cJSON_AddNumberToObject(root, "next", i);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "slave_ota.begin") == 0 && payload) {
        cJSON *j = cJSON_Parse(payload);
        const cJSON *sz = j ? cJSON_GetObjectItemCaseSensitive(j, "size") : NULL;
        esp_err_t err = cJSON_IsNumber(sz) ? slave_ota_begin((size_t)sz->valueint) : ESP_ERR_INVALID_ARG;
        if (j) cJSON_Delete(j);
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddStringToObject(root, "error", err == ESP_OK ? "" : esp_err_to_name(err));
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "slave_ota.data") == 0 && payload) {
        cJSON *j = cJSON_Parse(payload);
        const cJSON *d = j ? cJSON_GetObjectItemCaseSensitive(j, "data") : NULL;
        const cJSON *o = j ? cJSON_GetObjectItemCaseSensitive(j, "offset") : NULL;
        size_t off = cJSON_IsNumber(o) ? (size_t)o->valueint : 0;
        esp_err_t err = ESP_FAIL;
        size_t written = 0;
        if (cJSON_IsString(d)) {
            unsigned char buf[512];
            size_t out_len = 0;
            int r = mbedtls_base64_decode(buf, sizeof(buf), &out_len,
                                          (const unsigned char *)d->valuestring,
                                          strlen(d->valuestring));
            if (r == 0) {
                err = slave_ota_write_at(off, buf, out_len);
            }
        }
        if (j) cJSON_Delete(j);
        slave_ota_get_status(NULL, &written, NULL, NULL);
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddNumberToObject(root, "written", (double)written);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "slave_ota.start") == 0) {
        esp_err_t err = slave_ota_start();
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddStringToObject(root, "error", err == ESP_OK ? "" : esp_err_to_name(err));
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "slave_ota.abort") == 0) {
        slave_ota_abort();
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", true);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "slave_ota.status") == 0) {
        int state = 0; size_t written = 0, total = 0, sent = 0;
        slave_ota_get_status(&state, &written, &total, &sent);
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddNumberToObject(root, "state", state);
        cJSON_AddNumberToObject(root, "written", (double)written);
        cJSON_AddNumberToObject(root, "total", (double)total);
        cJSON_AddNumberToObject(root, "sent", (double)sent);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "file.begin") == 0 && payload) {
        cJSON *j = cJSON_Parse(payload);
        const cJSON *sz = j ? cJSON_GetObjectItemCaseSensitive(j, "size") : NULL;
        const cJSON *tg = j ? cJSON_GetObjectItemCaseSensitive(j, "target") : NULL;
        int target = FILE_OTA_TARGET_NONE;
        if (cJSON_IsString(tg)) {
            if (strcmp(tg->valuestring, "master") == 0) target = FILE_OTA_TARGET_MASTER;
            else if (strcmp(tg->valuestring, "slave") == 0) target = FILE_OTA_TARGET_SLAVE;
            else if (strcmp(tg->valuestring, "storage") == 0) target = FILE_OTA_TARGET_STORAGE;
        }
        const cJSON *nm = j ? cJSON_GetObjectItemCaseSensitive(j, "name") : NULL;
        esp_err_t err = ESP_OK;
        if (target == FILE_OTA_TARGET_STORAGE) {
            err = cJSON_IsString(nm) ? file_ota_set_name(nm->valuestring) : ESP_ERR_INVALID_ARG;
        }
        if (err == ESP_OK) {
            err = cJSON_IsNumber(sz) ? file_ota_begin((size_t)sz->valueint, target)
                                     : ESP_ERR_INVALID_ARG;
        }
        if (j) cJSON_Delete(j);
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddStringToObject(root, "error", err == ESP_OK ? "" : esp_err_to_name(err));
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "file.data") == 0 && payload) {
        cJSON *j = cJSON_Parse(payload);
        const cJSON *d = j ? cJSON_GetObjectItemCaseSensitive(j, "data") : NULL;
        const cJSON *o = j ? cJSON_GetObjectItemCaseSensitive(j, "offset") : NULL;
        size_t off = cJSON_IsNumber(o) ? (size_t)o->valueint : 0;
        esp_err_t err = ESP_FAIL;
        size_t written = 0;
        char reason[48] = "";
        if (!j) {
            strlcpy(reason, "request is not valid JSON", sizeof(reason));
            ESP_LOGW("Player", "file.data: invalid JSON (%u bytes): %.24s",
                     (unsigned)strlen(payload), payload);
        } else if (cJSON_IsString(d)) {
            unsigned char buf[512];
            size_t out_len = 0;
            int r = mbedtls_base64_decode(buf, sizeof(buf), &out_len,
                                          (const unsigned char *)d->valuestring,
                                          strlen(d->valuestring));
            if (r == 0 && out_len > 0) {
                err = file_ota_write_at(off, buf, out_len);
                if (err != ESP_OK) {
                    file_ota_get_error(reason, sizeof(reason));
                }
            } else {
                strlcpy(reason, "base64 decode failed", sizeof(reason));
                ESP_LOGW("Player", "file.data b64 decode failed (len=%u, ret=%d, out=%u)",
                         (unsigned)strlen(d->valuestring), r, (unsigned)out_len);
            }
        } else {
            strlcpy(reason, "missing 'data' field", sizeof(reason));
            ESP_LOGW("Player", "file.data missing 'data' field");
        }
        if (reason[0]) {
            file_ota_set_error_msg(reason);
        }
        if (j) cJSON_Delete(j);
        file_ota_get_status(NULL, NULL, &written, NULL, NULL);
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddNumberToObject(root, "written", (double)written);
        cJSON_AddStringToObject(root, "error", reason);
        cJSON_AddNumberToObject(root, "payload_len", (double)strlen(payload));
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "file.finish") == 0) {
        esp_err_t err = file_ota_finish();
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddStringToObject(root, "error", err == ESP_OK ? "" : esp_err_to_name(err));
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "file.flash") == 0 && payload) {
        cJSON *j = cJSON_Parse(payload);
        const cJSON *nm = j ? cJSON_GetObjectItemCaseSensitive(j, "name") : NULL;
        const cJSON *tg = j ? cJSON_GetObjectItemCaseSensitive(j, "target") : NULL;
        int target = FILE_OTA_TARGET_MASTER;
        if (cJSON_IsString(tg) && strcmp(tg->valuestring, "slave") == 0) {
            target = FILE_OTA_TARGET_SLAVE;
        }
        esp_err_t err = cJSON_IsString(nm)
                            ? file_ota_flash_stored(nm->valuestring, target)
                            : ESP_ERR_INVALID_ARG;
        char reason[48] = "";
        if (err != ESP_OK) file_ota_get_error(reason, sizeof(reason));
        if (j) cJSON_Delete(j);
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", err == ESP_OK);
        cJSON_AddStringToObject(root, "error", reason);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "file.abort") == 0) {
        file_ota_abort();
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "success", true);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "file.status") == 0) {
        int state = 0, target = 0;
        size_t written = 0, total = 0, applied = 0;
        file_ota_get_status(&state, &target, &written, &total, &applied);
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddNumberToObject(root, "state", state);
        cJSON_AddNumberToObject(root, "target", target);
        cJSON_AddNumberToObject(root, "written", (double)written);
        cJSON_AddNumberToObject(root, "total", (double)total);
        cJSON_AddNumberToObject(root, "applied", (double)applied);
        char ferr[48] = "";
        file_ota_get_error(ferr, sizeof(ferr));
        cJSON_AddStringToObject(root, "error", ferr);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "system_info.get") == 0) {
        const esp_app_desc_t *app = esp_app_get_description();
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddNumberToObject(root, "free_heap", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
        cJSON_AddNumberToObject(root, "total_heap", heap_caps_get_total_size(MALLOC_CAP_DEFAULT));
        cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(root, "total_psram", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        cJSON_AddNumberToObject(root, "uptime_ms", esp_timer_get_time() / 1000);
        cJSON_AddStringToObject(root, "firmware_version", app ? app->version : "unknown");
        cJSON_AddStringToObject(root, "build_date", __DATE__);
        cJSON_AddStringToObject(root, "build_time", __TIME__);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "version.get") == 0) {
        char player_date[16] = "Unknown", player_time[16] = "Unknown";
        char web_date[16] = "Unknown", web_time[16] = "Unknown";
        char uart_date[16] = "Unknown", uart_time[16] = "Unknown";
        nvs_handle_t handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
            size_t length = sizeof(player_date);
            nvs_get_str(handle, NVS_KEY_PLAYER_DATE, player_date, &length);
            length = sizeof(player_time);
            nvs_get_str(handle, NVS_KEY_PLAYER_TIME, player_time, &length);
            nvs_close(handle);
        }
        if (nvs_open("web_ota", NVS_READONLY, &handle) == ESP_OK) {
            size_t length = sizeof(web_date);
            nvs_get_str(handle, "date", web_date, &length);
            length = sizeof(web_time);
            nvs_get_str(handle, "time", web_time, &length);
            nvs_close(handle);
        }
        if (nvs_open("uart_ota", NVS_READONLY, &handle) == ESP_OK) {
            size_t length = sizeof(uart_date);
            nvs_get_str(handle, "uart_date", uart_date, &length);
            length = sizeof(uart_time);
            nvs_get_str(handle, "uart_time", uart_time, &length);
            nvs_close(handle);
        }
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddStringToObject(root, "player_build_date", player_date);
        cJSON_AddStringToObject(root, "player_build_time", player_time);
        cJSON_AddStringToObject(root, "web_build_date", web_date);
        cJSON_AddStringToObject(root, "web_build_time", web_time);
        cJSON_AddStringToObject(root, "uart_build_date", uart_date);
        cJSON_AddStringToObject(root, "uart_build_time", uart_time);
        char smd[12], smt[9], scd[12], sct[9], sud[12], sut[9];
        slave_version_get(smd, sizeof(smd), smt, sizeof(smt), scd, sizeof(scd), sct, sizeof(sct), sud, sizeof(sud), sut, sizeof(sut));
        char sfw[32] = "unknown";
        slave_fw_version_get(sfw, sizeof(sfw));
        if (sfw[0] == '\0') strlcpy(sfw, "unknown", sizeof(sfw));
        cJSON_AddStringToObject(root, "slave_fw_version", sfw);
        cJSON_AddStringToObject(root, "slave_main_build_date", smd);
        cJSON_AddStringToObject(root, "slave_main_build_time", smt);
        cJSON_AddStringToObject(root, "slave_crc_build_date", scd);
        cJSON_AddStringToObject(root, "slave_crc_build_time", sct);
        cJSON_AddStringToObject(root, "slave_uart_build_date", sud);
        cJSON_AddStringToObject(root, "slave_uart_build_time", sut);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "media.get") == 0) {
        player_media_t media;
        player_media_get(&media);
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "valid", media.valid);
        cJSON_AddBoolToObject(root, "playing", media.playing);
        cJSON_AddNumberToObject(root, "pos_ms", media.pos_ms);
        cJSON_AddNumberToObject(root, "dur_ms", media.dur_ms);
        cJSON_AddStringToObject(root, "title", media.title);
        cJSON_AddStringToObject(root, "artist", media.artist);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "device.restart") == 0) {
        if (xTaskCreate(ble_restart_task, "ble_restart", 2048, NULL, 2, NULL) != pdPASS) {
            strlcpy(response, "restart failed", response_size);
        } else {
            strlcpy(response, "restarting", response_size);
        }
        return strlen(response);
    }

    if (strcmp(request_copy, "can_statistics.get") == 0) {
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddNumberToObject(root, "frames_per_second", g_can_frames_per_second);
        cJSON_AddNumberToObject(root, "bytes_per_second", g_can_bytes_per_second);
        cJSON_AddNumberToObject(root, "total_frames", (double)g_can_total_frames);
        cJSON_AddNumberToObject(root, "total_bytes", (double)g_can_total_bytes);
        cJSON_AddNumberToObject(root, "bitrate", g_can_bitrate);
        return ble_rpc_return_json(root, response, response_size);
    }

    if (strcmp(request_copy, "sniffer.set") == 0 && payload) {
        player_control("set_sniffer", atoi(payload) != 0 ? 1 : 0);
        cJSON *root = cJSON_CreateObject();
        if (!root) return 0;
        cJSON_AddBoolToObject(root, "enabled", g_sniffer_enabled);
        return ble_rpc_return_json(root, response, response_size);
    }

    cJSON *error = cJSON_CreateObject();
    if (!error) return 0;
    cJSON_AddStringToObject(error, "error", "unsupported BLE request");
    return ble_rpc_return_json(error, response, response_size);
}

void ble_command_handler(const char *command, const char *params) {
    if (command == NULL) return;
    
    ESP_LOGI(TAG_BLE, "📱 BLE Command: %s (params: %s)", command, params ? params : "");
    
    app_message_t app_msg = {0};
    
    if (strcasecmp(command, "play") == 0) {
        if (params && strlen(params) > 0) {
            int track_idx = atoi(params);
            app_msg.id = APP_PLAY_TRACK;
            app_msg.data.int_param = track_idx;
            ESP_LOGI(TAG_BLE, "▶️ Play track %d via BLE", track_idx);
        } else {
            app_msg.id = APP_NEXT_TRACK;
        }
    } else if (strcasecmp(command, "pause") == 0) {
        app_msg.id = APP_TOGGLE_PAUSE;
        ESP_LOGI(TAG_BLE, "⏸️ Toggle pause via BLE");
    } else if (strcasecmp(command, "next") == 0) {
        app_msg.id = APP_NEXT_TRACK;
        ESP_LOGI(TAG_BLE, "⏭️ Next track via BLE");
    } else if (strcasecmp(command, "prev") == 0) {
        app_msg.id = APP_PREV_TRACK;
        ESP_LOGI(TAG_BLE, "⏮️ Previous track via BLE");
    } else if (strcasecmp(command, "seek") == 0) {
        if (params && strlen(params) > 0) {
            int pos_ms = atoi(params);
            app_msg.id = APP_SEEK_POSITION;
            app_msg.data.int_param = pos_ms;
            ESP_LOGI(TAG_BLE, "⏩ Seek to %d ms via BLE", pos_ms);
        }
    } else if (strcasecmp(command, "status") == 0) {
        ESP_LOGD(TAG_BLE, "📊 Status requested via BLE");
        return;
    } else if (strcasecmp(command, "switch_mode") == 0) {
        app_msg.id = APP_MODE_SWITCH;
        ESP_LOGI(TAG_BLE, "🔀 Switch mode via BLE");
    }
    
    if (app_msg.id != 0) {
        xQueueSendToBack(app_queue, &app_msg, pdMS_TO_TICKS(100));
    }
}


const char *ble_rpc_string(cJSON *root, const char *name, const char *fallback)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(value) ? value->valuestring : fallback;
}
