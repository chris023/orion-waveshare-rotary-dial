idf_component_register(
    SRCS "dial_mcp.c"
    INCLUDE_DIRS "."
    REQUIRES dial_oauth esp_http_client cjson)
