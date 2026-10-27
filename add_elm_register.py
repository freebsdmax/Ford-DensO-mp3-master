# -*- coding: utf-8 -*-
path = 'main/web_ota.c'
t = open(path,'r',encoding='utf-8',errors='ignore').read()
m = 'httpd_register_uri_handler(server, &component_logs_get_uri);'
if m in t and '/elm327_config' not in t:
    add = m + '''

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
    t2 = t.replace(m, add)
    open(path,'w',encoding='utf-8').write(t2)
    print('ok')
else:
    print('skip')
