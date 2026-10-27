p = 'components/elm327_emu/elm327_emu.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
old = '''send_response:
    if (strlen(response) == 0) {
        snprintf(response, sizeof(response), "?\\r\\r>");
    }

    if (is_bt && s_spp_connected && s_spp_handle != 0) {
        esp_spp_write(s_spp_handle, strlen(response), (uint8_t *)response);
    } else if (client_sock >= 0) {
        send(client_sock, response, strlen(response), 0);
    }'''
new = '''send_response:
    if (strlen(response) == 0) {
        snprintf(response, sizeof(response), "?\\r\\r>");
    }

    if (is_bt && s_spp_connected && s_spp_handle != 0) {
        /* BT SPP not supported in this build */
    } else if (client_sock >= 0) {
        send(client_sock, response, strlen(response), 0);
    }'''
if old in t:
    t2 = t.replace(old, new)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok')
else:
    print('no')
