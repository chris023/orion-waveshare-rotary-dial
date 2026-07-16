#pragma once
/*
 * Fake esp_wifi.h for the host simulator build. scr_wifi.c reads exactly two
 * fields off wifi_ap_record_t (.ssid, .rssi) via esp_wifi_sta_get_ap_info() —
 * this is a minimal stand-in, not a port of the real Wi-Fi driver header.
 * The real definition is provided by stubs.c.
 */
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint8_t ssid[33];
    int8_t  rssi;
} wifi_ap_record_t;

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info);
