/*
 * OAuth 2.1 client for the Orion MCP server. Port of firmware/reference-dial/
 * dial.ts (steps 1-2 + PKCE here; authorize/token next). Raw HTTP via
 * esp_http_client + the IDF cert bundle; JSON via cJSON; PKCE via mbedTLS.
 */

#include "dial_oauth.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_http_server.h"

// Trust anchors for Orion's server (GTS Root R4 + GlobalSign Root CA), embedded
// from the system trust store. Used instead of the IDF cert bundle, whose
// binary-search matching fails to find any root in this IDF v6.0 build.
extern const char orion_root_ca_pem_start[] asm("_binary_orion_root_ca_pem_start");
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"

static const char *TAG = "oauth";

#define MCP_ORIGIN "https://mcp.orionsleep.com"
#define OAUTH_SCOPE "orion:mcp"
#define NVS_NS "oauth"

/* ---- base64url ------------------------------------------------------- */

static void base64url(const uint8_t *in, size_t in_len, char *out, size_t out_sz)
{
    size_t olen = 0;
    // mbedtls writes standard base64 (with padding); transform to base64url.
    mbedtls_base64_encode((unsigned char *)out, out_sz, &olen, in, in_len);
    char *w = out;
    for (char *r = out; *r; r++) {
        if (*r == '+') *w++ = '-';
        else if (*r == '/') *w++ = '_';
        else if (*r == '=') { /* drop padding */ }
        else *w++ = *r;
    }
    *w = 0;
}

/* ---- PKCE ------------------------------------------------------------ */

void dial_oauth_pkce(char *verifier, size_t vsz, char *challenge, size_t csz)
{
    uint8_t rnd[32];
    esp_fill_random(rnd, sizeof(rnd));
    base64url(rnd, sizeof(rnd), verifier, vsz);   // 43-char verifier

    uint8_t hash[32];
    mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
               (const unsigned char *)verifier, strlen(verifier), hash);
    base64url(hash, sizeof(hash), challenge, csz);
}

/* ---- HTTP ------------------------------------------------------------ */

typedef struct { char *buf; int len; int cap; } resp_t;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0) {
        resp_t *r = e->user_data;
        if (r->len + e->data_len + 1 > r->cap) {
            r->cap = (r->len + e->data_len) * 2 + 512;
            r->buf = realloc(r->buf, r->cap);
        }
        if (r->buf) {
            memcpy(r->buf + r->len, e->data, e->data_len);
            r->len += e->data_len;
            r->buf[r->len] = 0;
        }
    }
    return ESP_OK;
}

// Perform an HTTP request. body_in/content_type NULL for GET. Returns HTTP
// status (or -1); *body_out is a malloc'd, NUL-terminated body (caller frees).
static int http_do(const char *url, esp_http_client_method_t method,
                   const char *content_type, const char *body_in, char **body_out)
{
    resp_t r = { 0 };
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http,
        .user_data = &r,
        .cert_pem = orion_root_ca_pem_start,   // verify against Orion's embedded roots
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_method(c, method);
    esp_http_client_set_header(c, "Accept", "application/json");
    if (content_type) esp_http_client_set_header(c, "Content-Type", content_type);
    if (body_in) esp_http_client_set_post_field(c, body_in, strlen(body_in));

    esp_err_t err = esp_http_client_perform(c);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(c) : -1;
    if (err != ESP_OK) ESP_LOGW(TAG, "%s: %s", url, esp_err_to_name(err));
    esp_http_client_cleanup(c);
    *body_out = r.buf;
    return status;
}

static bool json_get_str(cJSON *o, const char *key, char *dst, size_t sz)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    if (!cJSON_IsString(v) || !v->valuestring) return false;
    strncpy(dst, v->valuestring, sz - 1);
    dst[sz - 1] = 0;
    return true;
}

/* ---- discovery ------------------------------------------------------- */

bool dial_oauth_discover(oauth_disc_t *out)
{
    memset(out, 0, sizeof(*out));
    char *body = NULL;
    int st = http_do(MCP_ORIGIN "/.well-known/oauth-authorization-server",
                     HTTP_METHOD_GET, NULL, NULL, &body);
    if (st != 200 || !body) { ESP_LOGE(TAG, "discovery HTTP %d", st); free(body); return false; }

    cJSON *as = cJSON_Parse(body);
    free(body);
    if (!as) { ESP_LOGE(TAG, "discovery JSON parse failed"); return false; }
    bool ok = json_get_str(as, "authorization_endpoint", out->authorization_endpoint, sizeof(out->authorization_endpoint))
           && json_get_str(as, "token_endpoint", out->token_endpoint, sizeof(out->token_endpoint))
           && json_get_str(as, "registration_endpoint", out->registration_endpoint, sizeof(out->registration_endpoint));
    cJSON_Delete(as);
    if (!ok) { ESP_LOGE(TAG, "discovery missing endpoints"); return false; }

    // Protected-resource metadata → resource (RFC 8707). Fallback to origin.
    strncpy(out->resource, MCP_ORIGIN, sizeof(out->resource) - 1);
    body = NULL;
    st = http_do(MCP_ORIGIN "/.well-known/oauth-protected-resource/", HTTP_METHOD_GET, NULL, NULL, &body);
    if (st == 200 && body) {
        cJSON *prm = cJSON_Parse(body);
        if (prm) { json_get_str(prm, "resource", out->resource, sizeof(out->resource)); cJSON_Delete(prm); }
    }
    free(body);
    return true;
}

/* ---- Dynamic Client Registration ------------------------------------ */

static bool nvs_get(const char *key, char *dst, size_t sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    esp_err_t e = nvs_get_str(h, key, dst, &sz);
    nvs_close(h);
    return e == ESP_OK && dst[0];
}

static void nvs_put(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, key, val);
    nvs_commit(h);
    nvs_close(h);
}

bool dial_oauth_ensure_client(const oauth_disc_t *disc, const char *redirect_uri,
                              char *client_id_out, size_t sz)
{
    // Reuse a cached client_id only if it was registered for this redirect_uri.
    char cached_uri[160];
    if (nvs_get("client_id", client_id_out, sz) &&
        nvs_get("redirect_uri", cached_uri, sizeof(cached_uri)) &&
        strcmp(cached_uri, redirect_uri) == 0) {
        ESP_LOGI(TAG, "reusing cached client_id");
        return true;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "client_name", "orion-knob-dial");
    cJSON *uris = cJSON_AddArrayToObject(req, "redirect_uris");
    cJSON_AddItemToArray(uris, cJSON_CreateString(redirect_uri));
    cJSON *grants = cJSON_AddArrayToObject(req, "grant_types");
    cJSON_AddItemToArray(grants, cJSON_CreateString("authorization_code"));
    cJSON_AddItemToArray(grants, cJSON_CreateString("refresh_token"));
    cJSON *rtypes = cJSON_AddArrayToObject(req, "response_types");
    cJSON_AddItemToArray(rtypes, cJSON_CreateString("code"));
    cJSON_AddStringToObject(req, "token_endpoint_auth_method", "none");
    cJSON_AddStringToObject(req, "scope", OAUTH_SCOPE);
    char *reqstr = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    char *body = NULL;
    int st = http_do(disc->registration_endpoint, HTTP_METHOD_POST,
                     "application/json", reqstr, &body);
    free(reqstr);
    if ((st != 200 && st != 201) || !body) {
        ESP_LOGE(TAG, "DCR HTTP %d: %s", st, body ? body : "(no body)");
        free(body);
        return false;
    }

    cJSON *reg = cJSON_Parse(body);
    free(body);
    bool ok = reg && json_get_str(reg, "client_id", client_id_out, sz);
    cJSON_Delete(reg);
    if (!ok) { ESP_LOGE(TAG, "DCR: no client_id in response"); return false; }

    nvs_put("client_id", client_id_out);
    nvs_put("redirect_uri", redirect_uri);
    ESP_LOGI(TAG, "registered new client_id");
    return true;
}

/* ---- token storage --------------------------------------------------- */

static void save_tokens(const char *access, const char *refresh)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "access", access);
    if (refresh && refresh[0]) nvs_set_str(h, "refresh", refresh);
    nvs_commit(h);
    nvs_close(h);
}

bool dial_oauth_access_token(char *out, size_t sz)
{
    return nvs_get("access", out, sz);
}

bool dial_oauth_have_valid_access(void)
{
    // No wall clock yet: presence == usable. Expiry is handled by refreshing on
    // an MCP 401 (added with the MCP client). TODO: SNTP + expires_in check.
    char tmp[8];
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(tmp);
    bool have = (nvs_get_str(h, "access", NULL, &len) == ESP_OK) && len > 1;
    nvs_close(h);
    return have;
}

/* ---- url-encode ------------------------------------------------------ */

static void url_encode(const char *in, char *out, size_t out_sz)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;
    for (const char *p = in; *p && o + 4 < out_sz; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = c;
        } else {
            out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 0xF];
        }
    }
    out[o] = 0;
}

// Decode a query-string value in place (httpd does NOT url-decode; %XX + '+').
static void url_decode(char *s)
{
    char *o = s;
    for (char *i = s; *i; i++) {
        if (*i == '+') { *o++ = ' '; }
        else if (*i == '%' && i[1] && i[2]) {
            int hi = i[1], lo = i[2];
            hi = hi <= '9' ? hi - '0' : (hi | 0x20) - 'a' + 10;
            lo = lo <= '9' ? lo - '0' : (lo | 0x20) - 'a' + 10;
            *o++ = (char)((hi << 4) | lo); i += 2;
        } else { *o++ = *i; }
    }
    *o = 0;
}

/* ---- token exchange (shared by authorize + refresh) ------------------ */

static char s_token_err[192];   // last token-endpoint error, for on-screen display

static bool token_request(const char *token_endpoint, const char *body)
{
    char *resp = NULL;
    int st = http_do(token_endpoint, HTTP_METHOD_POST,
                     "application/x-www-form-urlencoded", body, &resp);
    if (st != 200 || !resp) {
        ESP_LOGE(TAG, "token HTTP %d: %s", st, resp ? resp : "(no body)");
        snprintf(s_token_err, sizeof(s_token_err), "HTTP %d\n%.150s", st, resp ? resp : "");
        free(resp);
        return false;
    }
    cJSON *j = cJSON_Parse(resp);
    free(resp);
    if (!j) return false;
    // Heap, not stack: access tokens are large (~1-2KB) and this runs in a task
    // whose stack is mostly consumed by the TLS handshake.
    char *access = calloc(1, 2048), *refresh = calloc(1, 512);
    bool ok = access && refresh && json_get_str(j, "access_token", access, 2048);
    if (ok) json_get_str(j, "refresh_token", refresh, 512);
    cJSON_Delete(j);
    if (ok) {
        save_tokens(access, refresh);
        ESP_LOGI(TAG, "tokens stored (access %d bytes, refresh %s)",
                 (int)strlen(access), refresh[0] ? "yes" : "no");
    } else {
        ESP_LOGE(TAG, "token response missing access_token");
    }
    free(access); free(refresh);
    return ok;
}

bool dial_oauth_refresh(const oauth_disc_t *disc, const char *client_id)
{
    char rt[512];
    if (!nvs_get("refresh", rt, sizeof(rt))) return false;
    char ert[600], eres[256], body[1200];
    url_encode(rt, ert, sizeof(ert));
    url_encode(disc->resource, eres, sizeof(eres));
    snprintf(body, sizeof(body),
             "grant_type=refresh_token&refresh_token=%s&client_id=%s&resource=%s&scope=%s",
             ert, client_id, eres, OAUTH_SCOPE);
    bool ok = token_request(disc->token_endpoint, body);
    if (ok) ESP_LOGI(TAG, "access token refreshed");
    return ok;
}

/* ---- interactive authorize: PKCE state + callback server ------------- */

static char s_verifier[80];
static char s_state[32];
static char s_code[1024];   // Orion auth codes can be long; avoid truncation
static volatile bool s_got_code;
static httpd_handle_t s_cb_httpd;

// Static scratch so the HTTP server task's stack stays small (large on-stack
// buffers here overflow the httpd task and reboot the board). s_code doubles as
// the captured-code buffer.
static char s_cb_query[1600];
static char s_cb_state[64];

static esp_err_t cb_handler(httpd_req_t *req)
{
    s_cb_query[0] = 0;
    httpd_req_get_url_query_str(req, s_cb_query, sizeof(s_cb_query));
    s_code[0] = s_cb_state[0] = 0;
    httpd_query_key_value(s_cb_query, "code", s_code, sizeof(s_code));
    httpd_query_key_value(s_cb_query, "state", s_cb_state, sizeof(s_cb_state));
    // httpd does not URL-decode query values; decode so the token request
    // re-encodes exactly once (double-encoding => "invalid code format").
    url_decode(s_code);
    url_decode(s_cb_state);
    bool match = (s_code[0] && strcmp(s_cb_state, s_state) == 0);
    ESP_LOGI(TAG, "callback hit: code=%s state_match=%d", s_code[0] ? "yes" : "no", match);

    httpd_resp_set_type(req, "text/html");
    if (match) {
        s_got_code = true;   // s_code already holds the code
        httpd_resp_sendstr(req,
            "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<body style='font-family:sans-serif;text-align:center;margin-top:40px'>"
            "<h2 style='color:#0b6'>Dial authorized \xE2\x9C\x93</h2>"
            "<p>You can close this page and return to the dial.</p></body>");
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "authorization failed (state mismatch or no code)");
    }
    return ESP_OK;
}

bool dial_oauth_start_authorize(const oauth_disc_t *disc, const char *client_id,
                                const char *redirect_uri, char *url_out, size_t url_sz)
{
    char challenge[64];
    dial_oauth_pkce(s_verifier, sizeof(s_verifier), challenge, sizeof(challenge));

    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    base64url(rnd, sizeof(rnd), s_state, sizeof(s_state));
    s_got_code = false;
    s_code[0] = 0;

    char eredir[128], escope[32], eres[256];
    url_encode(redirect_uri, eredir, sizeof(eredir));
    url_encode(OAUTH_SCOPE, escope, sizeof(escope));
    url_encode(disc->resource, eres, sizeof(eres));
    snprintf(url_out, url_sz,
             "%s?response_type=code&client_id=%s&redirect_uri=%s&scope=%s&state=%s"
             "&code_challenge=%s&code_challenge_method=S256&resource=%s",
             disc->authorization_endpoint, client_id, eredir, escope, s_state, challenge, eres);

    // Callback server on the LAN redirect (port 80, path /callback).
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = 80;
    hc.stack_size = 8192;   // default (~4K) overflows parsing the callback query
    hc.lru_purge_enable = true;
    if (httpd_start(&s_cb_httpd, &hc) != ESP_OK) { ESP_LOGE(TAG, "callback httpd failed"); return false; }
    httpd_uri_t cb = { .uri = "/callback", .method = HTTP_GET, .handler = cb_handler };
    httpd_register_uri_handler(s_cb_httpd, &cb);
    ESP_LOGI(TAG, "callback server up on %s", redirect_uri);
    return true;
}

bool dial_oauth_finish_authorize(const oauth_disc_t *disc, const char *client_id,
                                 const char *redirect_uri, int timeout_ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (!s_got_code && esp_timer_get_time() < end)
        vTaskDelay(pdMS_TO_TICKS(200));
    if (!s_got_code) { ESP_LOGE(TAG, "authorize timed out"); return false; }

    char *ecode = malloc(3100);   // url-encoded code (up to 3x the 1024 buffer)
    char eredir[128], eres[256];
    if (!ecode) return false;
    url_encode(s_code, ecode, 3100);
    url_encode(redirect_uri, eredir, sizeof(eredir));
    url_encode(disc->resource, eres, sizeof(eres));
    char *body = malloc(4096);
    if (!body) { free(ecode); return false; }
    snprintf(body, 4096,
             "grant_type=authorization_code&code=%s&redirect_uri=%s&client_id=%s"
             "&code_verifier=%s&resource=%s",
             ecode, eredir, client_id, s_verifier, eres);
    free(ecode);
    bool ok = token_request(disc->token_endpoint, body);
    free(body);
    return ok;
}

void dial_oauth_stop_authorize(void)
{
    if (s_cb_httpd) { httpd_stop(s_cb_httpd); s_cb_httpd = NULL; }
}

const char *dial_oauth_last_error(void) { return s_token_err; }
