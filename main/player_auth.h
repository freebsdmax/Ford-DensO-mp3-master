#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/**
 * Токен доступу до HTTP API (веб-інтерфейс, OTA, перезавантаження).
 *
 * Порожній токен = доступ відкритий (як було раніше).
 * Непорожній = кожен запит до захищених обробників мусить мати
 * `?token=…` або заголовок `X-Auth-Token`.
 */

void player_auth_load(void);
const char *player_auth_token(void);
bool player_auth_required(void);

/** Порожній рядок вимикає захист. Максимум 32 символи. */
esp_err_t player_auth_set(const char *token);

/** true, якщо запит авторизований (або токен не заданий). */
bool player_auth_request_ok(httpd_req_t *req);
