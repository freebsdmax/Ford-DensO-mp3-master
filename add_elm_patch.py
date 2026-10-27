# -*- coding: utf-8 -*-
import os
path = 'main/web_ota.c'
with open(path,'r',encoding='utf-8',errors='ignore') as f:
    s = f.read()

# add handlers
if 'elm327_config_get_handler' not in s:
    marker = 'static esp_err_t component_logs_post_handler'
    idx = s.find(marker)
    if idx >= 0:
        end = s.find('\n}\n', idx)
        if end >= 0:
            end = end + 3
            add = '''

static esp_err_t elm327_config_get_handler(httpd_req_t *req)
{
    elm327_config_t cfg;
    if (elm327_emu_get_config(&cfg) != ESP_OK) {
        elm327_emu_load_config(&cfg);
    }
    char resp[128];
    sprintf(resp, "{\\"enabled\\":%d,\\"wifi_enabled\\":%d,\\"bt_enabled\\":%d}", cfg.enabled?1:0, cfg.wifi_enabled?1:0, cfg.bt_enabled?1:0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t elm327_config_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret, remaining = req->content_len;
    if (remaining > 255) remaining = 255;
    if (remaining < 0) remaining = 0;
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty");
        return ESP_FAIL;
    }
    buf[ret] = 0;
    elm327_config_t cfg = {0};
    if (strstr(buf, "\\"enabled\\":1") || strstr(buf, "\\"enabled\\":true")) cfg.enabled = true;
    if (strstr(buf, "\\"wifi_enabled\\":1") || strstr(buf, "\\"wifi_enabled\\":true")) cfg.wifi_enabled = true;
    if (strstr(buf, "\\"bt_enabled\\":1") || strstr(buf, "\\"bt_enabled\\":true")) cfg.bt_enabled = true;
    if (elm327_emu_set_config(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "err");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\\"status\\":\\"ok\\"}", 15);
    return ESP_OK;
}

static esp_err_t elm327_status_handler(httpd_req_t *req)
{
    elm327_config_t cfg;
    elm327_emu_get_config(&cfg);
    char resp[160];
    sprintf(resp, "{\\"enabled\\":%d,\\"wifi_enabled\\":%d,\\"bt_enabled\\":%d,\\"running\\":%d}", cfg.enabled?1:0, cfg.wifi_enabled?1:0, cfg.bt_enabled?1:0, (cfg.enabled && (cfg.wifi_enabled || cfg.bt_enabled)) ? 1 : 0);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}
'''
            s = s[:end] + add + s[end:]
            print('handlers')

# routes
if '"/elm327_config"' not in s:
    reg = '''    httpd_uri_t elm327_config_get_uri = {
        .uri = "/elm327_config",
        .method = HTTP_GET,
        .handler = elm327_config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &elm327_config_get_uri);

    httpd_uri_t elm327_config_post_uri = {
        .uri = "/elm327_config",
        .method = HTTP_POST,
        .handler = elm327_config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &elm327_config_post_uri);

    httpd_uri_t elm327_status_uri = {
        .uri = "/elm327_status",
        .method = HTTP_GET,
        .handler = elm327_status_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &elm327_status_uri);
'''
    m2 = 'component_logs_get_uri'
    idx2 = s.find(m2)
    if idx2 >= 0:
        prev = s.rfind('httpd_uri_t', 0, idx2)
        if prev >= 0:
            s = s[:prev] + reg + '\n' + s[prev:]
            print('routes')

with open(path,'w',encoding='utf-8') as f:
    f.write(s)
print('done')
