p = 'components/elm327_emu/elm327_emu.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
old = '''    if (s_config.wifi_enabled) {
        xTaskCreatePinnedToCore(elm327_tcp_server_task, "elm327_tcp", 4096, NULL, 5, &s_tcp_server_task, 0);
        ESP_LOGI(TAG, "Wi-Fi TCP server task started on port %d", ELM327_TCP_PORT);
    }

    if (s_config.bt_enabled) {
        elm327_init_bt_spp();
        ESP_LOGI(TAG, "Bluetooth Classic SPP initialized");
    }'''
new = '''    if (s_config.wifi_enabled) {
        xTaskCreatePinnedToCore(elm327_tcp_server_task, "elm327_tcp", 4096, NULL, 5, &s_tcp_server_task, 0);
        ESP_LOGI(TAG, "Wi-Fi TCP server task started on port %d", ELM327_TCP_PORT);
    }
    if (s_config.bt_enabled) {
        ESP_LOGW(TAG, "BT Classic SPP not supported in this build");
    }'''
if old in t:
    t2 = t.replace(old, new)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok')
else:
    print('no')
