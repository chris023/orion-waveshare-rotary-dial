idf_component_register(
    SRCS "dial_wifi.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_netif esp_event nvs_flash esp_http_server lwip esp_timer)
