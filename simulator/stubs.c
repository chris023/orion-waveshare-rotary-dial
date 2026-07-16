/*
 * stubs.c — every non-dial_state external dependency the compiled dial_ui
 * screens call, per the scout report's per-file symbol list. All of it is
 * either a hardware driver (haptics, power, display rotation) with nothing
 * to actually do on a host, or Wi-Fi/OTA/app-descriptor state the simulator
 * fakes with fixed, plausible values instead of running the real drivers.
 *
 * Canned network scan list and IP per the build spec:
 *   "Home" (-48), "Home-Guest" (-55), "Bluebird Cottage" (-67), "Attic AP" (-79)
 *   dial_net_ip -> "192.168.1.23"
 */
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "dial_haptics.h"
#include "dial_power.h"
#include "dial_display.h"
#include "dial_wifi.h"
#include "esp_wifi.h"
#include "esp_app_desc.h"

/* ---- dial_haptics ------------------------------------------------------ */

void dial_haptics_play(haptic_effect_t fx) { (void)fx; }
void dial_haptics_set_enabled(bool enabled) { (void)enabled; }
void dial_haptics_set_night(bool night) { (void)night; }
void dial_haptics_init(void) {}

/* ---- dial_power ---------------------------------------------------------
 * Always ACTIVE: the simulator only ever renders one screen at a time, on
 * demand — there is no idle clock counting down toward DIMMED/STANDBY. */

dial_power_level_t dial_power_level(void) { return DPWR_ACTIVE; }
bool dial_power_wake_consumes(void) { return false; }
void dial_power_start(void) {}
void dial_power_set_night(bool night) { (void)night; }

/* ---- dial_display --------------------------------------------------------
 * Rotation always "succeeds" — the real function only fails when the 90/270
 * DMA scratch buffer can't be allocated, a panel-driver detail the host build
 * has no equivalent of. */

bool dial_display_set_rotation(uint8_t quarters) { (void)quarters; return true; }
uint8_t dial_display_rotation(void) { return 0; }
void dial_display_start(void) {}
bool dial_display_lock(int timeout_ms) { (void)timeout_ms; return true; }
void dial_display_unlock(void) {}
void dial_display_set_touch_filter(dial_display_touch_filter_t filter) { (void)filter; }

/* ---- dial_net / Wi-Fi ----------------------------------------------------
 * A fixed 4-network scan result and a fixed "connected to Home" status —
 * enough for scr_netpick/scr_passkey/scr_wifi to render real content without
 * a Wi-Fi driver underneath. */

typedef struct { const char *ssid; int8_t rssi; } fake_ap_t;
static const fake_ap_t FAKE_SCAN[] = {
    { "Home",             -48 },
    { "Home-Guest",       -55 },
    { "Bluebird Cottage", -67 },
    { "Attic AP",         -79 },
};
#define FAKE_SCAN_COUNT (int)(sizeof(FAKE_SCAN) / sizeof(FAKE_SCAN[0]))

int dial_net_scan_count(void) { return FAKE_SCAN_COUNT; }

const char *dial_net_scan_ssid(int i)
{
    if (i < 0 || i >= FAKE_SCAN_COUNT) return "";
    return FAKE_SCAN[i].ssid;
}

void dial_net_scan_request(void) { /* result is static; nothing to kick off */ }
void dial_net_submit_creds(const char *ssid, const char *pass) { (void)ssid; (void)pass; }

bool dial_wifi_is_connected(void) { return true; }

bool dial_net_ip(char *out, size_t sz)
{
    if (!out || sz == 0) return false;
    snprintf(out, sz, "192.168.1.23");
    return true;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap_info)
{
    if (!ap_info) return ESP_FAIL;
    memset(ap_info, 0, sizeof(*ap_info));
    strncpy((char *)ap_info->ssid, FAKE_SCAN[0].ssid, sizeof(ap_info->ssid) - 1);
    ap_info->rssi = FAKE_SCAN[0].rssi;
    return ESP_OK;
}

/* Unused by dial_ui (no screen calls dial_net_init/bringup/etc.), but part of
 * dial_wifi.h's surface; provided so any incidental reference still links. */
void dial_net_init(void) {}
void dial_net_seed(const char *ssid, const char *pass) { (void)ssid; (void)pass; }
bool dial_net_have_creds(void) { return true; }
void dial_net_forget(void) {}
void dial_net_request_setup(void) {}
bool dial_net_setup_requested(void) { return false; }
void dial_net_bringup(void) {}
const char *dial_net_ap_ssid(void) { return "OrionDial-A1B2"; }
void dial_net_on_event(dial_net_event_cb_t cb) { (void)cb; }

/* ---- esp_app_desc --------------------------------------------------------
 * Fixed "v1.0.1 / v6.0" identity for scr_about.c's Firmware/IDF rows —
 * kept in step with firmware/dial-idf/CMakeLists.txt's PROJECT_VER so the
 * simulator's about.png never shows a version the real firmware doesn't. */

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t desc = { .version = "1.0.1", .idf_ver = "v6.0" };
    return &desc;
}
