#pragma once
#include <stdbool.h>
#include <time.h>

/*
 * Wall-clock time for the dial: SNTP sync (esp_netif_sntp) + IANA -> POSIX TZ
 * resolution via an embedded copy of the posix_tz_db zones table, persisted
 * in NVS (ns "time") so the dial keeps showing local time across reboots
 * even before the account's timezone is (re)confirmed.
 */

// Start SNTP (esp_netif_sntp, pool.ntp.org) non-blocking, and restore the
// persisted POSIX TZ from NVS if present. Call once after Wi-Fi is up.
void dial_time_start(void);

// True once SNTP has synced at least once this boot AND a TZ is set.
bool dial_time_valid(void);

// Map an IANA zone name (e.g. "America/Denver") to a POSIX TZ string via the
// embedded table, apply it (setenv TZ + tzset), persist it in NVS (ns "time").
// Returns false if the name is unknown (leaves current TZ).
bool dial_time_set_iana_tz(const char *iana);

// Current local time, or false if not yet valid.
bool dial_time_now(struct tm *out);
