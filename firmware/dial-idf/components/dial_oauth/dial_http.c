#include "dial_http.h"

#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"

#define AUTH_RESPONSE_LIMIT (16 * 1024)

static esp_err_t set_deadline_timeout(esp_http_client_handle_t client, int64_t deadline)
{
    int64_t ms = (deadline - esp_timer_get_time()) / 1000;
    if (ms <= 0) return ESP_ERR_HTTP_READ_TIMEOUT;
    return esp_http_client_set_timeout_ms(client, (int)ms);
}

bool dial_http_auth_check_failed(esp_http_client_handle_t client, esp_err_t err)
{
    return client && (err == ESP_ERR_NOT_SUPPORTED || err == ESP_FAIL)
        && esp_http_client_get_state(client) == HTTP_STATE_RES_ON_DATA_START
        && esp_http_client_get_status_code(client) == 401;
}

esp_err_t dial_http_retry_unauthorized(esp_http_client_handle_t client,
                                      const char *body, char **body_out,
                                      int timeout_ms)
{
    *body_out = NULL;
    // A confirmed 401 explicitly rejects authorization; unlike an ambiguous
    // transport failure, it is safe to replay the denied request once.
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    int body_len = body ? strlen(body) : 0;
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) goto done;
    err = set_deadline_timeout(client, deadline);
    if (err != ESP_OK) goto done;
    if (body_len && esp_http_client_write(client, body, body_len) != body_len) {
        err = ESP_ERR_HTTP_WRITE_DATA;
        goto done;
    }
    // The explicit streaming API does not run IDF's Basic/Digest auth check.
    err = set_deadline_timeout(client, deadline);
    if (err != ESP_OK) goto done;
    if (esp_http_client_fetch_headers(client) < 0) {
        err = ESP_ERR_HTTP_FETCH_HEADER;
        goto done;
    }

    char byte;
    size_t len = 0, cap = 0;
    for (;;) {
        err = set_deadline_timeout(client, deadline);
        if (err != ESP_OK) break;
        // IDF read() tries to fill its entire buffer. One byte prevents a
        // trickling body from extending a single read past our deadline.
        int n = esp_http_client_read(client, &byte, 1);
        if (n < 0) {
            err = ESP_ERR_HTTP_INCOMPLETE_DATA;
            break;
        }
        if (n == 0) {
            err = esp_http_client_is_complete_data_received(client)
                ? ESP_OK : ESP_ERR_HTTP_INCOMPLETE_DATA;
            break;
        }
        if (len + n > AUTH_RESPONSE_LIMIT) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        if (len + n + 1 > cap) {
            size_t next_cap = cap ? cap * 2 : 512;
            if (next_cap > AUTH_RESPONSE_LIMIT + 1) next_cap = AUTH_RESPONSE_LIMIT + 1;
            char *next = realloc(*body_out, next_cap);
            if (!next) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            *body_out = next;
            cap = next_cap;
        }
        (*body_out)[len] = byte;
        len += n;
        (*body_out)[len] = 0;
    }

done:
    // Native streaming does not perform the usual keep-alive state reset.
    // Only this already-failed auth path loses its connection.
    {
        esp_err_t close_err = esp_http_client_close(client);
        if (err == ESP_OK) err = close_err;
    }
    if (err != ESP_OK) {
        free(*body_out);
        *body_out = NULL;
    }
    return err;
}
