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
#include <limits.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_timer.h"
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
static TaskHandle_t   s_dns_task;
static volatile bool s_got_creds;

// Post-boot reconnect: once a connection has been established, a drop retries
// forever with capped backoff (the initial-connect path keeps its bounded
// retries so the setup portal can still take over on bad creds).
static bool s_ever_connected;
static int  s_backoff_s = 1;
static esp_timer_handle_t s_retry_timer;
static dial_net_event_cb_t s_event_cb;

static void emit(dial_net_event_t ev) { if (s_event_cb) s_event_cb(ev); }
void dial_net_on_event(dial_net_event_cb_t cb) { s_event_cb = cb; }

// Reconnect indirection: the esp_timer task is shared with lv_tick_inc and
// the knob decoder, so the timer callback must not call esp_wifi_connect()
// (a Wi-Fi control call with no bounded-latency guarantee). It only posts a
// custom event; the actual connect runs on the event-loop task, the same
// context every stock IDF example connects from.
ESP_EVENT_DEFINE_BASE(DIAL_NET_EVENT);
enum { DIAL_NET_RETRY_CONNECT };

static void retry_timer_cb(void *arg)
{
    (void)arg;
    esp_event_post(DIAL_NET_EVENT, DIAL_NET_RETRY_CONNECT, NULL, 0, 0);
}

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

void dial_net_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "pass");
    nvs_commit(h);
    nvs_close(h);
}

// "Change network" (Settings/Wi-Fi): erasing the credentials is NOT enough on
// its own — the dev seed below would re-inject the compiled-in secrets.h
// network on the very next boot and the dial would silently rejoin the same
// Wi-Fi, never reaching the portal. This flag survives the reboot, suppresses
// the seed, and forces bringup into the portal regardless of what NVS holds.
// Cleared only once new credentials actually connect.
void dial_net_request_setup(void)
{
    dial_net_forget();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "setup", 1);
    nvs_commit(h);
    nvs_close(h);
}

bool dial_net_setup_requested(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    bool set = (nvs_get_u8(h, "setup", &v) == ESP_OK) && v;
    nvs_close(h);
    return set;
}

static void setup_request_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "setup");
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
    if (base == DIAL_NET_EVENT && id == DIAL_NET_RETRY_CONNECT) {
        if (!s_connected) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        // sta_connect() issues the connect itself; connecting here as well
        // caused the benign-but-noisy "sta is connecting" error at boot.
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool was_connected = s_connected;
        s_connected = false;
        if (s_ever_connected) {
            // Established link dropped: retry forever with capped backoff.
            if (was_connected) {
                ESP_LOGW(TAG, "Wi-Fi lost — reconnecting");
                s_backoff_s = 1;
                emit(DIAL_NET_EV_LOST);
            }
            esp_timer_start_once(s_retry_timer, (uint64_t)s_backoff_s * 1000000);
            if (s_backoff_s < 30) s_backoff_s *= 2;
        } else if (s_retries < 5) {
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
        s_backoff_s = 1;
        s_connected = true;
        s_ever_connected = true;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        emit(DIAL_NET_EV_GOT_IP);
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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(DIAL_NET_EVENT, DIAL_NET_RETRY_CONNECT, on_event, NULL, NULL));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "OrionDial-%02X%02X", mac[4], mac[5]);

    const esp_timer_create_args_t rt = { .callback = retry_timer_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&rt, &s_retry_timer));
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

// Seed NVS from a dev secrets value if not already provisioned. Non-placeholder
// only, and never when the user has asked for the setup portal (see
// dial_net_request_setup) — otherwise "Change network" would just rejoin the
// build's own network.
void dial_net_seed(const char *ssid, const char *pass)
{
    if (dial_net_setup_requested()) return;
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

/*
 * The network list is scanned ONCE, before any phone is attached, and cached.
 *
 * It used to be scanned inside root_get — a blocking, full-band scan on every
 * request. To scan, the radio has to leave the AP's channel, which is the very
 * channel the phone asking for the page is associated on: the access point
 * effectively went off the air for seconds at a time, while the browser sat
 * there waiting. And phones fire several captive-portal probes at "/" before
 * the real page load, so each one kicked off another scan. That is why joining
 * the setup network and loading its page both took forever.
 */
#define SCAN_MAX 20
static char s_scan_ssid[SCAN_MAX][33];
static int  s_scan_n;
static uint8_t s_ap_channel = 1;   // chosen from the scan; see pick_channel()

// Park the AP on the emptiest of the three non-overlapping channels. A crowded
// channel is a real source of "failed to join": association frames get lost in
// the noise and the phone gives up long before the dial has done anything
// wrong. The scan is already in hand, so this costs nothing.
static void pick_channel(const wifi_ap_record_t *recs, int n)
{
    int load[12] = { 0 };
    for (int i = 0; i < n; i++) {
        int ch = recs[i].primary;
        if (ch >= 1 && ch <= 11) {
            load[ch] += 2;                       // its own channel
            for (int d = 1; d <= 2; d++) {       // and its skirts, which still collide
                if (ch - d >= 1)  load[ch - d]++;
                if (ch + d <= 11) load[ch + d]++;
            }
        }
    }
    const uint8_t cands[] = { 1, 6, 11 };
    uint8_t best = 1;
    int best_load = INT_MAX;
    for (size_t i = 0; i < sizeof(cands); i++) {
        if (load[cands[i]] < best_load) { best_load = load[cands[i]]; best = cands[i]; }
    }
    s_ap_channel = best;
    ESP_LOGI(TAG, "portal: AP on channel %d (load %d)", best, best_load);
}

static void scan_cache_refresh(void)
{
    wifi_scan_config_t scan = {
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        // Explicit dwell times: the defaults are generous, and this runs while
        // the user is standing there waiting for a page.
        .scan_time.active = { .min = 40, .max = 90 },
    };
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) return;

    uint16_t n = SCAN_MAX;
    wifi_ap_record_t recs[SCAN_MAX];
    if (esp_wifi_scan_get_ap_records(&n, recs) != ESP_OK) return;

    pick_channel(recs, n);

    s_scan_n = 0;
    for (int i = 0; i < n && s_scan_n < SCAN_MAX; i++) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;                       // one row per network, not per band/AP
        for (int j = 0; j < s_scan_n; j++)
            if (strcmp(s_scan_ssid[j], (const char *)recs[i].ssid) == 0) { dup = true; break; }
        if (!dup) strlcpy(s_scan_ssid[s_scan_n++], (const char *)recs[i].ssid, sizeof(s_scan_ssid[0]));
    }
    ESP_LOGI(TAG, "portal: %d networks in range", s_scan_n);
}

// User-initiated rescan. This DOES bump the phone off-channel for a moment, but
// it's a deliberate tap with a page waiting for it, not a hidden cost on every
// request — and it's the way out if a network wasn't up during the first scan.
static esp_err_t rescan_get(httpd_req_t *req)
{
    scan_cache_refresh();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "rescanning");
    return ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{

    /*
     * ONE question, one answer. The list and a free-text box side by side left
     * it ambiguous which won if you filled in both — so the text box is not a
     * second field any more: it only exists once you pick "My network isn't
     * listed" from the same dropdown, and then it IS the answer.
     */
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Orion Dial setup</title><style>"
        "body{font-family:-apple-system,system-ui,sans-serif;max-width:420px;margin:24px auto;padding:0 16px;color:#1a1a1a}"
        "h2{color:#0b6}label{display:block;margin-top:14px;font-size:14px;font-weight:600}"
        "input,select,button{width:100%;padding:12px;margin:6px 0;font-size:16px;box-sizing:border-box;"
        "border:1px solid #ccc;border-radius:8px}"
        "button{background:#0b6;color:#fff;border:0;margin-top:18px;font-weight:600}"
        ".hint{color:#888;font-size:13px}"
        "#otherwrap{display:none}"
        "</style></head><body>"
        "<h2>Orion Dial Wi-Fi setup</h2><form method=POST action=/save>"
        "<label for=ssid>Network</label>"
        // Without a placeholder the browser silently pre-selects the first
        // network, so someone who goes straight to the password field submits
        // whichever SSID happened to sort first. Make the choice deliberate.
        "<select name=ssid id=ssid required onchange=\"document.getElementById('otherwrap')"
        ".style.display=(this.value=='__other__')?'block':'none'\">"
        "<option value='' disabled selected>Choose a network\xE2\x80\xA6</option>");

    for (int i = 0; i < s_scan_n; i++) {          // cached: no scan on this path
        httpd_resp_sendstr_chunk(req, "<option>");
        httpd_resp_sendstr_chunk(req, s_scan_ssid[i]);
        httpd_resp_sendstr_chunk(req, "</option>");
    }

    httpd_resp_sendstr_chunk(req,
        "<option value='__other__'>My network isn't listed\xE2\x80\xA6</option>"
        "</select>"
        "<div id=otherwrap>"
        "<label for=other>Network name</label>"
        "<input name=other id=other placeholder='Exact name, including capitals'>"
        "</div>"
        // No JS in this webview? Then the box can't be revealed, so show it.
        "<noscript><style>#otherwrap{display:block}</style></noscript>"
        "<label for=pass>Password</label>"
        "<input name=pass id=pass type=password placeholder='Wi-Fi password'>"
        "<button type=submit>Connect</button></form>"
        "<p class=hint>The dial's setup network disappears as soon as it starts connecting \xE2\x80\x94 "
        "that's expected, and your phone will drop back to its usual Wi-Fi.</p>"
        "<p><a href='/rescan' style='color:#0b6;font-size:13px'>Rescan for networks</a></p>"
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
    char other[33] = { 0 };
    char *ss = strstr(body, "ssid=");
    char *oo = strstr(body, "other=");
    char *pp = strstr(body, "pass=");
    if (ss) { ss += 5; char *amp = strchr(ss, '&'); if (amp) *amp = 0;
              strncpy(s_form_ssid, ss, sizeof(s_form_ssid) - 1); url_decode(s_form_ssid); if (amp) *amp = '&'; }
    if (oo) { oo += 6; char *amp = strchr(oo, '&'); if (amp) *amp = 0;
              strncpy(other, oo, sizeof(other) - 1); url_decode(other); if (amp) *amp = '&'; }
    if (pp) { pp += 5; char *amp = strchr(pp, '&'); if (amp) *amp = 0;
              strncpy(s_form_pass, pp, sizeof(s_form_pass) - 1); url_decode(s_form_pass); }

    // One question, one answer: the typed name is only consulted when the list
    // itself said "not listed", so there is never a case where both are filled
    // in and one silently loses.
    const char *problem = NULL;
    if (strcmp(s_form_ssid, "__other__") == 0) {
        if (other[0]) strlcpy(s_form_ssid, other, sizeof(s_form_ssid));
        else          problem = "Type the name of your network.";
    } else if (!s_form_ssid[0]) {
        problem = "Choose your network from the list.";
    }

    if (problem) {
        // Say what's missing and send them back, rather than failing the
        // request and leaving the browser on a blank error page.
        httpd_resp_set_type(req, "text/html");
        httpd_resp_sendstr_chunk(req,
            "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
            "<body style='font-family:-apple-system,system-ui,sans-serif;text-align:center;margin-top:40px'>"
            "<h2>Almost</h2><p>");
        httpd_resp_sendstr_chunk(req, problem);
        httpd_resp_sendstr_chunk(req,
            "</p><p><a href='/' style='color:#0b6'>Back</a></p></body>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

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
    ap.ap.channel = s_ap_channel;   // emptiest of 1/6/11, from the scan
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_uri_handlers = 8;
    hc.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_httpd, &hc));
    httpd_uri_t root   = { .uri = "/", .method = HTTP_GET, .handler = root_get };
    httpd_uri_t save   = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
    httpd_uri_t rescan = { .uri = "/rescan", .method = HTTP_GET, .handler = rescan_get };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);
    httpd_register_uri_handler(s_httpd, &rescan);
    httpd_register_err_handler(s_httpd, HTTPD_404_NOT_FOUND, redirect_404);

    // Only ever one DNS task: portal_start runs again on a failed password, and
    // a second one would just fail to bind port 53 and die.
    if (!s_dns_task) xTaskCreate(dns_hijack_task, "dns", 3072, NULL, 5, &s_dns_task);
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

    // Force the portal only while the setup request is outstanding AND there
    // are no credentials to try. The two conditions are stored in separate NVS
    // commits, so a power cut can land between them; deriving the decision from
    // both (rather than trusting the flag alone) makes every such window
    // self-healing:
    //   flag + no creds  -> the request as intended: run the portal.
    //   flag + creds     -> the portal already saved the network the user just
    //                       chose and we died before clearing the flag. Those
    //                       creds ARE the new ones (request_setup erased the old
    //                       ones first), so connect with them and clear the flag
    //                       below — never re-run setup on top of a good network.
    // The worst remaining outcome is a crash mid-request leaving neither, which
    // just means the change-network tap didn't take; the user taps it again.
    bool want_setup = dial_net_setup_requested() && !dial_net_have_creds();

    // 1) Try stored credentials. Three rounds before falling into the portal:
    //    a router that is still booting after a power blip shouldn't strand the
    //    dial in setup mode.
    if (!want_setup && load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        emit(DIAL_NET_EV_CONNECTING);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        for (int round = 0; round < 3; round++) {
            if (sta_connect(ssid, pass, 20000)) {
                setup_request_clear();   // no-op unless we're healing the window above
                return;
            }
            ESP_LOGW(TAG, "stored creds attempt %d/3 failed", round + 1);
        }
        ESP_LOGW(TAG, "stored creds failed — starting setup portal");
        esp_wifi_stop();
    }

    /*
     * 2) Run the captive portal until we get creds that connect.
     *
     * ORDER MATTERS, and getting it wrong is what made this flaky. The QR on
     * screen is an invitation to join a network — so that network, its DHCP
     * server and its web server all have to be up and stable BEFORE the QR is
     * shown. Previously the QR went up first, and the radio then spent a second
     * or two configuring the AP and running an off-channel scan: a phone that
     * scanned the code promptly tried to associate with an AP that wasn't
     * listening yet, got "failed to join", and iOS then backed off for 10-20
     * seconds before it would even try again.
     *
     * So: scan first (no AP yet — nothing to disturb), then bring the AP and
     * the servers up, and only then invite anyone.
     */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());
    scan_cache_refresh();
    portal_start();
    emit(DIAL_NET_EV_PORTAL);      // the QR goes up last: the network exists now

    for (;;) {
        s_got_creds = false;
        while (!s_got_creds)
            vTaskDelay(pdMS_TO_TICKS(100));

        // Flush the "Connecting…" page, then take the portal DOWN before
        // dialling the real network — not after it succeeds.
        //
        // The phone's captive-portal sheet stays open until this AP goes away,
        // so leaving it up for the whole join + DHCP was most of the wait the
        // user actually sat through. And it bought nothing: in APSTA the AP is
        // dragged onto the station's channel the instant it associates, which
        // knocks the phone off anyway. Dropping it first is both faster and
        // cleaner.
        vTaskDelay(pdMS_TO_TICKS(300));
        portal_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        emit(DIAL_NET_EV_CONNECTING);   // the dial stops showing a QR for a network it just took down

        if (sta_connect(s_form_ssid, s_form_pass, 20000)) {
            save_creds(s_form_ssid, s_form_pass);
            setup_request_clear();   // provisioned: stop forcing the portal
            ESP_LOGI(TAG, "provisioned + connected");
            return;
        }

        // Wrong password, or the network wasn't there. Put the portal back and
        // let them try again: phones re-join an open AP they've just used on
        // their own, and re-show the page with it.
        ESP_LOGW(TAG, "those creds didn't connect — reopening the portal");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        portal_start();
        emit(DIAL_NET_EV_PORTAL);       // again: the QR only after the AP is back
    }
}
