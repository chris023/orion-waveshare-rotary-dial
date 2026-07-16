#pragma once
#include <stdbool.h>

/*
 * Firmware updates from GitHub Releases (M6): the version check hits the
 * public repo's GitHub API (api.github.com), and the image itself is
 * esp_https_ota'd from the release's "orion-dial.bin" asset (a 302 to
 * objects.githubusercontent.com, which esp_https_ota follows natively). TLS
 * on both hosts verifies against dial_oauth_root_ca() -- the same embedded
 * multi-root PEM used for Orion already covers GitHub's chains too.
 *
 * Threading: dial_ota_check/download_and_apply are blocking and worker-task
 * only (same discipline as dial_mcp/dial_oauth). dial_ota_get() is a
 * mutex-guarded snapshot safe to call from any task (mirrors dial_state_get,
 * so the LVGL-side settings screen can read progress without touching the
 * worker directly -- though in practice the worker mirrors this into
 * app_state_t and screens read that instead).
 */

typedef enum {
    OTA_IDLE = 0,      // no check yet, or checked and already current
    OTA_CHECKING,
    OTA_AVAILABLE,     // a newer release is ready to download
    OTA_DOWNLOADING,
    OTA_READY_REBOOT,  // image written + verified; caller must esp_restart()
    OTA_FAILED,
} dial_ota_status_t;

typedef struct {
    dial_ota_status_t status;
    char latest[16];       // "X.Y.Z" from the release tag, once known
    int  progress_pct;     // 0-100, meaningful while OTA_DOWNLOADING
    char err[96];          // last failure, human-readable
} dial_ota_info_t;

// Snapshot the current status under the internal lock. Safe from any task.
void dial_ota_get(dial_ota_info_t *out);

// Blocking: GET the latest GitHub release, compare its tag_name (stripped of
// the "dial-v" prefix) against the running esp_app_get_description()
// version, and -- if newer -- record the "orion-dial.bin" asset's download
// URL for a subsequent dial_ota_download_and_apply(). Leaves status
// OTA_AVAILABLE (newer found), OTA_IDLE (already current, or nothing to
// compare), or OTA_FAILED (network/parse error, see .err). Worker task only.
bool dial_ota_check(void);

// Blocking: esp_https_ota the asset URL captured by the last dial_ota_check
// that found OTA_AVAILABLE. progress_cb (may be NULL) is invoked with 0-100
// on every esp_https_ota_perform() iteration; this component does not rate-
// limit those calls itself -- the caller (the worker, committing to the
// shared app_state_t) decides how often to act on them. On success, sets
// OTA_READY_REBOOT and returns true -- esp_restart() is the CALLER's job,
// this function never reboots. On failure, sets OTA_FAILED + err and
// returns false. Worker task only.
bool dial_ota_download_and_apply(void (*progress_cb)(int pct));

// Records a FAILED status carrying a caller-supplied reason, without making
// any network call -- for a caller that must withhold dial_ota_check() on a
// precondition of its own (system clock not yet valid, notably: mbedTLS
// needs a real wall clock to validate the GitHub/objects.githubusercontent.com
// chains) but still owes the Settings row a clear reason instead of a
// silent no-op. Worker task only (same discipline as dial_ota_check()).
void dial_ota_set_blocked(const char *reason);

// Call once after a healthy boot (the worker's "device linked" moment): if
// the running image is still ESP_OTA_IMG_PENDING_VERIFY (booted straight
// from an OTA install, rollback armed), mark it valid so the bootloader
// stops treating it as provisional. A no-op (just a log line) on a normal,
// non-OTA boot.
void dial_ota_mark_valid_if_pending(void);
