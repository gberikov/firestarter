idf_component_register(SRCS "firestarter.c"
        INCLUDE_DIRS "."
        EMBED_TXTFILES "index.html"
        REQUIRES esp_driver_ledc esp_driver_gpio esp_http_server esp_wifi nvs_flash)
