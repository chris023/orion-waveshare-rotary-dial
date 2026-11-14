#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_HTTP_CONNECT 0x7002
#define ESP_ERR_HTTP_WRITE_DATA 0x7003
#define ESP_ERR_HTTP_FETCH_HEADER 0x7004
#define ESP_ERR_HTTP_INCOMPLETE_DATA 0x7009
#define ESP_ERR_HTTP_READ_TIMEOUT 0x700a

typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST } esp_http_client_method_t;
typedef enum {
    HTTP_STATE_INIT, HTTP_STATE_CONNECTED, HTTP_STATE_RES_ON_DATA_START
} esp_http_state_t;
typedef enum {
    HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA
} esp_http_client_event_id_t;
typedef struct mock_http *esp_http_client_handle_t;
typedef struct {
    esp_http_client_event_id_t event_id;
    char *header_key, *header_value;
    void *data;
    int data_len;
} esp_http_client_event_t;
typedef struct {
    const char *url, *cert_pem;
    esp_http_client_method_t method;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
    int timeout_ms;
    bool keep_alive_enable;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t);
esp_err_t esp_http_client_close(esp_http_client_handle_t);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t, const char *);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t, int);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t, esp_http_client_method_t);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t, const char *);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t, const char *, int);
esp_err_t esp_http_client_perform(esp_http_client_handle_t);
int esp_http_client_get_status_code(esp_http_client_handle_t);
esp_http_state_t esp_http_client_get_state(esp_http_client_handle_t);
esp_err_t esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t, int *, int *);
esp_err_t esp_http_client_open(esp_http_client_handle_t, int);
int esp_http_client_write(esp_http_client_handle_t, const char *, int);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t);
int esp_http_client_read(esp_http_client_handle_t, char *, int);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t);
