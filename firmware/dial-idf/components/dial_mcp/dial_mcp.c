/*
 * Raw MCP client over Streamable HTTP for Orion. See dial_mcp.h.
 * Port of the RawMcp class in the project's earlier TypeScript prototype
 * (reference-dial/dial.ts, in repo history).
 */

#include "dial_mcp.h"
#include "dial_oauth.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_timer.h"
#include "cJSON.h"

static const char *TAG = "mcp";

#define MCP_URL "https://mcp.orionsleep.com/"
#define MCP_PROTOCOL_VERSION "2025-06-18"

static char s_session_id[128];
static int  s_rpc_id;
static char s_err[192];

static void set_err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_err, sizeof(s_err), fmt, ap);
    va_end(ap);
    ESP_LOGW(TAG, "%s", s_err);
}

const char *dial_mcp_last_error(void) { return s_err; }

/* ---- HTTP with header capture ---------------------------------------- */

typedef struct {
    char *buf; int len; int cap;
    char content_type[64];
    char session_id[128];
} mcp_resp_t;

/*
 * The connection is held open across calls (keep-alive) instead of being torn
 * down and rebuilt every time.
 *
 * Every call used to run esp_http_client_init -> perform -> cleanup, which
 * meant a fresh TCP connect and a full TLS handshake per MCP call — hundreds of
 * milliseconds of CPU-bound crypto, on a device whose whole job is to answer a
 * knob. That handshake bought nothing: it's the same host, with the same cert,
 * every time. Now one handle is opened lazily and reused.
 *
 * The handle is owned exclusively by the worker task (nothing else calls into
 * dial_mcp), so it needs no locking.
 *
 * Because the response handler's user_data is bound at init time and the handle
 * now outlives a single call, the active response buffer is tracked here rather
 * than passed through the config.
 */
static esp_http_client_handle_t s_http;
static mcp_resp_t              *s_active;

static esp_err_t on_http(esp_http_client_event_t *e)
{
    mcp_resp_t *r = s_active;
    if (!r) return ESP_OK;
    switch (e->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (e->header_key && e->header_value) {
            if (strcasecmp(e->header_key, "Mcp-Session-Id") == 0)
                strlcpy(r->session_id, e->header_value, sizeof(r->session_id));
            else if (strcasecmp(e->header_key, "Content-Type") == 0)
                strlcpy(r->content_type, e->header_value, sizeof(r->content_type));
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (e->data_len > 0) {
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
        break;
    default: break;
    }
    return ESP_OK;
}

static void http_close(void)
{
    if (s_http) {
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
}

static bool http_open(void)
{
    if (s_http) return true;
    esp_http_client_config_t cfg = {
        .url               = MCP_URL,
        .method            = HTTP_METHOD_POST,
        .event_handler     = on_http,
        .cert_pem          = dial_oauth_root_ca(),
        .timeout_ms        = 20000,
        .buffer_size       = 2048,
        .keep_alive_enable = true,     // the whole point: one TLS handshake, many calls
    };
    s_http = esp_http_client_init(&cfg);
    return s_http != NULL;
}

// One POST on the persistent handle. Headers are re-set every call because the
// bearer token changes on refresh, and because a reconnect hands us a fresh
// handle with none of them.
static esp_err_t http_send(const char *method, const char *auth, const char *body)
{
    if (!http_open()) return ESP_ERR_NO_MEM;

    esp_http_client_set_method(s_http, HTTP_METHOD_POST);
    esp_http_client_set_header(s_http, "Content-Type", "application/json");
    esp_http_client_set_header(s_http, "Accept", "application/json, text/event-stream");
    esp_http_client_set_header(s_http, "Authorization", auth);
    if (s_session_id[0]) esp_http_client_set_header(s_http, "Mcp-Session-Id", s_session_id);
    if (strcmp(method, "initialize") != 0)
        esp_http_client_set_header(s_http, "MCP-Protocol-Version", MCP_PROTOCOL_VERSION);
    esp_http_client_set_post_field(s_http, body, strlen(body));

    return esp_http_client_perform(s_http);
}

/*
 * Perform one JSON-RPC POST. On a non-notification call, parses the response
 * (JSON or SSE) and returns the detached `result` cJSON node (caller deletes),
 * or NULL on any error (with s_err set). For notifications returns NULL but
 * sets *ok=true when the server accepted it.
 */
static cJSON *rpc(const char *method, cJSON *params, bool notification, bool *ok)
{
    if (ok) *ok = false;

    // Build request body.
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", method);
    if (params) cJSON_AddItemToObject(req, "params", params);   // takes ownership
    if (!notification) cJSON_AddNumberToObject(req, "id", ++s_rpc_id);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) { set_err("oom building %s", method); return NULL; }

    // Bearer token.
    static char token[4096];
    if (!dial_oauth_access_token(token, sizeof(token))) {
        set_err("no access token");
        free(body);
        return NULL;
    }
    static char auth[4160];
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    mcp_resp_t r = { 0 };
    s_active = &r;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = http_send(method, auth, body);
    if (err != ESP_OK) {
        // A kept-alive connection the server has since dropped (idle timeout,
        // or the Wi-Fi went away and came back) fails on its FIRST use and only
        // then — the socket looks fine until we write to it. That is not a real
        // failure, so rebuild the connection and send once more before saying
        // so. Without this, keep-alive would turn every idle gap into a
        // spurious "Orion unreachable".
        ESP_LOGI(TAG, "connection stale (%s) — reconnecting", esp_err_to_name(err));
        http_close();
        free(r.buf);
        r = (mcp_resp_t){ 0 };
        s_active = &r;
        err = http_send(method, auth, body);
    }
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(s_http) : -1;
    ESP_LOGI(TAG, "%s: %d in %lld ms", method, status, (esp_timer_get_time() - t0) / 1000);
    s_active = NULL;
    free(body);

    if (r.session_id[0]) strlcpy(s_session_id, r.session_id, sizeof(s_session_id));

    if (err != ESP_OK) { set_err("%s: %s", method, esp_err_to_name(err)); free(r.buf); return NULL; }

    if (notification) {
        // Notifications get 202 Accepted (or any 2xx) with no body.
        bool accepted = (status == 202) || (status >= 200 && status < 300);
        if (!accepted) set_err("%s: HTTP %d", method, status);
        if (ok) *ok = accepted;
        free(r.buf);
        return NULL;
    }

    if (status < 200 || status >= 300) {
        set_err("%s: HTTP %d\n%.120s", method, status, r.buf ? r.buf : "");
        free(r.buf);
        return NULL;
    }

    // Parse body: JSON, or SSE with `data:` frames.
    cJSON *msg = NULL;
    if (strstr(r.content_type, "text/event-stream")) {
        // Collect `data:` lines; take the last that parses to a JSON-RPC message.
        char *save = NULL;
        for (char *line = strtok_r(r.buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
            if (strncmp(line, "data:", 5) != 0) continue;
            char *d = line + 5;
            while (*d == ' ') d++;
            cJSON *cand = cJSON_Parse(d);
            if (cand && (cJSON_GetObjectItem(cand, "result") || cJSON_GetObjectItem(cand, "error"))) {
                if (msg) cJSON_Delete(msg);
                msg = cand;
            } else if (cand) {
                cJSON_Delete(cand);
            }
        }
    } else {
        msg = cJSON_Parse(r.buf);
    }
    free(r.buf);

    if (!msg) { set_err("%s: unparseable response", method); return NULL; }

    cJSON *jerr = cJSON_GetObjectItem(msg, "error");
    if (jerr) {
        cJSON *code = cJSON_GetObjectItem(jerr, "code");
        cJSON *emsg = cJSON_GetObjectItem(jerr, "message");
        set_err("%s: rpc %d %.120s", method,
                code ? code->valueint : 0,
                (emsg && emsg->valuestring) ? emsg->valuestring : "");
        cJSON_Delete(msg);
        return NULL;
    }

    cJSON *result = cJSON_DetachItemFromObject(msg, "result");
    cJSON_Delete(msg);
    if (ok) *ok = true;
    return result;   // may be NULL if server omitted it
}

/* ---- public API ------------------------------------------------------ */

bool dial_mcp_connect(char **server_out)
{
    if (server_out) *server_out = NULL;
    s_session_id[0] = 0;   // fresh session

    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "protocolVersion", MCP_PROTOCOL_VERSION);
    cJSON_AddItemToObject(p, "capabilities", cJSON_CreateObject());
    cJSON *ci = cJSON_CreateObject();
    cJSON_AddStringToObject(ci, "name", "orion-knob-dial");
    cJSON_AddStringToObject(ci, "version", "0.1.0");
    cJSON_AddItemToObject(p, "clientInfo", ci);

    bool ok = false;
    cJSON *res = rpc("initialize", p, false, &ok);
    if (!ok) { if (res) cJSON_Delete(res); return false; }

    if (server_out && res) {
        cJSON *si = cJSON_GetObjectItem(res, "serverInfo");
        if (si) {
            cJSON *n = cJSON_GetObjectItem(si, "name");
            cJSON *v = cJSON_GetObjectItem(si, "version");
            char buf[96];
            snprintf(buf, sizeof(buf), "%s %s",
                     (n && n->valuestring) ? n->valuestring : "?",
                     (v && v->valuestring) ? v->valuestring : "");
            *server_out = strdup(buf);
        }
    }
    if (res) cJSON_Delete(res);
    ESP_LOGI(TAG, "initialized; session=%s", s_session_id[0] ? s_session_id : "(none)");

    // Required handshake completion before any other request.
    rpc("notifications/initialized", NULL, true, &ok);
    if (!ok) { ESP_LOGW(TAG, "notifications/initialized not accepted"); return false; }
    return true;
}

int dial_mcp_list_tools_count(void)
{
    bool ok = false;
    cJSON *res = rpc("tools/list", NULL, false, &ok);
    if (!ok || !res) { if (res) cJSON_Delete(res); return -1; }
    cJSON *tools = cJSON_GetObjectItem(res, "tools");
    int n = cJSON_IsArray(tools) ? cJSON_GetArraySize(tools) : -1;
    cJSON_Delete(res);
    return n;
}

bool dial_mcp_call_tool(const char *name, const char *args_json, char **result_out)
{
    if (result_out) *result_out = NULL;

    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "name", name);
    cJSON *args = args_json ? cJSON_Parse(args_json) : cJSON_CreateObject();
    if (!args) args = cJSON_CreateObject();
    cJSON_AddItemToObject(p, "arguments", args);

    bool ok = false;
    cJSON *res = rpc("tools/call", p, false, &ok);
    if (!ok || !res) { if (res) cJSON_Delete(res); return false; }

    // Some servers report tool failures via isError + content, not JSON-RPC error.
    cJSON *is_err = cJSON_GetObjectItem(res, "isError");
    if (cJSON_IsTrue(is_err)) {
        char snippet[128] = "tool error";
        cJSON *content = cJSON_GetObjectItem(res, "content");
        if (cJSON_IsArray(content)) {
            cJSON *first = cJSON_GetArrayItem(content, 0);
            cJSON *t = first ? cJSON_GetObjectItem(first, "text") : NULL;
            if (t && t->valuestring) strlcpy(snippet, t->valuestring, sizeof(snippet));
        }
        set_err("%s: %.120s", name, snippet);
        cJSON_Delete(res);
        return false;
    }

    // Unwrap: prefer structuredContent, else concatenate text content items.
    char *out = NULL;
    cJSON *structured = cJSON_GetObjectItem(res, "structuredContent");
    if (structured) {
        out = cJSON_PrintUnformatted(structured);
    } else {
        cJSON *content = cJSON_GetObjectItem(res, "content");
        if (cJSON_IsArray(content)) {
            size_t total = 1;
            cJSON *it;
            cJSON_ArrayForEach(it, content) {
                cJSON *type = cJSON_GetObjectItem(it, "type");
                cJSON *text = cJSON_GetObjectItem(it, "text");
                if (type && type->valuestring && strcmp(type->valuestring, "text") == 0 &&
                    text && text->valuestring)
                    total += strlen(text->valuestring) + 1;
            }
            out = calloc(1, total);
            if (out) {
                cJSON_ArrayForEach(it, content) {
                    cJSON *type = cJSON_GetObjectItem(it, "type");
                    cJSON *text = cJSON_GetObjectItem(it, "text");
                    if (type && type->valuestring && strcmp(type->valuestring, "text") == 0 &&
                        text && text->valuestring) {
                        if (out[0]) strlcat(out, "\n", total);
                        strlcat(out, text->valuestring, total);
                    }
                }
            }
        }
    }
    cJSON_Delete(res);

    if (!out) { set_err("%s: empty result", name); return false; }
    if (result_out) *result_out = out; else free(out);
    return true;
}
