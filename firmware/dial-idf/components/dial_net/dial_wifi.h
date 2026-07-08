#pragma once
#include <stdbool.h>

/*
 * Wi-Fi station bring-up for the Orion dial. Initializes NVS + netif + the STA,
 * connects with auto-retry, and blocks until an IP is acquired (or timeout).
 */

// Connect to Wi-Fi. Returns true once an IP is obtained, false on timeout.
bool dial_wifi_start(const char *ssid, const char *password, int timeout_ms);

// True while the station currently has an IP lease.
bool dial_wifi_is_connected(void);
