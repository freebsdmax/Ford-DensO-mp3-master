
set(COMPONENT_SRCS  "play_music-master.c" "web_ota.c" "uart_ota_master.c")
set(COMPONENT_ADD_INCLUDEDIRS .)

register_component(
    REQUIRES usb_host msc_host esp_vfs_fat wifi_can_sniffer esp_http_server esp_wifi esp_netif esp_ota
)
