p = 'components/elm327_emu/elm327_emu.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
old = '''    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (param->auth_cmpl.success) {
                ESP_LOGI(TAG, "BT authentication success");
            } else {
                ESP_LOGE(TAG, "BT authentication failed");
            }
            break;
        default:
            break;
    }'''
new = '''    switch (event) {
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            ESP_LOGI(TAG, "BT authentication completed");
            break;
        default:
            break;
    }'''
if old in t:
    t2 = t.replace(old, new)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok')
else:
    print('no')
