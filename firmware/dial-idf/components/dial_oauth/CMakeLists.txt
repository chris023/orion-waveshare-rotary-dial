idf_component_register(
    SRCS "dial_oauth.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client esp-tls mbedtls cjson nvs_flash
    EMBED_TXTFILES "orion_root_ca.pem")
