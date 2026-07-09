idf_component_register(
    SRCS "dial_ota.c"
    INCLUDE_DIRS "."
    REQUIRES esp_https_ota app_update esp_http_client esp_app_format cjson dial_oauth)
