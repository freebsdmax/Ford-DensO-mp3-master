p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
key = 'httpd_register_uri_handler(s_server, &component_logs_get);'
if key in t and '/elm327_config' not in t:
    add = key + '''
    {
        httpd_uri_t u1 = { .uri = "/elm327_config", .method = HTTP_GET, .handler = elm327_config_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_server, &u1);
    }
    {
        httpd_uri_t u2 = { .uri = "/elm327_config", .method = HTTP_POST, .handler = elm327_config_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_server, &u2);
    }
    {
        httpd_uri_t u3 = { .uri = "/elm327_status", .method = HTTP_GET, .handler = elm327_status_handler, .user_ctx = NULL };
        httpd_register_uri_handler(s_server, &u3);
    }
'''
    open(p,'w',encoding='utf-8').write(t.replace(key, add))
    print('ok')
else:
    print('skip')
