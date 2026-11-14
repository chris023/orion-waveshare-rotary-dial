#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dial_http.h"
#include "dial_oauth.h"
#include "dial_mcp.h"
#include "nvs.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"

struct mock_http {
    esp_http_client_config_t cfg;
    esp_http_state_t state;
    int status, id, tls_flags;
    const char *post;
    char auth[64], content_type[64], session[64];
    size_t read_pos, emitted;
};

static struct {
    int status, replay_status, tls_flags;
    esp_err_t perform_err, open_err;
    const char *challenge, *response;
    int read_failure;   // -1: timeout; 1: truncated EOF; 2: deadline
    bool short_write, bad_headers;
    bool check_session;
} fixture;
static int perform_count, open_count, init_count, primary_id, saved_tokens;
static int64_t now;

const char orion_root_ca_pem_start[] asm("_binary_orion_root_ca_pem_start") = "test CA";

static void emit(esp_http_client_handle_t h, size_t start, size_t n)
{
    if (!n) return;
    esp_http_client_event_t e = {
        .event_id = HTTP_EVENT_ON_DATA,
        .data = (void *)(fixture.response + start), .data_len = n,
    };
    h->cfg.event_handler(&e);
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    struct mock_http *h = calloc(1, sizeof(*h));
    assert(h && cfg->cert_pem && cfg->keep_alive_enable);
    h->cfg = *cfg;
    h->id = ++init_count;
    return h;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) { free(h); return ESP_OK; }
esp_err_t esp_http_client_close(esp_http_client_handle_t h) { h->state = HTTP_STATE_INIT; return ESP_OK; }
esp_err_t esp_http_client_set_url(esp_http_client_handle_t h, const char *url) { h->cfg.url = url; return ESP_OK; }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t h, int ms)
{
    assert(ms > 0); h->cfg.timeout_ms = ms; return ESP_OK;
}
esp_err_t esp_http_client_set_method(esp_http_client_handle_t h, esp_http_client_method_t m)
{
    h->cfg.method = m; return ESP_OK;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h, const char *key, const char *value)
{
    if (!strcmp(key, "Authorization")) snprintf(h->auth, sizeof(h->auth), "%s", value);
    if (!strcmp(key, "Content-Type")) snprintf(h->content_type, sizeof(h->content_type), "%s", value);
    if (!strcmp(key, "Mcp-Session-Id")) snprintf(h->session, sizeof(h->session), "%s", value);
    return ESP_OK;
}
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t h, const char *key)
{
    if (!strcmp(key, "Content-Type")) h->content_type[0] = 0;
    return ESP_OK;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t h, const char *body, int len)
{
    assert(len == (body ? (int)strlen(body) : 0)); h->post = body; return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t h)
{
    perform_count++;
    primary_id = h->id;
    if (fixture.check_session && strstr(h->post, "notifications/initialized"))
        assert(!strcmp(h->session, "test-session"));
    h->status = fixture.status;
    h->tls_flags = fixture.tls_flags;
    h->state = fixture.perform_err == ESP_ERR_HTTP_CONNECT
        ? HTTP_STATE_INIT : HTTP_STATE_RES_ON_DATA_START;
    if (h->state == HTTP_STATE_INIT) return fixture.perform_err;
    if (fixture.status == 401) {
        // Model v6.0 add_auth: missing/unsupported challenges stop perform()
        // after headers, even if some body bytes arrived in that same packet.
        emit(h, 0, strlen(fixture.response) > 3 ? 3 : strlen(fixture.response));
        if (fixture.perform_err == ESP_FAIL) return ESP_FAIL;
        if (fixture.perform_err != ESP_OK) return fixture.perform_err;
        assert(!fixture.challenge || !strncmp(fixture.challenge, "Bearer", 6));
        return ESP_ERR_NOT_SUPPORTED;
    }
    emit(h, 0, strlen(fixture.response));
    return fixture.perform_err;
}
int esp_http_client_get_status_code(esp_http_client_handle_t h) { return h->status; }
esp_http_state_t esp_http_client_get_state(esp_http_client_handle_t h) { return h->state; }
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t h, int *code, int *flags)
{
    *code = 0; *flags = h ? h->tls_flags : 0;
    if (h) h->tls_flags = 0;
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t h, int len)
{
    open_count++;
    assert(h->id != primary_id);   // no uncached perform() response buffer reuse
    assert(h->cfg.method == HTTP_METHOD_POST);
    assert(len == (int)strlen(h->post));
    if (strstr(h->cfg.url, "/token"))
        assert(!strcmp(h->content_type, "application/x-www-form-urlencoded"));
    else
        assert(!strcmp(h->auth, "Bearer test-access"));
    return fixture.open_err;
}
int esp_http_client_write(esp_http_client_handle_t h, const char *body, int len)
{
    assert(!strcmp(body, h->post));
    return fixture.short_write ? len - 1 : len;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h)
{
    if (fixture.bad_headers) return -1;
    h->status = fixture.replay_status;
    h->state = HTTP_STATE_RES_ON_DATA_START;
    esp_http_client_event_t header = {
        .event_id = HTTP_EVENT_ON_HEADER,
        .header_key = "Mcp-Session-Id", .header_value = "test-session",
    };
    h->cfg.event_handler(&header);
    h->emitted = strlen(fixture.response) > 5 ? 5 : strlen(fixture.response);
    emit(h, 0, h->emitted);   // native API caches these bytes as well as emitting them
    return strlen(fixture.response);
}
int esp_http_client_read(esp_http_client_handle_t h, char *buf, int len)
{
    if (fixture.read_failure == -1) return -1;
    if (fixture.read_failure == 1 && h->read_pos >= 5) return 0;
    if (fixture.read_failure == 2) now += 16000000;
    size_t left = strlen(fixture.response) - h->read_pos;
    size_t n = left < (size_t)len ? left : (size_t)len;
    if (n > 5 && strlen(fixture.response) < 100) n = 5;
    memcpy(buf, fixture.response + h->read_pos, n);
    if (h->read_pos + n > h->emitted) {
        size_t start = h->read_pos > h->emitted ? h->read_pos : h->emitted;
        emit(h, start, h->read_pos + n - start);
        h->emitted = h->read_pos + n;
    }
    h->read_pos += n;
    return n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h)
{
    return h->read_pos == strlen(fixture.response);
}

int64_t esp_timer_get_time(void) { return now; }
void vTaskDelay(int ms) { now += (int64_t)ms * 1000; }
uint32_t esp_get_free_heap_size(void) { return 100000; }
const char *esp_err_to_name(esp_err_t e) { return e == ESP_OK ? "ESP_OK" : "mock transport failure"; }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { *h = 1; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    const char *value = !strcmp(key, "access") ? "test-access" : "test-refresh";
    if (*len <= strlen(value)) return ESP_FAIL;
    if (out) strcpy(out, value);
    *len = strlen(value) + 1;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value) { saved_tokens++; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }
void nvs_close(nvs_handle_t h) {}
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) { abort(); }
esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t value) { abort(); }
esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *value) { abort(); }
void esp_fill_random(void *out, size_t len) { abort(); }
const void *mbedtls_md_info_from_type(int type) { abort(); }
int mbedtls_md(const void *info, const unsigned char *in, size_t len, unsigned char *out) { abort(); }
int mbedtls_base64_encode(unsigned char *out, size_t cap, size_t *len,
                         const unsigned char *in, size_t size) { abort(); }
size_t strlcpy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (cap) { size_t copy = n < cap - 1 ? n : cap - 1; memcpy(dst, src, copy); dst[copy] = 0; }
    return n;
}
size_t strlcat(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(dst); return n + strlcpy(dst + n, src, cap - n);
}

static void reset(int status, const char *response)
{
    dial_oauth_release_connection();
    dial_mcp_release_connection();
    memset(&fixture, 0, sizeof(fixture));
    fixture.status = fixture.replay_status = status;
    fixture.response = response;
    perform_count = open_count = init_count = primary_id = saved_tokens = 0;
    now = 0;
}
static bool refresh(void)
{
    oauth_disc_t disc = { .token_endpoint = "https://test.invalid/token", .resource = "test" };
    return dial_oauth_refresh(&disc, "test-client");
}

int main(void)
{
    struct mock_http stale = { .state = HTTP_STATE_INIT, .status = 401 };
    assert(!dial_http_auth_check_failed(&stale, ESP_FAIL));
    assert(!dial_http_auth_check_failed(&stale, ESP_ERR_NOT_SUPPORTED));
    const char *invalid = "{\"error\":\"invalid_grant\"}";
    const char *challenges[] = { NULL, "Bearer realm=\"test\"" };
    for (size_t i = 0; i < 2; i++) {
        reset(401, invalid);
        fixture.challenge = challenges[i];
        assert(!refresh());
        assert(dial_oauth_last_token_err_permanent());
        assert(!strcmp(dial_oauth_last_error(), "HTTP 401\n{\"error\":\"invalid_grant\"}"));
        assert(perform_count == 1 && open_count == 1 && init_count == 2);

        reset(401, "{\"error\":\"unauthorized\"}");
        fixture.challenge = challenges[i];
        assert(!dial_mcp_connect(NULL));
        assert(!strcmp(dial_mcp_last_error(), "initialize: HTTP 401\n{\"error\":\"unauthorized\"}"));
        assert(perform_count == 1 && open_count == 1);
    }
    reset(401, invalid);
    fixture.perform_err = ESP_FAIL;   // disabled/exhausted IDF authorization retries
    assert(!refresh() && dial_oauth_last_token_err_permanent());
    assert(open_count == 1);

    const char *transient_bodies[] = {
        "{\"error\":\"invalid_client\"}", "not JSON", "{\"error\":1}", ""
    };
    for (size_t i = 0; i < 4; i++) {
        reset(401, transient_bodies[i]);
        assert(!refresh() && !dial_oauth_last_token_err_permanent());
    }
    int statuses[] = { 400, 403, 408, 429, 500, 503 };
    for (size_t i = 0; i < 6; i++) {
        reset(statuses[i], invalid);
        assert(!refresh());
        assert(dial_oauth_last_token_err_permanent() == (statuses[i] == 400));
        assert(perform_count == 1 && open_count == 0);
    }
    // A stale 401 status alongside a network/TLS failure must not trigger replay.
    reset(401, invalid);
    fixture.perform_err = ESP_ERR_HTTP_CONNECT;
    fixture.tls_flags = 0x10;
    assert(!refresh() && !dial_oauth_last_token_err_permanent());
    assert(dial_oauth_last_err_cert() && perform_count == 2 && open_count == 0);
    assert(!dial_mcp_connect(NULL) && dial_mcp_last_err_cert());
    assert(open_count == 0);
    reset(401, invalid);
    fixture.perform_err = ESP_ERR_HTTP_READ_TIMEOUT;
    assert(!refresh() && !dial_oauth_last_token_err_permanent());
    assert(perform_count == 2 && open_count == 0);
    reset(200, invalid);
    fixture.perform_err = ESP_ERR_NOT_SUPPORTED;
    assert(!refresh() && !dial_oauth_last_token_err_permanent());
    assert(perform_count == 2 && open_count == 0);

    for (int failure = -1; failure <= 2; failure++) {
        if (!failure) continue;
        reset(401, invalid);
        fixture.read_failure = failure;
        assert(!refresh() && !dial_oauth_last_token_err_permanent());
        assert(perform_count == 1 && open_count == 1);
        assert(strstr(dial_oauth_last_error(), "HTTP -1"));
    }
    for (int failure = 0; failure < 3; failure++) {
        reset(401, invalid);
        fixture.open_err = failure == 0 ? ESP_ERR_HTTP_CONNECT : ESP_OK;
        fixture.short_write = failure == 1;
        fixture.bad_headers = failure == 2;
        assert(!refresh() && !dial_oauth_last_token_err_permanent());
        assert(perform_count == 1 && open_count == 1);
    }
    reset(401, invalid);
    fixture.read_failure = 1;
    assert(!dial_mcp_connect(NULL));
    assert(perform_count == 1 && open_count == 1);
    assert(!strstr(dial_mcp_last_error(), "HTTP 401"));
    char *large = malloc(16 * 1024 + 2);
    memset(large, 'x', 16 * 1024 + 1);
    large[16 * 1024] = 0;
    reset(401, large);
    assert(!refresh() && !dial_oauth_last_token_err_permanent());
    assert(strstr(dial_oauth_last_error(), "HTTP 401"));
    large[16 * 1024] = 'x'; large[16 * 1024 + 1] = 0;
    reset(401, large);
    assert(!refresh() && !dial_oauth_last_token_err_permanent());
    assert(perform_count == 1 && open_count == 1);
    assert(strstr(dial_oauth_last_error(), "HTTP -1"));
    free(large);

    // A fresh replay can succeed; subsequent normal requests keep that handle.
    reset(401, "{\"access_token\":\"test-new-access\"}");
    fixture.replay_status = 200;
    assert(refresh() && !dial_oauth_last_token_err_permanent() && saved_tokens > 0);
    fixture.status = 200;
    assert(refresh() && init_count == 2 && open_count == 1);
    reset(200, "{\"access_token\":\"test-new-access\"}");
    assert(refresh() && refresh());
    assert(init_count == 1 && perform_count == 2 && open_count == 0);
    reset(200, "{\"result\":{\"serverInfo\":{\"name\":\"test\",\"version\":\"1\"}}}");
    assert(dial_mcp_connect(NULL) && dial_mcp_connect(NULL));
    assert(init_count == 1 && perform_count == 4 && open_count == 0);
    reset(401, "{\"result\":{\"serverInfo\":{\"name\":\"test\",\"version\":\"1\"}}}");
    fixture.replay_status = 200;
    fixture.check_session = true;
    assert(dial_mcp_connect(NULL));
    assert(open_count == 2);   // initialize and its notification each get at most one replay
    reset(200, "");
    puts("all authentication-response assertions passed");
    return 0;
}
