
# Generate git version header
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE PLAYER_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT PLAYER_GIT_HASH)
    set(PLAYER_GIT_HASH "dev")
endif()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/player_git_version.h "#pragma once\n#define PLAYER_GIT_HASH \"${PLAYER_GIT_HASH}\"\n")

set(COMPONENT_SRCS  "player_master.c" "player_nvs.c" "player_playlist.c" "player_pipeline.c" "player_console.c" "player_sleep.c" "player_usb.c" "player_can.c" "player_ble.c" "player_wifi.c" "player_link.c" "player_logs.c" "player_slave.c" "player_slave_ota.c" "player_media.c" "player_file_ota.c" "player_ble_ota.c" "web_ota.c" "uart_ota_master.c")
set(COMPONENT_ADD_INCLUDEDIRS . ${CMAKE_CURRENT_BINARY_DIR})

register_component(
    REQUIRES usb_host msc_host esp_vfs_fat wifi_can_sniffer esp_http_server esp_wifi esp_netif esp_ota ble_player_service bt json elm327_emu
)
