#include "player_auth.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "Auth";

static char s_token[33] = "";

void player_auth_load(void)
{
    nvs_handle_t h;
    if (nvs_open("audio_player", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_token);
        if (nvs_get_str(h, "auth_token", s_token, &len) != ESP_OK) {
            s_token[0] = '\0';
        }
        nvs_close(h);
    }
    ESP_LOGI(TAG, "HTTP auth: %s", s_token[0] ? "token required" : "open");
}

const char *player_auth_token(void)
{
    return s_token;
}

bool player_auth_required(void)
{
    return s_token[0] != '\0';
}

esp_err_t player_auth_set(const char *token)
{
    if (!token) token = "";

    size_t len = strlen(token);
    if (len > 32) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t h;
    esp_err_t err = nvs_open("audio_player", NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    err = nvs_set_str(h, "auth_token", token);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        strlcpy(s_token, token, sizeof(s_token));
        ESP_LOGI(TAG, "HTTP auth %s", s_token[0] ? "enabled" : "disabled");
    }
    return err;
}

bool player_auth_request_ok(httpd_req_t *req)
{
    if (!req) return false;
    if (!player_auth_required()) return true;

    /* 1) заголовок X-Auth-Token */
    char hdr[40];
    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", hdr, sizeof(hdr)) == ESP_OK) {
        if (strcmp(hdr, s_token) == 0) return true;
    }

    /* 2) ?token=… у query string */
    char query[160];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[40];
        if (httpd_query_key_value(query, "token", val, sizeof(val)) == ESP_OK) {
            if (strcmp(val, s_token) == 0) return true;
        }
    }

    return false;
}
