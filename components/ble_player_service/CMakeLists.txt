idf_component_register(
    SRCS "ble_player_service.c"
    INCLUDE_DIRS "."
    REQUIRES freertos bt
)

# Explicitly add BT include paths for esp32s3 (uses esp32c3 headers)
target_include_directories(${COMPONENT_LIB} PRIVATE 
    $ENV{IDF_PATH}/components/bt/include
    $ENV{IDF_PATH}/components/bt/include/esp32c3/include
)
