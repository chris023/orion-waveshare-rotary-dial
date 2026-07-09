idf_component_register(
    SRCS "dial_oauth.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client esp_http_server esp-tls mbedtls cjson nvs_flash esp_timer
    EMBED_TXTFILES "orion_root_ca.pem")
