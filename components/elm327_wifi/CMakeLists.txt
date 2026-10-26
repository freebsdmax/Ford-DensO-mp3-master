idf_component_register(SRCS "elm327_wifi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_wifi lwip driver nvs_flash)