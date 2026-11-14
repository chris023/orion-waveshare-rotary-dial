#pragma once

#include "esp_http_client.h"

// ESP-IDF v6.0 stops perform() before finishing an unsupported 401 challenge.
// Check the state too: a status left over from an earlier call is not enough.
bool dial_http_auth_check_failed(esp_http_client_handle_t client, esp_err_t err);

// Replay one explicitly rejected request on a NEW, configured handle. Suppress
// ON_DATA capture during this call; *body_out is malloc'd and caller-owned.
esp_err_t dial_http_retry_unauthorized(esp_http_client_handle_t client,
                                      const char *body, char **body_out,
                                      int timeout_ms);
