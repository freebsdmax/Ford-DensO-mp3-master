p='main/web_ota.c'
t=open(p,'r',encoding='utf-8',errors='ignore').read()

# Add handlers if not exist
if 'elm327_config_get_handler' not in t:
    handlers = '''
static esp_err_t elm327_config_get_handler(httpd_req_t *req)
{
    char resp[256];
    bool enabled = false, wifi_en = false, bt_en = false;
    // Try to get from elm327_emu
    extern bool elm327_is_enabled(void);
    extern bool elm327_is_wifi_enabled(void);
    extern bool elm327_is_bt_enabled(void);
    snprintf(resp, sizeof(resp), "{\\"enabled\\":%s,\\"wifi_enabled\\":%s,\\"bt_enabled\\":%s}", 
             elm327_is_enabled() ? "true" : "false",
             elm327_is_wifi_enabled() ? "true" : "false",
             elm327_is_bt_enabled() ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t elm327_config_post_handler(httpd_req_t *req)
{
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf)-1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad");
        return ESP_FAIL;
    }
    buf[ret] = 0;
    
    bool enabled = strstr(buf, "\\"enabled\\":true") != NULL;
    bool wifi_en = strstr(buf, "\\"wifi_enabled\\":true") != NULL;
    bool bt_en = strstr(buf, "\\"bt_enabled\\":true") != NULL;
    
    extern void elm327_set_config(bool enabled, bool wifi_en, bool bt_en);
    elm327_set_config(enabled, wifi_en, bt_en);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\\"ok\\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t elm327_status_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "{\\"ok\\":true}", HTTPD_RESP_USE_STRLEN);
}
'''
    # Insert before web_ota_start
    marker = 'void web_ota_start'
    if marker in t:
        idx = t.find(marker)
        t2 = t[:idx] + handlers + '\n' + t[idx:]
        print('added')
        t = t2
    else:
        print('no_marker')
else:
    print('exists')
    
# Add routes
if '"/elm327_config"' not in t:
    routes = '''
    httpd_uri_t elm327_config_get_uri = {
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
    # Add before return in web_ota_start
    marker = 'ESP_LOGI(TAG, "Web server started");'
    if marker in t:
        t2 = t.replace(marker, routes + '\n    ' + marker)
        print('routes')
        t = t2
    else:
        print('no_routes')
        
open(p,'w',encoding='utf-8').write(t)
