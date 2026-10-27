idf_component_register(
    SRCS "elm327_emu.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash driver esp_wifi lwip bt esp_event freertos
)
