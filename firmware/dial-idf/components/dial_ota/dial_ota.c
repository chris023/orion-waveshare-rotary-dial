/*
 * OTA updates from GitHub Releases (M6). See dial_ota.h for the threading
 * contract and the overall flow.
 */
#include "dial_ota.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "dial_oauth.h"   // dial_oauth_root_ca() -- covers GitHub's chains too

static const char *TAG = "ota";

#define GITHUB_API_URL \
    "https://api.github.com/repos/chris023/orion-waveshare-rotary-dial/releases/latest"
// The LIST endpoint (beta channel only) -- unlike /releases/latest, this
// includes prereleases. Same host/owner/repo, no trailing "/latest".
#define GITHUB_API_URL_LIST \
    "https://api.github.com/repos/chris023/orion-waveshare-rotary-dial/releases"
#define ASSET_NAME     "orion-dial.bin"
#define TAG_PREFIX     "dial-v"
#define CHECK_BUF_CAP  (64 * 1024)   // release JSON is normally ~10-30KB
// Beta channel only: how many of the list endpoint's (newest-first) entries
// to inspect before giving up on finding a usable release. Bounds both the
// JSON walk and the worst case where the newest few entries are all drafts
// or malformed -- this device has no business scanning its whole release
// history.
#define RELEASES_LIST_SCAN_CAP 10

// Guards s_info and s_asset_url. A short spinlock (never held across a
// blocking call), matching dial_state.c's s_input_mux idiom for cross-task
// state that's only ever read/written as a quick copy.
static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static dial_ota_info_t    s_info;
static char               s_asset_url[300];   // captured by dial_ota_check()
// esp_timer_get_time() at the moment s_info.status last became OTA_FAILED --
// dial_ota_clear_stale_failure()'s clock. Only meaningful while status ==
// OTA_FAILED; never read otherwise.
static int64_t             s_failed_at_us;

void dial_ota_get(dial_ota_info_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_info;
    taskEXIT_CRITICAL(&s_mux);
}

static void set_status(dial_ota_status_t status, const char *latest, const char *err)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_mux);
    s_info.status = status;
    if (status == OTA_FAILED) s_failed_at_us = now;
    if (latest) strlcpy(s_info.latest, latest, sizeof(s_info.latest));
    if (err)    strlcpy(s_info.err, err, sizeof(s_info.err));
    else        s_info.err[0] = 0;
    taskEXIT_CRITICAL(&s_mux);
}

bool dial_ota_clear_stale_failure(int64_t max_age_us)
{
    int64_t now = esp_timer_get_time();
    bool cleared = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_info.status == OTA_FAILED && (now - s_failed_at_us) >= max_age_us) {
        s_info.status = OTA_IDLE;
        s_info.err[0] = 0;
        cleared = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return cleared;
}

static void set_progress(int pct)
{
    taskENTER_CRITICAL(&s_mux);
    s_info.progress_pct = pct;
    taskEXIT_CRITICAL(&s_mux);
}

// Parses an optional "-beta.N" suffix trailing a major.minor.patch core (the
// only prerelease form this firmware ships). *has_suffix reports whether one
// was found at all; *n is its numeral (0 if absent, or if present but
// unparseable -- a malformed "-beta" with no digits still counts as SOME
// prerelease for ordering purposes rather than silently acting like a full
// release, which would let a busted tag skip the channel altogether).
static void parse_beta_suffix(const char *ver, bool *has_suffix, int *n)
{
    const char *p = strstr(ver, "-beta.");
    *has_suffix = (p != NULL);
    *n = 0;
    if (p) sscanf(p + 6, "%d", n);
}

// Semver-ish compare, prerelease-aware (semver §11): split the major.minor.
// patch core on dots, numeric compare. Missing trailing components default
// to 0 ("1.2" == "1.2.0"); %d naturally stops at the first non-digit, so a
// "-beta.N" suffix on either string doesn't perturb the core parse (this is
// what lets `current` -- PROJECT_VER, which for a beta build is literally
// "1.1.0-beta.1" -- flow through the same sscanf as `latest` with no special
// casing). Any core component that fails to parse as a number for EITHER
// string is treated as "not newer" -- an unexpected tag format must never
// trigger a spurious update.
//
// Equal cores fall through to the prerelease tiebreak: a build with no
// "-beta.N" suffix outranks one that has it (a stable release beats any
// prerelease of the same core version -- this is what lets a device parked
// on a beta graduate onto the matching stable release automatically, and,
// the other direction, is why a device already ON stable X.Y.Z never
// "upgrades" to a beta of that same X.Y.Z even with the beta toggle on).
// Between two betas of the same core, higher N wins.
static bool is_newer(const char *latest, const char *current)
{
    int am = 0, an = 0, ap = 0, bm = 0, bn = 0, bp = 0;
    if (sscanf(latest, "%d.%d.%d", &am, &an, &ap) < 1)  return false;
    if (sscanf(current, "%d.%d.%d", &bm, &bn, &bp) < 1) return false;
    if (am != bm) return am > bm;
    if (an != bn) return an > bn;
    if (ap != bp) return ap > bp;

    bool a_beta, b_beta;
    int  a_n, b_n;
    parse_beta_suffix(latest, &a_beta, &a_n);
    parse_beta_suffix(current, &b_beta, &b_n);
    if (a_beta != b_beta) return !a_beta;   // one's a prerelease, the other isn't
    if (!a_beta) return false;              // both plain, equal core -> not newer
    return a_n > b_n;
}

/* ---- version-check HTTP GET -------------------------------------------- */

typedef struct { char *buf; int len; int cap; bool overflow; } check_resp_t;

static esp_err_t on_check_http(esp_http_client_event_t *e)
{
    if (e->event_id != HTTP_EVENT_ON_DATA || e->data_len <= 0) return ESP_OK;
    check_resp_t *r = e->user_data;
    if (r->overflow) return ESP_OK;

    int newlen = r->len + e->data_len;
    if (newlen + 1 > CHECK_BUF_CAP) { r->overflow = true; return ESP_OK; }
    if (newlen + 1 > r->cap) {
        int newcap = newlen * 2 + 512;
        if (newcap > CHECK_BUF_CAP) newcap = CHECK_BUF_CAP;
        char *nb = realloc(r->buf, newcap);
        if (!nb) { r->overflow = true; return ESP_OK; }
        r->buf = nb;
        r->cap = newcap;
    }
    memcpy(r->buf + r->len, e->data, e->data_len);
    r->len = newlen;
    r->buf[r->len] = 0;
    return ESP_OK;
}

// Extracts + TAG_PREFIX-strips a release object's version tag into `out`
// (sized like dial_ota_info_t.latest). False (leaving *out untouched) if the
// object has no usable tag_name -- callers treat that as "skip this entry"
// (list/beta mode) or "malformed release" (single/stable mode).
static bool release_version(cJSON *rel, char *out, size_t out_sz)
{
    cJSON *tag = cJSON_GetObjectItem(rel, "tag_name");
    if (!cJSON_IsString(tag) || !tag->valuestring) return false;
    const char *ver = tag->valuestring;
    if (!strncmp(ver, TAG_PREFIX, strlen(TAG_PREFIX))) ver += strlen(TAG_PREFIX);
    strlcpy(out, ver, out_sz);
    return true;
}

bool dial_ota_check(bool beta)
{
    set_status(OTA_CHECKING, NULL, NULL);

    const esp_app_desc_t *desc = esp_app_get_description();
    char user_agent[40];
    snprintf(user_agent, sizeof(user_agent), "orion-dial/%s", desc->version);

    check_resp_t r = { 0 };
    esp_http_client_config_t cfg = {
        .url           = beta ? GITHUB_API_URL_LIST : GITHUB_API_URL,
        .event_handler = on_check_http,
        .user_data     = &r,
        .cert_pem      = dial_oauth_root_ca(),
        .user_agent    = user_agent,   // required by the GitHub API
        .timeout_ms    = 15000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    esp_err_t err = esp_http_client_perform(c);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);

    if (err != ESP_OK || status != 200 || !r.buf) {
        ESP_LOGW(TAG, "release check failed: %s (HTTP %d)", esp_err_to_name(err), status);
        char msg[96];
        snprintf(msg, sizeof(msg), "check failed (HTTP %d)", status);
        set_status(OTA_FAILED, NULL, msg);
        free(r.buf);
        return false;
    }
    if (r.overflow) {
        ESP_LOGW(TAG, "release JSON exceeded %d bytes", CHECK_BUF_CAP);
        set_status(OTA_FAILED, NULL, "release JSON too large");
        free(r.buf);
        return false;
    }

    cJSON *root = cJSON_Parse(r.buf);
    free(r.buf);
    if (!root) {
        set_status(OTA_FAILED, NULL, "bad release JSON");
        return false;
    }

    bool ok = false;
    // The release object to actually pull tag_name/assets from: /latest's
    // singular object for the stable channel, or -- beta on -- whichever
    // entry of the list endpoint carries the newest version among the first
    // RELEASES_LIST_SCAN_CAP (newest-first) entries. cJSON owns all of
    // `chosen`'s memory either way (it's a view into `root`), so there is
    // nothing to free beyond the one cJSON_Delete(root) at the bottom.
    cJSON *chosen = NULL;
    char   latest[16];

    if (!beta) {
        chosen = root;
        if (!release_version(chosen, latest, sizeof(latest))) {
            set_status(OTA_FAILED, NULL, "no tag_name in release");
            goto done;
        }
    } else {
        if (!cJSON_IsArray(root)) {
            set_status(OTA_FAILED, NULL, "release list JSON not an array");
            goto done;
        }
        char chosen_ver[16] = { 0 };
        int n = cJSON_GetArraySize(root);
        if (n > RELEASES_LIST_SCAN_CAP) n = RELEASES_LIST_SCAN_CAP;
        for (int i = 0; i < n; i++) {
            cJSON *rel = cJSON_GetArrayItem(root, i);
            if (!cJSON_IsObject(rel)) continue;
            if (cJSON_IsTrue(cJSON_GetObjectItem(rel, "draft"))) continue;
            char ver[16];
            if (!release_version(rel, ver, sizeof(ver))) continue;
            if (!chosen || is_newer(ver, chosen_ver)) {
                chosen = rel;
                strlcpy(chosen_ver, ver, sizeof(chosen_ver));
            }
        }
        if (!chosen) {
            set_status(OTA_FAILED, NULL, "no usable release in list");
            goto done;
        }
        strlcpy(latest, chosen_ver, sizeof(latest));
    }

    {
        char asset_url[sizeof(s_asset_url)] = { 0 };
        cJSON *assets = cJSON_GetObjectItem(chosen, "assets");
        cJSON *a;
        cJSON_ArrayForEach(a, assets) {
            cJSON *name = cJSON_GetObjectItem(a, "name");
            if (!cJSON_IsString(name) || strcmp(name->valuestring, ASSET_NAME) != 0) continue;
            cJSON *url = cJSON_GetObjectItem(a, "browser_download_url");
            if (cJSON_IsString(url)) strlcpy(asset_url, url->valuestring, sizeof(asset_url));
            break;
        }

        if (!asset_url[0]) {
            set_status(OTA_FAILED, latest, "no " ASSET_NAME " asset in latest release");
        } else if (is_newer(latest, desc->version)) {
            taskENTER_CRITICAL(&s_mux);
            strlcpy(s_asset_url, asset_url, sizeof(s_asset_url));
            taskEXIT_CRITICAL(&s_mux);
            set_status(OTA_AVAILABLE, latest, NULL);
            ok = true;
        } else {
            set_status(OTA_IDLE, latest, NULL);
            ok = true;
        }
    }

done:
    cJSON_Delete(root);
    return ok;
}

void dial_ota_set_blocked(const char *reason)
{
    set_status(OTA_FAILED, NULL, reason);
}

/* ---- download + apply ---------------------------------------------------*/

bool dial_ota_download_and_apply(void (*progress_cb)(int pct))
{
    char asset_url[sizeof(s_asset_url)];
    taskENTER_CRITICAL(&s_mux);
    strlcpy(asset_url, s_asset_url, sizeof(asset_url));
    taskEXIT_CRITICAL(&s_mux);

    set_status(OTA_DOWNLOADING, NULL, NULL);
    set_progress(0);

    if (!asset_url[0]) {
        set_status(OTA_FAILED, NULL, "no update URL (check() never ran or found none)");
        return false;
    }

    esp_http_client_config_t http_cfg = {
        .url        = asset_url,
        .cert_pem   = dial_oauth_root_ca(),
        .timeout_ms = 30000,
        .buffer_size = 4096,
        /* GitHub 302s the asset to a signed URL ~900 bytes long; the default
         * 512-byte TX buffer can't even fit the redirected request line. */
        .buffer_size_tx = 4096,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin: %s", esp_err_to_name(err));
        char msg[96];
        snprintf(msg, sizeof(msg), "download start failed: %s", esp_err_to_name(err));
        set_status(OTA_FAILED, NULL, msg);
        return false;
    }

    int image_size = esp_https_ota_get_image_size(handle);
    int last_pct = -1;
    while ((err = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int read = esp_https_ota_get_image_len_read(handle);
        int pct = (image_size > 0) ? (int)(((int64_t)read * 100) / image_size) : 0;
        if (pct != last_pct) {
            last_pct = pct;
            set_progress(pct);
        }
        if (progress_cb) progress_cb(pct);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_perform: %s", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        char msg[96];
        snprintf(msg, sizeof(msg), "download failed: %s", esp_err_to_name(err));
        set_status(OTA_FAILED, NULL, msg);
        return false;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "incomplete image received");
        esp_https_ota_abort(handle);
        set_status(OTA_FAILED, NULL, "incomplete image received");
        return false;
    }

    // esp_https_ota_finish() cleans up the handle regardless of its return
    // value -- esp_https_ota_abort() must NOT be called after this point
    // (see esp_https_ota.h's note beside esp_https_ota_abort).
    err = esp_https_ota_finish(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish: %s", esp_err_to_name(err));
        char msg[96];
        snprintf(msg, sizeof(msg), "image validation failed: %s", esp_err_to_name(err));
        set_status(OTA_FAILED, NULL, msg);
        return false;
    }

    ESP_LOGI(TAG, "OTA image written and verified; ready to reboot");
    set_progress(100);
    set_status(OTA_READY_REBOOT, NULL, NULL);
    return true;
}

/* ---- rollback health check ----------------------------------------------*/

void dial_ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    bool pending = false;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_get_state_partition failed at boot; assuming not pending");
    } else {
        pending = (ota_state == ESP_OTA_IMG_PENDING_VERIFY);
    }
    taskENTER_CRITICAL(&s_mux);
    s_info.pending_verify = pending;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "boot pending-verify: %s", pending ? "true (rollback armed)" : "false");
}

void dial_ota_mark_valid_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_get_state_partition failed; leaving rollback state as-is");
        return;
    }
    if (ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "boot not pending verification (state %d) -- nothing to do", ota_state);
        return;
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        ESP_LOGI(TAG, "app marked valid; rollback cancelled");
        // Clears the mirror so scr_connecting/scr_dial's "Finalizing update"
        // notice drops as soon as the caller (main.c) re-commits the OTA
        // snapshot -- pending_verify must never stay true once the
        // bootloader has actually stopped tracking a rollback.
        taskENTER_CRITICAL(&s_mux);
        s_info.pending_verify = false;
        taskEXIT_CRITICAL(&s_mux);
    } else {
        ESP_LOGE(TAG, "failed to mark app valid / cancel rollback");
    }
}
