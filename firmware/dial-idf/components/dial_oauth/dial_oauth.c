/*
 * OAuth 2.1 client for the Orion MCP server. Port of the project's earlier
 * TypeScript prototype (reference-dial/dial.ts, in repo history); steps 1-2
 * + PKCE here, authorize/token next. Raw HTTP via
 * esp_http_client + the IDF cert bundle; JSON via cJSON; PKCE via mbedTLS.
 */

#include "dial_oauth.h"
#include "dial_link_config.h"   // ORION_DIAL_RELAY_BASE — the code-relay origin

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_system.h"   // esp_get_free_heap_size, in the transport error detail

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

// Set on every http_do() call (cleared on success, so it never outlives the
// request it describes). See dial_oauth_last_err_cert().
static bool s_last_err_cert;

// One persistent keep-alive handle for ALL of this component's requests
// (discovery, DCR, token), exactly like dial_mcp's s_http. Field incident
// 2026-07-28: opening a fresh TLS connection per request, retried every few
// seconds, tripped a per-device new-connection rate limit (home-router flood
// protection) that then chopped every later handshake in the burst — the
// dial could reach the MCP endpoint (one reused connection) but never its
// own token endpoint. One reused connection per component keeps the dial's
// handshake rate at "polite client" levels no matter how long it retries.
static esp_http_client_handle_t s_http;
static resp_t *s_resp;   // per-call sink for on_http (single worker task)

static esp_err_t on_http(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0 && s_resp) {
        resp_t *r = s_resp;
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

// Why the last esp_http_client_perform() failed at the transport layer, kept
// because that path reports no status and no body -- see where it's filled in.
static char s_transport_err[112];

static void http_reset(void)
{
    if (s_http) { esp_http_client_cleanup(s_http); s_http = NULL; }
}

// Perform an HTTP request on the shared keep-alive handle. body_in/
// content_type NULL for GET. Returns HTTP status (or -1); *body_out is a
// malloc'd, NUL-terminated body (caller frees).
static int http_do(const char *url, esp_http_client_method_t method,
                   const char *content_type, const char *body_in, char **body_out,
                   int timeout_ms)
{
    resp_t r = { 0 };
    s_resp = &r;

    // One transparent retry on a fresh connection: a kept-alive socket the
    // server quietly closed fails its first reuse — same workaround
    // dial_mcp's http_send carries, not a general retry loop.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!s_http) {
            esp_http_client_config_t cfg = {
                .url = url,
                .event_handler = on_http,
                .cert_pem = orion_root_ca_pem_start,   // verify against Orion's embedded roots
                .timeout_ms = timeout_ms,
                .keep_alive_enable = true,
            };
            s_http = esp_http_client_init(&cfg);
            if (!s_http) { s_resp = NULL; *body_out = NULL; return -1; }
        } else {
            esp_http_client_set_url(s_http, url);
        }
        // The handle is kept alive and reused, so its init-time timeout would
        // otherwise stick at whatever the FIRST caller set. Re-apply per call so
        // the relay poll's short timeout (below) actually takes effect and can't
        // let one blocking poll stall the consent loop's reboot servicing.
        esp_http_client_set_timeout_ms(s_http, timeout_ms);
        esp_http_client_set_method(s_http, method);
        esp_http_client_set_header(s_http, "Accept", "application/json");
        // Headers and the POST body PERSIST on a reused handle: clear both
        // explicitly on requests that don't carry them, or a GET after a
        // token POST replays the old body.
        if (content_type) esp_http_client_set_header(s_http, "Content-Type", content_type);
        else              esp_http_client_delete_header(s_http, "Content-Type");
        esp_http_client_set_post_field(s_http, body_in, body_in ? strlen(body_in) : 0);

        err = esp_http_client_perform(s_http);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "%s: %s%s", url, esp_err_to_name(err),
                 attempt == 0 ? " (retrying on a fresh connection)" : "");
        // Salvage the TLS detail BEFORE dropping the handle (cert check below).
        int tc = 0, tf = 0;
        esp_http_client_get_and_clear_last_tls_error(s_http, &tc, &tf);
        s_last_err_cert = tf != 0;
        // A transport failure returns -1 with no body, so "HTTP -1" was the
        // whole story anyone got -- on screen and in the log. Keep the reason
        // here: esp_err_to_name already separates DNS from connect from
        // handshake, the TLS pair separates a cert problem from a truncated
        // one, and the heap number catches the handshake that couldn't afford
        // its ~40KB.
        snprintf(s_transport_err, sizeof(s_transport_err), "%s tls=0x%x/0x%x heap=%u",
                 esp_err_to_name(err), tc, tf, (unsigned)esp_get_free_heap_size());
        http_reset();
        free(r.buf); r = (resp_t){ 0 };   // don't glue two attempts' bodies together
    }

    int status = (err == ESP_OK) ? esp_http_client_get_status_code(s_http) : -1;
    if (err == ESP_OK) {
        // Nonzero verify flags (MBEDTLS_X509_BADCERT_NOT_TRUSTED/EXPIRED/...)
        // mean the handshake failed because our embedded trust anchors don't
        // vouch for the server's cert — worth telling apart from a plain
        // outage. Cleared every call so a stale flag never leaks forward.
        int tls_code = 0, tls_flags = 0;
        esp_http_client_get_and_clear_last_tls_error(s_http, &tls_code, &tls_flags);
        s_last_err_cert = tls_flags != 0;
    }

    s_resp = NULL;
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

const char *dial_oauth_root_ca(void) { return orion_root_ca_pem_start; }

/* ---- discovery ------------------------------------------------------- */

bool dial_oauth_discover(oauth_disc_t *out)
{
    memset(out, 0, sizeof(*out));
    char *body = NULL;
    int st = http_do(MCP_ORIGIN "/.well-known/oauth-authorization-server",
                     HTTP_METHOD_GET, NULL, NULL, &body, 15000);
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
    st = http_do(MCP_ORIGIN "/.well-known/oauth-protected-resource/", HTTP_METHOD_GET, NULL, NULL, &body, 15000);
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
                     "application/json", reqstr, &body, 15000);
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

void dial_oauth_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "access");
    nvs_erase_key(h, "refresh");
    // Also drop the DCR client_id AND its cached redirect_uri. The redirect is
    // now one fleet-wide constant (the relay's /cb), but a device that first
    // linked on PRE-RELAY firmware still has the old on-LAN redirect
    // (http://orion-dial-xxxx.local/callback) cached here, and
    // dial_oauth_cached_redirect() would hand it back on the next boot — so the
    // re-link QR would carry that dead .local redirect, the phone's approval
    // would be sent to a callback server that no longer exists, and the relay
    // poll would wait forever. Erasing both makes the next boot re-register a
    // fresh client against the relay redirect and re-consent once. Safe: forget()
    // means "fully unlink" (the refresh token is already gone above, so the old
    // client holds nothing), and a healthy device that is still refreshing never
    // calls this, so it is never dragged through a needless re-registration.
    nvs_erase_key(h, "client_id");
    nvs_erase_key(h, "redirect_uri");
    nvs_commit(h);
    nvs_close(h);
}

/* ---- "session lapsed" marker -------------------------------------------- */
// A u8 in this same namespace, deliberately NOT erased by dial_oauth_forget():
// forget() is exactly what a lapse calls, and the flag exists to survive it (and
// the reboot that follows) so the next boot can tell "expired" from "never
// linked". nvs_flash_erase (factory reset) still wipes it with everything else.
#define NVS_KEY_LAPSED "lapsed"

void dial_oauth_set_lapsed(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (on) nvs_set_u8(h, NVS_KEY_LAPSED, 1);
    else    nvs_erase_key(h, NVS_KEY_LAPSED);   // ESP_ERR_NVS_NOT_FOUND is fine: already clear
    nvs_commit(h);
    nvs_close(h);
}

bool dial_oauth_is_lapsed(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    bool lapsed = nvs_get_u8(h, NVS_KEY_LAPSED, &v) == ESP_OK && v == 1;
    nvs_close(h);
    return lapsed;
}

bool dial_oauth_have_refresh(void)
{
    char rt[512];   // same bound dial_oauth_refresh reads it with
    return nvs_get("refresh", rt, sizeof(rt));
}

// Drop ONLY the access token, keeping the refresh token and client_id.
// dial_oauth_have_valid_access() reports presence, not validity, so a token the
// server has expired or revoked still reads as "valid" here and the supervisor
// would skip the refresh branch forever. Erasing it is what lets the next pass
// fall through to refresh — and, if that fails too, to interactive consent —
// instead of spinning on a token that will never be accepted again.
void dial_oauth_forget_access(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "access");
    nvs_commit(h);
    nvs_close(h);
}

bool dial_oauth_cached_redirect(char *dst, size_t sz)
{
    char cid[96];
    if (!nvs_get("client_id", cid, sizeof(cid))) return false;   // no client -> nothing to preserve
    return nvs_get("redirect_uri", dst, sz);
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

/* ---- token exchange (shared by authorize + refresh) ------------------ */

static char s_token_err[192];   // last token-endpoint error, for on-screen display
static bool s_token_err_permanent;   // see dial_oauth_last_token_err_permanent

// RFC 6749 §5.2: the token endpoint reports a dead refresh token (expired,
// revoked, or rotated out from under us) as an error body with
// "error":"invalid_grant". That's the one condition that will never clear on
// its own; every other failure body is either absent or a different error.
static bool is_invalid_grant(const char *body)
{
    if (!body) return false;
    cJSON *j = cJSON_Parse(body);
    if (!j) return false;
    char err[32] = { 0 };
    bool bad = json_get_str(j, "error", err, sizeof(err)) && strcmp(err, "invalid_grant") == 0;
    cJSON_Delete(j);
    return bad;
}

static bool token_request(const char *token_endpoint, const char *body)
{
    char *resp = NULL;
    int st = http_do(token_endpoint, HTTP_METHOD_POST,
                     "application/x-www-form-urlencoded", body, &resp, 15000);
    if (st != 200 || !resp) {
        ESP_LOGE(TAG, "token HTTP %d: %s", st, resp ? resp : "(no body)");
        if (st <= 0)
            snprintf(s_token_err, sizeof(s_token_err), "HTTP %d\n%s", st, s_transport_err);
        else
            snprintf(s_token_err, sizeof(s_token_err), "HTTP %d\n%.150s", st, resp ? resp : "");
        // Permanent ONLY for the RFC-defined dead-refresh-token signal (400/401
        // + invalid_grant). Transport failures (status <= 0), 408/429, 5xx, and
        // any other 4xx are transient -- worth retrying, not worth re-linking.
        s_token_err_permanent = (st == 400 || st == 401) && is_invalid_grant(resp);
        free(resp);
        return false;
    }
    s_token_err_permanent = false;   // a 200 response is never the dead-token signal
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

/* ---- interactive authorize: PKCE state + relay poll ------------------ */

static char s_verifier[80];
static char s_state[32];
static char s_code[1024];   // Orion auth codes can be long; avoid truncation
static volatile bool s_got_code;

bool dial_oauth_start_authorize(const oauth_disc_t *disc, const char *client_id,
                                const char *redirect_uri, char *url_out, size_t url_sz)
{
    char challenge[64];
    dial_oauth_pkce(s_verifier, sizeof(s_verifier), challenge, sizeof(challenge));

    // state is both the OAuth CSRF/binding value the code is checked against
    // AND the relay mailbox key the phone's /cb write and the dial's /poll read
    // agree on, so it must be unguessable — a bearer secret. 16 random bytes ->
    // 128-bit, base64url.
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

    // No on-device server: the phone's approval is redirected to the hosted
    // relay's /cb (redirect_uri above), which stashes the code keyed by state.
    // The dial collects it with dial_oauth_poll_relay_once (outbound HTTPS) —
    // the whole port-80 httpd/socket-exhaustion path is gone. Only PKCE state
    // is recorded here; nothing is opened to listen.
    ESP_LOGI(TAG, "authorize URL built; code arrives via relay poll (%s)", redirect_uri);
    return true;
}

dial_relay_poll_t dial_oauth_poll_relay_once(const oauth_disc_t *disc)
{
    (void)disc;   // relay host is a fixed constant, not one of disc's endpoints
    if (s_got_code) return DIAL_RELAY_GOT;   // already captured; nothing to fetch

    // GET <RELAY>/poll?state=… : {"code":"…"} once the phone's redirect has
    // deposited the code (single-use — the relay deletes on read), otherwise
    // {"status":"pending"}. state fits s_state (base64url of 16 bytes) and the
    // relay origin is a compile constant, so 256 bytes is ample.
    char url[256];
    snprintf(url, sizeof(url), ORION_DIAL_RELAY_BASE "/poll?state=%s", s_state);

    // A SHORT timeout (vs the 15s the token/discovery legs use): this runs
    // inside the consent loop's slice, and the worker task is the only one that
    // services Settings' Re-link / Change-network / Factory-reset — a poll that
    // blocked for 15s on a slow/unreachable relay would make those feel dead.
    char *body = NULL;
    int st = http_do(url, HTTP_METHOD_GET, NULL, NULL, &body, 5000);
    if (st != 200 || !body) {
        ESP_LOGD(TAG, "relay poll HTTP %d", st);   // transient; the worker keeps slicing
        free(body);
        return DIAL_RELAY_ERROR;
    }

    cJSON *j = cJSON_Parse(body);
    free(body);
    if (!j) return DIAL_RELAY_ERROR;
    // json_get_str truncates to fit (strict bounds) and leaves s_code untouched
    // when "code" is absent, so a pending response never clobbers a code we may
    // already hold. The relay returns the code raw (not percent-encoded), so no
    // decode step — finish_authorize url-encodes it exactly once for the token
    // request (double-encoding => "invalid code format").
    bool got = json_get_str(j, "code", s_code, sizeof(s_code)) && s_code[0];
    cJSON_Delete(j);
    if (!got) return DIAL_RELAY_PENDING;

    s_got_code = true;   // s_code holds the code; finish_authorize reads it + s_verifier
    ESP_LOGI(TAG, "relay delivered auth code (%d bytes)", (int)strlen(s_code));
    return DIAL_RELAY_GOT;
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

// Has the browser come back with the code yet? Lets the caller wait for consent
// in slices it can do other work between, instead of parking in the call above
// for the whole window -- the worker task is the only thing that services the
// command queue, so a five-minute park in here left Settings' reboot actions
// looking dead. The wait above still handles the arrival itself.
bool dial_oauth_have_code(void) { return s_got_code; }

void dial_oauth_stop_authorize(void)
{
    // No callback server to tear down any more (the relay poll replaced the
    // on-LAN httpd). Retire the spent mailbox key so a stale state can't be
    // polled again; the captured code and PKCE verifier are deliberately left
    // intact for the finish_authorize that follows this call.
    s_state[0] = 0;
}

const char *dial_oauth_last_error(void) { return s_token_err; }

bool dial_oauth_last_token_err_permanent(void) { return s_token_err_permanent; }

bool dial_oauth_last_err_cert(void) { return s_last_err_cert; }

void dial_oauth_release_connection(void) { http_reset(); }
