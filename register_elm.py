p = 'main/web_ota.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
if '/elm327_config' not in t:
    key = 'httpd_register_uri_handler(server, &component_logs_get_uri);'
    if key in t:
        add = key + '''
    {
        httpd_uri_t u = { .uri = "/elm327_config", .method = HTTP_GET, .handler = elm327_config_get_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &u);
    }
    {
        httpd_uri_t u = { .uri = "/elm327_config", .method = HTTP_POST, .handler = elm327_config_post_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &u);
    }
    {
        httpd_uri_t u = { .uri = "/elm327_status", .method = HTTP_GET, .handler = elm327_status_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &u);
    }
'''
        open(p,'w',encoding='utf-8').write(t.replace(key, add))
        print('ok')
    else:
        print('nok')
else:
    print('has')
