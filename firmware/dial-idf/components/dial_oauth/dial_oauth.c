/*
 * OAuth 2.1 client for the Orion MCP server. Port of firmware/reference-dial/
 * dial.ts (steps 1-2 + PKCE here; authorize/token next). Raw HTTP via
 * esp_http_client + the IDF cert bundle; JSON via cJSON; PKCE via mbedTLS.
 */

#include "dial_oauth.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_http_client.h"

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

/* ---- foundation smoke test ------------------------------------------ */

void dial_oauth_test(const char *redirect_uri)
{
    char v[80], ch[64];
    dial_oauth_pkce(v, sizeof(v), ch, sizeof(ch));
    ESP_LOGI(TAG, "PKCE verifier=%s", v);
    ESP_LOGI(TAG, "PKCE challenge=%s", ch);

    oauth_disc_t disc;
    if (!dial_oauth_discover(&disc)) { ESP_LOGE(TAG, "discovery failed"); return; }
    ESP_LOGI(TAG, "authorize: %s", disc.authorization_endpoint);
    ESP_LOGI(TAG, "token:     %s", disc.token_endpoint);
    ESP_LOGI(TAG, "register:  %s", disc.registration_endpoint);
    ESP_LOGI(TAG, "resource:  %s", disc.resource);

    char client_id[96];
    if (dial_oauth_ensure_client(&disc, redirect_uri, client_id, sizeof(client_id)))
        ESP_LOGI(TAG, "client_id: %s", client_id);
    else
        ESP_LOGE(TAG, "DCR failed");
}
