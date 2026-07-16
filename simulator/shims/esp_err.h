#pragma once
/* Fake esp_err.h for the host simulator build — just enough for the one
 * comparison dial_ui code makes (scr_wifi.c: esp_wifi_sta_get_ap_info() == ESP_OK). */

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1
