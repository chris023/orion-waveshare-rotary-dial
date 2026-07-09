/*
 * Wi-Fi for the Orion dial.
 *
 *  - Credentials persist in NVS (namespace "wifi", keys "ssid"/"pass").
 *  - dial_net_bringup() connects with stored creds; if none (or connecting
 *    fails), it runs a SoftAP captive portal: the dial hosts an open AP
 *    "OrionDial-XXXX" + a DNS hijack (so phones auto-pop the portal) + an HTTP
 *    form listing nearby networks. On submit it saves creds and connects.
 *  - An optional dev seed (from secrets.h via dial_net_init) pre-fills NVS so
 *    development can skip the portal.
 */

#include "dial_wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "lwip/sockets.h"

static const char *TAG = "net";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define NVS_NS   "wifi"

static EventGroupHandle_t s_events;
static volatile bool s_connected;
static int s_retries;
static char s_ap_ssid[16];
static esp_netif_t *s_sta_netif, *s_ap_netif;
static httpd_handle_t s_httpd;
static volatile bool s_got_creds;

/* ---- credential storage ---------------------------------------------- */

bool dial_net_have_creds(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    size_t len = 0;
    bool have = (nvs_get_str(h, "ssid", NULL, &len) == ESP_OK) && len > 1;
    nvs_close(h);
    return have;
}

static void save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass ? pass : "");
    nvs_commit(h);
    nvs_close(h);
}

static bool load_creds(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_str(h, "ssid", ssid, &ssid_sz) == ESP_OK);
    if (ok && nvs_get_str(h, "pass", pass, &pass_sz) != ESP_OK)
        pass[0] = 0;
    nvs_close(h);
    return ok && ssid[0];
}

/* ---- wifi events ----------------------------------------------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retries < 5) {
            s_retries++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got IP " IPSTR ", gw " IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.gw));
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
            ESP_LOGI(TAG, "DNS server " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        s_retries = 0;
        s_connected = true;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

/* ---- init ------------------------------------------------------------ */

void dial_net_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "OrionDial-%02X%02X", mac[4], mac[5]);
}

const char *dial_net_ap_ssid(void) { return s_ap_ssid; }
bool dial_wifi_is_connected(void) { return s_connected; }

bool dial_net_ip(char *out, size_t sz)
{
    esp_netif_ip_info_t ip;
    if (!s_connected || !s_sta_netif || esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK)
        return false;
    snprintf(out, sz, IPSTR, IP2STR(&ip.ip));
    return true;
}

// Seed NVS from a dev secrets value if not already provisioned. Non-placeholder only.
void dial_net_seed(const char *ssid, const char *pass)
{
    if (ssid && ssid[0] && strcmp(ssid, "your-wifi-ssid") != 0 && !dial_net_have_creds()) {
        ESP_LOGI(TAG, "seeding Wi-Fi creds from dev secrets");
        save_creds(ssid, pass);
    }
}

/* ---- STA connect ----------------------------------------------------- */

static bool sta_connect(const char *ssid, const char *pass, int timeout_ms)
{
    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);

    s_retries = 0;
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_LOGI(TAG, "connecting to \"%s\"", ssid);
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ---- captive portal: DNS hijack -------------------------------------- */

// Minimal DNS server: answer every A query with the AP's IP (192.168.4.1) so
// the phone's captive-portal check resolves to our page and auto-launches it.
static void dns_hijack_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (sock < 0 || bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        ESP_LOGW(TAG, "DNS bind failed");
        if (sock >= 0) close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 12) continue;
        buf[2] |= 0x80; buf[3] = 0x80;               // flags: response, no error
        buf[6] = 0; buf[7] = 1;                        // answer count = 1
        buf[8] = buf[9] = buf[10] = buf[11] = 0;       // NS/AR = 0
        if (n + 16 > (int)sizeof(buf)) continue;
        uint8_t *p = buf + n;
        *p++ = 0xC0; *p++ = 0x0C;                      // name pointer to question
        *p++ = 0x00; *p++ = 0x01;                      // type A
        *p++ = 0x00; *p++ = 0x01;                      // class IN
        *p++ = 0; *p++ = 0; *p++ = 0; *p++ = 60;       // TTL 60
        *p++ = 0x00; *p++ = 0x04;                      // rdlength 4
        *p++ = 192; *p++ = 168; *p++ = 4; *p++ = 1;    // 192.168.4.1
        sendto(sock, buf, p - buf, 0, (struct sockaddr *)&from, fl);
    }
}

/* ---- captive portal: HTTP form --------------------------------------- */

static char s_form_ssid[33];
static char s_form_pass[65];

// URL-decode application/x-www-form-urlencoded in place.
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

static esp_err_t root_get(httpd_req_t *req)
{
    // Scan for nearby networks to populate the dropdown.
    wifi_scan_config_t scan = { .show_hidden = false };
    esp_wifi_scan_start(&scan, true);
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) n = 20;
    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Orion Dial setup</title><style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
        "h2{color:#0b6}input,select,button{width:100%;padding:12px;margin:6px 0;font-size:16px;box-sizing:border-box}"
        "button{background:#0b6;color:#fff;border:0;border-radius:8px}</style></head><body>"
        "<h2>Orion Dial Wi-Fi setup</h2><form method=POST action=/save>"
        "<label>Network</label><select name=ssid>");
    for (int i = 0; i < n; i++) {
        httpd_resp_sendstr_chunk(req, "<option>");
        httpd_resp_sendstr_chunk(req, (char *)recs[i].ssid);
        httpd_resp_sendstr_chunk(req, "</option>");
    }
    httpd_resp_sendstr_chunk(req,
        "</select><label>Password</label><input name=pass type=password placeholder='Wi-Fi password'>"
        "<button type=submit>Connect</button></form>"
        "<p style='color:#888;font-size:13px'>Pick your network, enter its password, and the dial will join it.</p>"
        "</body></html>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[256];
    int len = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int got = httpd_req_recv(req, body, len);
    if (got <= 0) return ESP_FAIL;
    body[got] = 0;

    s_form_ssid[0] = s_form_pass[0] = 0;
    char *ss = strstr(body, "ssid=");
    char *pp = strstr(body, "pass=");
    if (ss) { ss += 5; char *amp = strchr(ss, '&'); if (amp) *amp = 0;
              strncpy(s_form_ssid, ss, sizeof(s_form_ssid) - 1); url_decode(s_form_ssid); if (amp) *amp = '&'; }
    if (pp) { pp += 5; char *amp = strchr(pp, '&'); if (amp) *amp = 0;
              strncpy(s_form_pass, pp, sizeof(s_form_pass) - 1); url_decode(s_form_pass); }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:sans-serif;text-align:center;margin-top:40px'>"
        "<h2 style='color:#0b6'>Connecting…</h2><p>The dial is joining your network. "
        "You can close this page.</p></body>");
    ESP_LOGI(TAG, "portal submitted SSID \"%s\"", s_form_ssid);
    s_got_creds = true;
    return ESP_OK;
}

// Redirect any other request to the portal root (captive-portal detection).
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "redirect");
    return ESP_OK;
}

static void portal_start(void)
{
    // AP config: open network named OrionDial-XXXX.
    wifi_config_t ap = { 0 };
    strncpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 8;
    hc.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &hc));
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);

    xTaskCreate(dns_hijack_task, "dns", 3072, NULL, 5, NULL);
    ESP_LOGI(TAG, "captive portal up — join Wi-Fi \"%s\" then open any page", s_ap_ssid);
}

static void portal_stop(void)
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = NULL; }
}

/* ---- orchestration --------------------------------------------------- */

void dial_net_bringup(void)
{
    char ssid[33], pass[65];

    // 1) Try stored credentials.
    if (load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        if (sta_connect(ssid, pass, 20000))
            return;
        ESP_LOGW(TAG, "stored creds failed — starting setup portal");
        esp_wifi_stop();
    }

    // 2) Run the captive portal until we get creds that connect.
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());
    portal_start();
    for (;;) {
        s_got_creds = false;
        while (!s_got_creds)
            vTaskDelay(pdMS_TO_TICKS(100));
        // Give the "Connecting…" page a moment to flush, then try.
        vTaskDelay(pdMS_TO_TICKS(500));
        if (sta_connect(s_form_ssid, s_form_pass, 20000)) {
            save_creds(s_form_ssid, s_form_pass);
            portal_stop();
            esp_wifi_set_mode(WIFI_MODE_STA);
            ESP_LOGI(TAG, "provisioned + connected");
            return;
        }
        ESP_LOGW(TAG, "those creds didn't connect — portal still open");
    }
}
