idf_component_register(
    SRCS "wifi_can_sniffer.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi lwip driver
)
