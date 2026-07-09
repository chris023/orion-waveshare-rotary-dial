#include "dial_time.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif_sntp.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "dial_time";

#define NVS_NS       "time"
#define NVS_KEY_TZ   "posix_tz"

// IANA -> POSIX TZ table, from https://github.com/nayarsystems/posix_tz_db
// (CSV rows: "IANA/Name","POSIXRuleString"). EMBED_TXTFILES null-terminates
// the blob, so it's safe to treat as an ordinary C string.
extern const char zones_csv_start[] asm("_binary_zones_csv_start");

// Written once each: s_synced by the SNTP sync callback, s_tz_set by whichever
// of restore-from-NVS / dial_time_set_iana_tz runs first. Plain bool reads/
// writes are fine here — no ordering requirement beyond "eventually visible".
static volatile bool s_synced = false;
static volatile bool s_tz_set = false;

static void apply_posix_tz(const char *posix)
{
    setenv("TZ", posix, 1);
    tzset();
    s_tz_set = true;
}

static void persist_posix_tz(const char *posix)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_TZ, posix);
    nvs_commit(h);
    nvs_close(h);
}

// Restore a previously-resolved POSIX TZ from NVS, if any, so the dial shows
// sensible local time immediately on boot (before SNTP + any fresh timezone
// lookup complete).
static void restore_posix_tz(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    char buf[64];
    size_t sz = sizeof(buf);
    if (nvs_get_str(h, NVS_KEY_TZ, buf, &sz) == ESP_OK) {
        apply_posix_tz(buf);
        ESP_LOGI(TAG, "restored TZ from NVS: %s", buf);
    }
    nvs_close(h);
}

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    ESP_LOGI(TAG, "SNTP time synced");
}

void dial_time_start(void)
{
    restore_posix_tz();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.start          = true;
    config.wait_for_sync  = false;   // non-blocking; poll dial_time_valid() instead
    config.sync_cb        = sntp_sync_cb;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "SNTP started (pool.ntp.org)");
}

bool dial_time_valid(void)
{
    return s_synced && s_tz_set;
}

bool dial_time_set_iana_tz(const char *iana)
{
    if (!iana || !iana[0]) return false;

    // Match a full CSV field: "<iana>","  -- the trailing `","` guarantees we
    // don't stop on a zone name that's merely a prefix of another one.
    char needle[80];
    int n = snprintf(needle, sizeof(needle), "\"%s\",\"", iana);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return false;

    const char *hit = strstr(zones_csv_start, needle);
    if (!hit) {
        ESP_LOGW(TAG, "unknown IANA zone: %s", iana);
        return false;
    }

    const char *val_start = hit + strlen(needle);
    const char *val_end   = strchr(val_start, '"');
    if (!val_end) return false;
    size_t vlen = (size_t)(val_end - val_start);
    if (vlen == 0 || vlen >= 64) return false;

    char posix[64];
    memcpy(posix, val_start, vlen);
    posix[vlen] = '\0';

    apply_posix_tz(posix);
    persist_posix_tz(posix);
    ESP_LOGI(TAG, "TZ %s -> %s", iana, posix);
    return true;
}

bool dial_time_now(struct tm *out)
{
    if (!out || !dial_time_valid()) return false;
    time_t now = time(NULL);
    localtime_r(&now, out);
    return true;
}
