p = 'components/elm327_emu/elm327_emu.c'
t = open(p,'r',encoding='utf-8',errors='ignore').read()
# replace bt init with stub
old = '''static esp_err_t elm327_init_bt_spp(void)
{

    if (!s_config.bt_enabled) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing Bluetooth Classic SPP for ELM327 emulation");

    // BT already initialized in ble_player_service? Check if BT controller is enabled
    esp_err_t ret = esp_spp_register_callback(spp_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPP register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_spp_init(ESP_SPP_MODE_CB);

    esp_bt_gap_register_callback(bt_gap_callback);
    esp_bt_gap_set_device_name("OBDLink-EX");
    
    // Make device discoverable and connectable
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    ret = esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, "SPP_SERVER");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPP start server failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Bluetooth Classic SPP initialized, device name: OBDLink-EX");
    return ESP_OK;
}'''
new = '''static esp_err_t elm327_init_bt_spp(void)
{
    if (!s_config.bt_enabled) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Bluetooth Classic SPP disabled in this build");
    return ESP_OK;
}'''
if old in t:
    t2 = t.replace(old, new)
    open(p,'w',encoding='utf-8').write(t2)
    print('ok')
else:
    print('no')
