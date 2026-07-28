#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Firmware updates from GitHub Releases (M6): the version check hits the
 * public repo's GitHub API (api.github.com), and the image itself is
 * esp_https_ota'd from the release's "orion-dial.bin" asset (a 302 to
 * objects.githubusercontent.com, which esp_https_ota follows natively). TLS
 * on both hosts verifies against dial_oauth_root_ca() -- the same embedded
 * multi-root PEM used for Orion already covers GitHub's chains too.
 *
 * Beta channel: dial_ota_check(beta) takes the caller's current
 * dial_state.beta preference (SCR_UPDATE's "Beta builds" toggle). Off, this
 * is exactly today's behavior. On, it queries the releases LIST endpoint
 * instead of /releases/latest (which by definition excludes prereleases)
 * and picks the newest by version among the entries it inspects, tags like
 * "dial-v1.1.0-beta.1" included -- see is_newer()'s semver §11 prerelease
 * tiebreak in the .c file.
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
    // True from dial_ota_init() (captured once at boot) until
    // dial_ota_mark_valid_if_pending() actually confirms the image -- i.e.
    // this boot is a fresh OTA install the bootloader will silently roll
    // back on the next reset/power-cycle unless it survives to confirm. The
    // worker mirrors this into app_state_t so scr_connecting/scr_dial can
    // warn the user not to unplug during that window (see main.c's field
    // incident writeup near ota_confirm_once()).
    bool pending_verify;
} dial_ota_info_t;

// Snapshot the current status under the internal lock. Safe from any task.
void dial_ota_get(dial_ota_info_t *out);

// Blocking: GET the latest GitHub release (beta == false, today's
// /releases/latest -- prereleases excluded by GitHub itself) or the newest
// release INCLUDING prereleases (beta == true, the /releases list endpoint,
// scanned defensively -- see dial_ota.c), compare its tag_name (stripped of
// the "dial-v" prefix) against the running esp_app_get_description()
// version -- prerelease-aware, semver §11 -- and -- if newer -- record the
// "orion-dial.bin" asset's download URL for a subsequent
// dial_ota_download_and_apply(). Leaves status OTA_AVAILABLE (newer found),
// OTA_IDLE (already current, or nothing to compare), or OTA_FAILED
// (network/parse error, see .err). Worker task only. `beta` is the caller's
// current dial_state_t.beta snapshot -- this component doesn't depend on
// dial_state, so the caller decides the channel on every call.
bool dial_ota_check(bool beta);

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

// OTA_FAILED must not be a terminal state -- a stale check/download failure
// (or a blocked-check reason) must not wedge the Settings row, or any other
// reader of dial_ota_get()/app_state_t.ota, forever (field bug: it used to
// take a manual power cycle to clear). No-op unless status is currently
// OTA_FAILED AND at least max_age_us has elapsed since it became so; when it
// does apply, reverts to OTA_IDLE (clearing .err) and returns true. Two
// callers, both worker task only (same discipline as the rest of this
// file's mutators -- the caller is responsible for re-committing the
// app_state_t mirror, e.g. via main.c's commit_ota_snapshot(), when this
// returns true):
//  - main.c's idle loop, periodically, with a real max_age_us -- the
//    time-based "don't stay wedged for minutes" rule.
//  - main.c's CMD_OTA_CLEAR_FAILED handler, with max_age_us=0 -- fired from
//    scr_settings.c's screen teardown, so a fresh visit to Settings never
//    shows a failure left over from a prior visit.
bool dial_ota_clear_stale_failure(int64_t max_age_us);

// Call once after a healthy boot (the worker's "device linked" moment, or a
// 30s stable-boot fallback timer -- see main.c): if the running image is
// still ESP_OTA_IMG_PENDING_VERIFY (booted straight from an OTA install,
// rollback armed), mark it valid so the bootloader stops treating it as
// provisional, and clears info.pending_verify. A no-op (just a log line) on
// a normal, non-OTA boot.
void dial_ota_mark_valid_if_pending(void);

// Call once, very early in app_main (before Wi-Fi/UI first paint): captures
// whether this boot is currently ESP_OTA_IMG_PENDING_VERIFY into
// info.pending_verify, WITHOUT marking anything valid. This exists
// separately from dial_ota_mark_valid_if_pending() because the UI needs to
// know "you're on probation" from the very first rendered frame -- long
// before the worker reaches a healthy poll or the 30s confirm timer fires --
// so the pending-verify warning can actually be seen before it's resolved.
void dial_ota_init(void);
