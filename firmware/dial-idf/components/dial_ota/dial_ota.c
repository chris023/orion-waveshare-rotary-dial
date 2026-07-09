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
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "cJSON.h"
#include "dial_oauth.h"   // dial_oauth_root_ca() -- covers GitHub's chains too

static const char *TAG = "ota";

#define GITHUB_API_URL \
    "https://api.github.com/repos/chris023/orion-waveshare-rotary-dial/releases/latest"
#define ASSET_NAME     "orion-dial.bin"
#define TAG_PREFIX     "dial-v"
#define CHECK_BUF_CAP  (64 * 1024)   // release JSON is normally ~10-30KB

// Guards s_info and s_asset_url. A short spinlock (never held across a
// blocking call), matching dial_state.c's s_input_mux idiom for cross-task
// state that's only ever read/written as a quick copy.
static portMUX_TYPE       s_mux = portMUX_INITIALIZER_UNLOCKED;
static dial_ota_info_t    s_info;
static char               s_asset_url[300];   // captured by dial_ota_check()

void dial_ota_get(dial_ota_info_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_info;
    taskEXIT_CRITICAL(&s_mux);
}

static void set_status(dial_ota_status_t status, const char *latest, const char *err)
{
    taskENTER_CRITICAL(&s_mux);
    s_info.status = status;
    if (latest) strlcpy(s_info.latest, latest, sizeof(s_info.latest));
    if (err)    strlcpy(s_info.err, err, sizeof(s_info.err));
    else        s_info.err[0] = 0;
    taskEXIT_CRITICAL(&s_mux);
}

static void set_progress(int pct)
{
    taskENTER_CRITICAL(&s_mux);
    s_info.progress_pct = pct;
    taskEXIT_CRITICAL(&s_mux);
}

// Simple semver-ish compare: split on dots, numeric compare major/minor/
// patch. Missing trailing components default to 0 ("1.2" == "1.2.0"). Any
// component that fails to parse as a number for EITHER string is treated as
// "not newer" -- an unexpected tag format must never trigger a spurious
// update.
static bool is_newer(const char *latest, const char *current)
{
    int am = 0, an = 0, ap = 0, bm = 0, bn = 0, bp = 0;
    if (sscanf(latest, "%d.%d.%d", &am, &an, &ap) < 1)  return false;
    if (sscanf(current, "%d.%d.%d", &bm, &bn, &bp) < 1) return false;
    if (am != bm) return am > bm;
    if (an != bn) return an > bn;
    return ap > bp;
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

bool dial_ota_check(void)
{
    set_status(OTA_CHECKING, NULL, NULL);

    const esp_app_desc_t *desc = esp_app_get_description();
    char user_agent[40];
    snprintf(user_agent, sizeof(user_agent), "orion-dial/%s", desc->version);

    check_resp_t r = { 0 };
    esp_http_client_config_t cfg = {
        .url           = GITHUB_API_URL,
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
    cJSON *tag = cJSON_GetObjectItem(root, "tag_name");
    if (!cJSON_IsString(tag) || !tag->valuestring) {
        set_status(OTA_FAILED, NULL, "no tag_name in release");
        goto done;
    }

    {
        const char *ver = tag->valuestring;
        if (!strncmp(ver, TAG_PREFIX, strlen(TAG_PREFIX))) ver += strlen(TAG_PREFIX);
        char latest[16];
        strlcpy(latest, ver, sizeof(latest));

        char asset_url[sizeof(s_asset_url)] = { 0 };
        cJSON *assets = cJSON_GetObjectItem(root, "assets");
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
    } else {
        ESP_LOGE(TAG, "failed to mark app valid / cancel rollback");
    }
}
