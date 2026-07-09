idf_component_register(
    SRCS "dial_time.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_netif nvs_flash
    EMBED_TXTFILES "zones.csv")
