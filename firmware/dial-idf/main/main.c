#include <stdio.h>

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"

#include "lvgl.h"
#include "lv_demos.h"
#include "esp_lcd_sh8601.h"
#include "i2c_bsp.h"
#include "lcd_touch_bsp.h"
#include "user_config.h"
#include "lcd_bl_pwm_bsp.h"
#include "dial_wifi.h"
#include "dial_oauth.h"
#include "dial_mcp.h"
#include "bidi_switch_knob.h"
#include "secrets.h"
#include "cJSON.h"
static const char *TAG = "example";

static bool example_lvgl_lock(int timeout_ms);
static void example_lvgl_unlock(void);

// Full-screen setup screen: instruction label + a QR of the authorize URL.
static void ui_show_qr(const char *title, const char *url)
{
    if (!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0x0b6a4a), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *qr = lv_qrcode_create(scr, 220, lv_color_black(), lv_color_white());
    lv_qrcode_update(qr, url, strlen(url));
    lv_obj_center(qr);
    example_lvgl_unlock();
}

// Centered status message.
static void ui_show_msg(const char *msg)
{
    if (!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_width(label, 300);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, msg);
    lv_obj_set_style_text_color(label, lv_color_hex(0x0b6a4a), 0);
    lv_obj_center(label);
    example_lvgl_unlock();
}

// OAuth: get a usable access token (stored, refreshed, or via on-screen consent).
/* ========================= Orion control ============================== *
 * The rotary encoder lives on the companion MCU, so for now this dial is
 * driven by the working touchscreen: an LVGL arc you drag to set the target
 * temperature and a button to toggle the zone on/off. Touch events post
 * commands to a worker (the oauth_task loop) that owns the MCP session; the
 * worker polls get_device_state and pushes the truth back to the UI.
 */

#define ORION_ZONE  "zone_a"      // this dial controls zone_a (owner side)
#define TEMP_MIN_F  55
#define TEMP_MAX_F  110

typedef enum { CMD_SET_TEMP, CMD_TOGGLE_ON } orion_cmd_kind_t;
typedef struct { orion_cmd_kind_t kind; int temp_f; } orion_cmd_t;

static QueueHandle_t     s_cmd_q;
static SemaphoreHandle_t s_state_mux;
static char  s_serial[16];
static float s_temp_c;       // last known device temp for our zone (°C)
static bool  s_on;
static bool  s_online;
static bool  s_have_state;

static lv_obj_t *s_arc, *s_temp_lbl, *s_state_lbl;

// Rotary encoder on GPIO8 (A) / GPIO7 (B). The encoder is only electrically
// live under the full board init (display+touch up) — bare-GPIO probes read it
// dead. Decoded by Waveshare's bidi_switch_knob (iot_knob), the same decoder the
// stock firmware uses for this exact detented part. Each detent -> a left/right
// event -> a 1°F step on the setpoint.
#define KNOB_A 8
#define KNOB_B 7
static knob_handle_t s_knob;
static volatile int  s_ui_temp_f = -1;   // current shown setpoint (°F); -1 = unknown

// Device resync is gated on a quiet period: every user input stamps this, and
// the poll only reads the bed back once there's been no input for a while (so an
// update can never land mid-interaction). The poll interval is measured from T0
// = the last input, not wall-clock.
static volatile int64_t s_last_input_us;
#define KNOB_SETTLE_US    2500000    // 2.5s of no input before the bed is read back
#define POLL_INTERVAL_US 10000000    // and at most every ~10s when idle

// Orion works in °C; this US user's app shows °F. The device's own conversion
// table is the linear formula rounded to 0.1°C, so linear round-trips exactly.
static inline int   c_to_f(float c) { return (int)lroundf(c * 1.8f + 32.0f); }
static inline float f_to_c(int f)   { return roundf(((f - 32) / 1.8f) * 10.0f) / 10.0f; }

// ---- UI (all callers below hold the LVGL mux where noted) ----

// Update the center readout. Caller must hold the LVGL mux.
static void ui_set_center(int temp_f, bool on, bool online)
{
    char t[8];
    snprintf(t, sizeof(t), "%d", temp_f);
    lv_label_set_text(s_temp_lbl, t);
    lv_label_set_text(s_state_lbl, !online ? "OFFLINE" : (on ? "ON" : "OFF"));
    lv_color_t c = !online ? lv_color_hex(0x666666)
                           : (on ? lv_color_hex(0xff7043) : lv_color_hex(0x5b8def));
    lv_obj_set_style_text_color(s_temp_lbl, c, 0);
    lv_obj_set_style_arc_color(s_arc, c, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc, on ? LV_OPA_COVER : LV_OPA_40, LV_PART_INDICATOR);
}

// LVGL event callbacks run inside lv_timer_handler, which already holds the
// LVGL mux — so they must NOT re-lock. They only touch LVGL + post to the queue.
static void arc_event_cb(lv_event_t *e)
{
    s_last_input_us = esp_timer_get_time();
    int f = lv_arc_get_value(s_arc);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        char t[8];
        snprintf(t, sizeof(t), "%d", f);
        lv_label_set_text(s_temp_lbl, t);          // live feedback while dragging
    } else {                                        // LV_EVENT_RELEASED
        orion_cmd_t cmd = { .kind = CMD_SET_TEMP, .temp_f = f };
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

static void power_event_cb(lv_event_t *e)
{
    s_last_input_us = esp_timer_get_time();
    orion_cmd_t cmd = { .kind = CMD_TOGGLE_ON };
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void ui_build_dial(void)
{
    if (!example_lvgl_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 320, 320);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, TEMP_MIN_F, TEMP_MAX_F);
    lv_arc_set_value(s_arc, (TEMP_MIN_F + TEMP_MAX_F) / 2);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_arc, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *zone = lv_label_create(scr);
    lv_obj_set_style_text_font(zone, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(zone, lv_color_hex(0x888888), 0);
    lv_label_set_text(zone, "MY SIDE");
    lv_obj_align(zone, LV_ALIGN_CENTER, 0, -66);

    s_temp_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_temp_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_temp_lbl, lv_color_hex(0xff7043), 0);
    lv_label_set_text(s_temp_lbl, "--");
    lv_obj_align(s_temp_lbl, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *unit = lv_label_create(scr);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text(unit, "\xC2\xB0" "F");        // UTF-8 degree sign + F
    lv_obj_align_to(unit, s_temp_lbl, LV_ALIGN_OUT_RIGHT_TOP, 4, 8);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 104, 44);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 62);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(btn, power_event_cb, LV_EVENT_CLICKED, NULL);
    s_state_lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_state_lbl, "--");
    lv_obj_center(s_state_lbl);

    example_lvgl_unlock();
}

// Push the shared device state into the UI (called from the worker task).
static void ui_apply_state(void)
{
    xSemaphoreTake(s_state_mux, portMAX_DELAY);
    int  f      = c_to_f(s_temp_c);
    bool on     = s_on;
    bool online = s_online;
    bool have   = s_have_state;
    if (have) s_ui_temp_f = f;     // keep the knob's baseline synced to the device
    xSemaphoreGive(s_state_mux);
    if (!have) return;
    if (!example_lvgl_lock(-1)) return;
    lv_arc_set_value(s_arc, f);
    ui_set_center(f, on, online);
    example_lvgl_unlock();
}

// ---- Rotary knob -> temperature setpoint ----
// A knob detent adjusts the setpoint by 1°F: update UI optimistically and queue
// a set_zone. Runs in the decoder's esp_timer task, so it only touches the
// queue + the two mutexes (never blocks on the network).
static void knob_apply_step(int delta_f)
{
    if (!s_cmd_q) return;
    s_last_input_us = esp_timer_get_time();
    xSemaphoreTake(s_state_mux, portMAX_DELAY);
    int cur = (s_ui_temp_f >= 0) ? s_ui_temp_f : c_to_f(s_temp_c);
    int nf  = cur + delta_f;
    if (nf < TEMP_MIN_F) nf = TEMP_MIN_F;
    if (nf > TEMP_MAX_F) nf = TEMP_MAX_F;
    s_ui_temp_f = nf;
    bool on = s_on;
    xSemaphoreGive(s_state_mux);

    if (example_lvgl_lock(50)) {
        lv_arc_set_value(s_arc, nf);
        ui_set_center(nf, on, true);
        example_lvgl_unlock();
    }
    orion_cmd_t cmd = { .kind = CMD_SET_TEMP, .temp_f = nf };
    xQueueSend(s_cmd_q, &cmd, 0);
}
static void knob_left_cb(void *arg, void *data)  { knob_apply_step(-1); }  // CCW = cooler
static void knob_right_cb(void *arg, void *data) { knob_apply_step(+1); }  // CW  = warmer
static void knob_init(void)
{
    knob_config_t cfg = { .gpio_encoder_a = KNOB_A, .gpio_encoder_b = KNOB_B };
    s_knob = iot_knob_create(&cfg);
    if (!s_knob) { ESP_LOGE(TAG, "knob create failed"); return; }
    iot_knob_register_cb(s_knob, KNOB_LEFT, knob_left_cb, NULL);
    iot_knob_register_cb(s_knob, KNOB_RIGHT, knob_right_cb, NULL);
    ESP_LOGI(TAG, "knob ready on GPIO%d/%d", KNOB_A, KNOB_B);
}

// ---- Orion MCP tool calls (run on the worker task) ----

static bool orion_refresh_state(void)
{
    char args[48];
    snprintf(args, sizeof(args), "{\"serial\":\"%s\"}", s_serial);
    char *json = NULL;
    if (!dial_mcp_call_tool("get_device_state", args, &json) || !json) return false;
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return false;

    bool online = false, on = false, found = false;
    float temp = 0;
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (status) online = cJSON_IsTrue(cJSON_GetObjectItem(status, "online"));
    cJSON *z;
    cJSON_ArrayForEach(z, cJSON_GetObjectItem(root, "zones")) {
        cJSON *id = cJSON_GetObjectItem(z, "id");
        if (id && id->valuestring && strcmp(id->valuestring, ORION_ZONE) == 0) {
            cJSON *t = cJSON_GetObjectItem(z, "temp");
            if (cJSON_IsNumber(t)) temp = (float)t->valuedouble;
            on = cJSON_IsTrue(cJSON_GetObjectItem(z, "on"));
            found = true;
        }
    }
    cJSON_Delete(root);
    if (!found) return false;

    xSemaphoreTake(s_state_mux, portMAX_DELAY);
    s_temp_c = temp; s_on = on; s_online = online; s_have_state = true;
    xSemaphoreGive(s_state_mux);
    return true;
}

static bool orion_set_zone(const char *field_json)
{
    char args[96];
    snprintf(args, sizeof(args), "{\"serial\":\"%s\",\"zone_id\":\"%s\",%s}",
             s_serial, ORION_ZONE, field_json);
    char *r = NULL;
    bool ok = dial_mcp_call_tool("set_zone", args, &r);
    ESP_LOGI(TAG, "set_zone %s -> %s (%s)", field_json, ok ? "ok" : "fail",
             ok ? "" : dial_mcp_last_error());
    free(r);
    return ok;
}

static void oauth_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    char ip[16];
    if (!dial_net_ip(ip, sizeof(ip))) { vTaskDelete(NULL); return; }
    char redirect_uri[48];
    snprintf(redirect_uri, sizeof(redirect_uri), "http://%s/callback", ip);

    oauth_disc_t disc;
    char client_id[96];
    if (!dial_oauth_discover(&disc) ||
        !dial_oauth_ensure_client(&disc, redirect_uri, client_id, sizeof(client_id))) {
        ui_show_msg("Setup error:\nOrion unreachable");
        vTaskDelete(NULL);
        return;
    }

    if (dial_oauth_have_valid_access()) {
        ESP_LOGI(TAG, "using stored access token");
    } else if (dial_oauth_refresh(&disc, client_id)) {
        ESP_LOGI(TAG, "refreshed access token");
    } else {
        char url[600];
        if (dial_oauth_start_authorize(&disc, client_id, redirect_uri, url, sizeof(url))) {
            ESP_LOGI(TAG, "authorize URL: %s", url);
            ui_show_qr("Scan to link your dial", url);
            bool ok = dial_oauth_finish_authorize(&disc, client_id, redirect_uri, 300000);
            dial_oauth_stop_authorize();
            if (ok) {
                ui_show_msg("Dial linked!");
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg), "Link failed:\n%s", dial_oauth_last_error());
                ui_show_msg(msg);
                vTaskDelete(NULL);
                return;
            }
        }
    }

    ui_show_msg("Connecting to Orion...");

    // ---- Open the MCP session ----
    char *server = NULL;
    if (!dial_mcp_connect(&server)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Orion connect failed:\n%s", dial_mcp_last_error());
        ui_show_msg(msg);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "MCP server: %s, %d tools", server ? server : "?",
             dial_mcp_list_tools_count());
    free(server);

    // ---- Find this bed's serial from list_devices ----
    char *devices = NULL;
    if (dial_mcp_call_tool("list_devices", "{}", &devices) && devices) {
        cJSON *root = cJSON_Parse(devices);
        cJSON *arr  = root ? cJSON_GetObjectItem(root, "devices") : NULL;
        cJSON *dev0 = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, 0) : NULL;
        cJSON *serial = dev0 ? cJSON_GetObjectItem(dev0, "serial_number") : NULL;
        if (serial && serial->valuestring)
            strlcpy(s_serial, serial->valuestring, sizeof(s_serial));
        if (root) cJSON_Delete(root);
    }
    free(devices);
    if (!s_serial[0]) {
        ui_show_msg("No Orion device\non this account");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "controlling serial %s zone %s", s_serial, ORION_ZONE);

    // ---- Build the touch dial, start the knob, enter the command + poll loop ----
    s_cmd_q     = xQueueCreate(16, sizeof(orion_cmd_t));
    s_state_mux = xSemaphoreCreateMutex();
    orion_refresh_state();
    ui_build_dial();
    ui_apply_state();
    knob_init();     // safe now: queue + mutex + UI all exist

    int64_t last_poll_us = esp_timer_get_time();
    for (;;) {
        orion_cmd_t cmd;
        // Short tick so the loop stays responsive to commands; the device
        // readback is gated separately on the quiet-period below.
        if (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(300)) == pdTRUE) {
            // Coalesce a burst (e.g. spinning the knob): collapse to at most one
            // net on/off toggle + the final target temperature.
            int  last_temp = -1, toggles = 0;
            do {
                if (cmd.kind == CMD_SET_TEMP) last_temp = cmd.temp_f;
                else toggles++;
            } while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE);

            if (toggles & 1) {
                xSemaphoreTake(s_state_mux, portMAX_DELAY);
                bool cur = s_on;
                xSemaphoreGive(s_state_mux);
                if (orion_set_zone(cur ? "\"on\":false" : "\"on\":true")) {
                    xSemaphoreTake(s_state_mux, portMAX_DELAY);
                    s_on = !cur;                 // optimistic: trust the change
                    xSemaphoreGive(s_state_mux);
                }
            }
            if (last_temp >= 0) {
                char f[24];
                snprintf(f, sizeof(f), "\"temp\":%.1f", f_to_c(last_temp));
                if (orion_set_zone(f)) {
                    xSemaphoreTake(s_state_mux, portMAX_DELAY);
                    s_temp_c = f_to_c(last_temp);  // optimistic: trust the change
                    xSemaphoreGive(s_state_mux);
                }
            }
            // Reflect the (optimistic) local state. Deliberately NO device
            // readback here: the bed lags a beat. Also push the poll's T0 out so
            // the resync waits for the quiet period after this input.
            ui_apply_state();
            last_poll_us = esp_timer_get_time();
            continue;
        }

        // No command this tick. Resync to the device truth only when it's been
        // quiet (no input for KNOB_SETTLE_US) AND a poll interval has elapsed —
        // so an update can never land mid-interaction.
        int64_t now = esp_timer_get_time();
        if (now - s_last_input_us < KNOB_SETTLE_US) continue;
        if (now - last_poll_us < POLL_INTERVAL_US) continue;

        if (!dial_oauth_have_valid_access())
            dial_oauth_refresh(&disc, client_id);
        if (!orion_refresh_state() && dial_oauth_refresh(&disc, client_id)) {
            dial_mcp_connect(NULL);       // token rotated — reopen the session
            orion_refresh_state();
        }
        ui_apply_state();
        last_poll_us = esp_timer_get_time();
    }
}
static SemaphoreHandle_t lvgl_mux = NULL;


#if CONFIG_LV_COLOR_DEPTH == 32
#define LCD_BIT_PER_PIXEL       (24)
#elif CONFIG_LV_COLOR_DEPTH == 16
#define LCD_BIT_PER_PIXEL       (16)
#endif
//d5-d7
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xF0, (uint8_t[]){0x28}, 1, 0},
    {0xF2, (uint8_t[]){0x28}, 1, 0},
    {0x73, (uint8_t[]){0xF0}, 1, 0},
    {0x7C, (uint8_t[]){0xD1}, 1, 0},
    {0x83, (uint8_t[]){0xE0}, 1, 0},
    {0x84, (uint8_t[]){0x61}, 1, 0},
    {0xF2, (uint8_t[]){0x82}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x01}, 1, 0},
    {0xF1, (uint8_t[]){0x01}, 1, 0},
    {0xB0, (uint8_t[]){0x56}, 1, 0},
    {0xB1, (uint8_t[]){0x4D}, 1, 0},
    {0xB2, (uint8_t[]){0x24}, 1, 0},
    {0xB4, (uint8_t[]){0x87}, 1, 0},
    {0xB5, (uint8_t[]){0x44}, 1, 0},
    {0xB6, (uint8_t[]){0x8B}, 1, 0},
    {0xB7, (uint8_t[]){0x40}, 1, 0},
    {0xB8, (uint8_t[]){0x86}, 1, 0},
    {0xBA, (uint8_t[]){0x00}, 1, 0},
    {0xBB, (uint8_t[]){0x08}, 1, 0},
    {0xBC, (uint8_t[]){0x08}, 1, 0},
    {0xBD, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x80}, 1, 0},
    {0xC1, (uint8_t[]){0x10}, 1, 0},
    {0xC2, (uint8_t[]){0x37}, 1, 0},
    {0xC3, (uint8_t[]){0x80}, 1, 0},
    {0xC4, (uint8_t[]){0x10}, 1, 0},
    {0xC5, (uint8_t[]){0x37}, 1, 0},
    {0xC6, (uint8_t[]){0xA9}, 1, 0},
    {0xC7, (uint8_t[]){0x41}, 1, 0},
    {0xC8, (uint8_t[]){0x01}, 1, 0},
    {0xC9, (uint8_t[]){0xA9}, 1, 0},
    {0xCA, (uint8_t[]){0x41}, 1, 0},
    {0xCB, (uint8_t[]){0x01}, 1, 0},
    {0xD0, (uint8_t[]){0x91}, 1, 0},
    {0xD1, (uint8_t[]){0x68}, 1, 0},
    {0xD2, (uint8_t[]){0x68}, 1, 0},
    {0xF5, (uint8_t[]){0x00, 0xA5}, 2, 0},
    {0xDD, (uint8_t[]){0x4F}, 1, 0},
    {0xDE, (uint8_t[]){0x4F}, 1, 0},
    {0xF1, (uint8_t[]){0x10}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0xF0, (uint8_t[]){0x02}, 1, 0},
    {0xE0, (uint8_t[]){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
    {0xE1, (uint8_t[]){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
    {0xF0, (uint8_t[]){0x10}, 1, 0},
    {0xF3, (uint8_t[]){0x10}, 1, 0},
    {0xE0, (uint8_t[]){0x07}, 1, 0},
    {0xE1, (uint8_t[]){0x00}, 1, 0},
    {0xE2, (uint8_t[]){0x00}, 1, 0},
    {0xE3, (uint8_t[]){0x00}, 1, 0},
    {0xE4, (uint8_t[]){0xE0}, 1, 0},
    {0xE5, (uint8_t[]){0x06}, 1, 0},
    {0xE6, (uint8_t[]){0x21}, 1, 0},
    {0xE7, (uint8_t[]){0x01}, 1, 0},
    {0xE8, (uint8_t[]){0x05}, 1, 0},
    {0xE9, (uint8_t[]){0x02}, 1, 0},
    {0xEA, (uint8_t[]){0xDA}, 1, 0},
    {0xEB, (uint8_t[]){0x00}, 1, 0},
    {0xEC, (uint8_t[]){0x00}, 1, 0},
    {0xED, (uint8_t[]){0x0F}, 1, 0},
    {0xEE, (uint8_t[]){0x00}, 1, 0},
    {0xEF, (uint8_t[]){0x00}, 1, 0},
    {0xF8, (uint8_t[]){0x00}, 1, 0},
    {0xF9, (uint8_t[]){0x00}, 1, 0},
    {0xFA, (uint8_t[]){0x00}, 1, 0},
    {0xFB, (uint8_t[]){0x00}, 1, 0},
    {0xFC, (uint8_t[]){0x00}, 1, 0},
    {0xFD, (uint8_t[]){0x00}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xFF, (uint8_t[]){0x00}, 1, 0},
    {0x60, (uint8_t[]){0x40}, 1, 0},
    {0x61, (uint8_t[]){0x04}, 1, 0},
    {0x62, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0x42}, 1, 0},
    {0x64, (uint8_t[]){0xD9}, 1, 0},
    {0x65, (uint8_t[]){0x00}, 1, 0},
    {0x66, (uint8_t[]){0x00}, 1, 0},
    {0x67, (uint8_t[]){0x00}, 1, 0},
    {0x68, (uint8_t[]){0x00}, 1, 0},
    {0x69, (uint8_t[]){0x00}, 1, 0},
    {0x6A, (uint8_t[]){0x00}, 1, 0},
    {0x6B, (uint8_t[]){0x00}, 1, 0},
    {0x70, (uint8_t[]){0x40}, 1, 0},
    {0x71, (uint8_t[]){0x03}, 1, 0},
    {0x72, (uint8_t[]){0x00}, 1, 0},
    {0x73, (uint8_t[]){0x42}, 1, 0},
    {0x74, (uint8_t[]){0xD8}, 1, 0},
    {0x75, (uint8_t[]){0x00}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0},
    {0x77, (uint8_t[]){0x00}, 1, 0},
    {0x78, (uint8_t[]){0x00}, 1, 0},
    {0x79, (uint8_t[]){0x00}, 1, 0},
    {0x7A, (uint8_t[]){0x00}, 1, 0},
    {0x7B, (uint8_t[]){0x00}, 1, 0},
    {0x80, (uint8_t[]){0x48}, 1, 0},
    {0x81, (uint8_t[]){0x00}, 1, 0},
    {0x82, (uint8_t[]){0x06}, 1, 0},
    {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0xD6}, 1, 0},
    {0x85, (uint8_t[]){0x04}, 1, 0},
    {0x86, (uint8_t[]){0x00}, 1, 0},
    {0x87, (uint8_t[]){0x00}, 1, 0},
    {0x88, (uint8_t[]){0x48}, 1, 0},
    {0x89, (uint8_t[]){0x00}, 1, 0},
    {0x8A, (uint8_t[]){0x08}, 1, 0},
    {0x8B, (uint8_t[]){0x02}, 1, 0},
    {0x8C, (uint8_t[]){0xD8}, 1, 0},
    {0x8D, (uint8_t[]){0x04}, 1, 0},
    {0x8E, (uint8_t[]){0x00}, 1, 0},
    {0x8F, (uint8_t[]){0x00}, 1, 0},
    {0x90, (uint8_t[]){0x48}, 1, 0},
    {0x91, (uint8_t[]){0x00}, 1, 0},
    {0x92, (uint8_t[]){0x0A}, 1, 0},
    {0x93, (uint8_t[]){0x02}, 1, 0},
    {0x94, (uint8_t[]){0xDA}, 1, 0},
    {0x95, (uint8_t[]){0x04}, 1, 0},
    {0x96, (uint8_t[]){0x00}, 1, 0},
    {0x97, (uint8_t[]){0x00}, 1, 0},
    {0x98, (uint8_t[]){0x48}, 1, 0},
    {0x99, (uint8_t[]){0x00}, 1, 0},
    {0x9A, (uint8_t[]){0x0C}, 1, 0},
    {0x9B, (uint8_t[]){0x02}, 1, 0},
    {0x9C, (uint8_t[]){0xDC}, 1, 0},
    {0x9D, (uint8_t[]){0x04}, 1, 0},
    {0x9E, (uint8_t[]){0x00}, 1, 0},
    {0x9F, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x48}, 1, 0},
    {0xA1, (uint8_t[]){0x00}, 1, 0},
    {0xA2, (uint8_t[]){0x05}, 1, 0},
    {0xA3, (uint8_t[]){0x02}, 1, 0},
    {0xA4, (uint8_t[]){0xD5}, 1, 0},
    {0xA5, (uint8_t[]){0x04}, 1, 0},
    {0xA6, (uint8_t[]){0x00}, 1, 0},
    {0xA7, (uint8_t[]){0x00}, 1, 0},
    {0xA8, (uint8_t[]){0x48}, 1, 0},
    {0xA9, (uint8_t[]){0x00}, 1, 0},
    {0xAA, (uint8_t[]){0x07}, 1, 0},
    {0xAB, (uint8_t[]){0x02}, 1, 0},
    {0xAC, (uint8_t[]){0xD7}, 1, 0},
    {0xAD, (uint8_t[]){0x04}, 1, 0},
    {0xAE, (uint8_t[]){0x00}, 1, 0},
    {0xAF, (uint8_t[]){0x00}, 1, 0},
    {0xB0, (uint8_t[]){0x48}, 1, 0},
    {0xB1, (uint8_t[]){0x00}, 1, 0},
    {0xB2, (uint8_t[]){0x09}, 1, 0},
    {0xB3, (uint8_t[]){0x02}, 1, 0},
    {0xB4, (uint8_t[]){0xD9}, 1, 0},
    {0xB5, (uint8_t[]){0x04}, 1, 0},
    {0xB6, (uint8_t[]){0x00}, 1, 0},
    {0xB7, (uint8_t[]){0x00}, 1, 0},
    {0xB8, (uint8_t[]){0x48}, 1, 0},
    {0xB9, (uint8_t[]){0x00}, 1, 0},
    {0xBA, (uint8_t[]){0x0B}, 1, 0},
    {0xBB, (uint8_t[]){0x02}, 1, 0},
    {0xBC, (uint8_t[]){0xDB}, 1, 0},
    {0xBD, (uint8_t[]){0x04}, 1, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xBF, (uint8_t[]){0x00}, 1, 0},
    {0xC0, (uint8_t[]){0x10}, 1, 0},
    {0xC1, (uint8_t[]){0x47}, 1, 0},
    {0xC2, (uint8_t[]){0x56}, 1, 0},
    {0xC3, (uint8_t[]){0x65}, 1, 0},
    {0xC4, (uint8_t[]){0x74}, 1, 0},
    {0xC5, (uint8_t[]){0x88}, 1, 0},
    {0xC6, (uint8_t[]){0x99}, 1, 0},
    {0xC7, (uint8_t[]){0x01}, 1, 0},
    {0xC8, (uint8_t[]){0xBB}, 1, 0},
    {0xC9, (uint8_t[]){0xAA}, 1, 0},
    {0xD0, (uint8_t[]){0x10}, 1, 0},
    {0xD1, (uint8_t[]){0x47}, 1, 0},
    {0xD2, (uint8_t[]){0x56}, 1, 0},
    {0xD3, (uint8_t[]){0x65}, 1, 0},
    {0xD4, (uint8_t[]){0x74}, 1, 0},
    {0xD5, (uint8_t[]){0x88}, 1, 0},
    {0xD6, (uint8_t[]){0x99}, 1, 0},
    {0xD7, (uint8_t[]){0x01}, 1, 0},
    {0xD8, (uint8_t[]){0xBB}, 1, 0},
    {0xD9, (uint8_t[]){0xAA}, 1, 0},
    {0xF3, (uint8_t[]){0x01}, 1, 0},
    {0xF0, (uint8_t[]){0x00}, 1, 0},
    {0x21, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 0},
#ifdef EXAMPLE_Rotate_90
    {0x36, (uint8_t[]){0x60}, 1, 0},
#else
    {0x36, (uint8_t[]){0x00}, 1, 0},
#endif
};

static bool example_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
    lv_disp_flush_ready(disp_driver);
    return false;
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t) drv->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;

#if LCD_BIT_PER_PIXEL == 24
    uint8_t *to = (uint8_t *)color_map;
    uint8_t temp = 0;
    uint16_t pixel_num = (offsetx2 - offsetx1 + 1) * (offsety2 - offsety1 + 1);

    // Special dealing for first pixel
    temp = color_map[0].ch.blue;
    *to++ = color_map[0].ch.red;
    *to++ = color_map[0].ch.green;
    *to++ = temp;
    // Normal dealing for other pixels
    for (int i = 1; i < pixel_num; i++) {
        *to++ = color_map[i].ch.red;
        *to++ = color_map[i].ch.green;
        *to++ = color_map[i].ch.blue;
    }
#endif

    // copy a buffer's content to a specific area of the display
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
}

void example_lvgl_rounder_cb(struct _lv_disp_drv_t *disp_drv, lv_area_t *area)
{
    uint16_t x1 = area->x1;
    uint16_t x2 = area->x2;

    uint16_t y1 = area->y1;
    uint16_t y2 = area->y2;

    // round the start of coordinate down to the nearest 2M number
    area->x1 = (x1 >> 1) << 1;
    area->y1 = (y1 >> 1) << 1;
    // round the end of coordinate up to the nearest 2N+1 number
    area->x2 = ((x2 >> 1) << 1) + 1;
    area->y2 = ((y2 >> 1) << 1) + 1;
}

#if EXAMPLE_USE_TOUCH
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t tp_x;
    uint16_t tp_y;
    uint8_t win = tpGetCoordinates(&tp_x,&tp_y);
    if (win)
    {
        #ifdef EXAMPLE_Rotate_90
            data->point.x = tp_y;
            data->point.y = (EXAMPLE_LCD_V_RES - tp_x);
        #else
            data->point.x = tp_x;
            data->point.y = tp_y;
        #endif
        if(data->point.x > EXAMPLE_LCD_H_RES)
        data->point.x = EXAMPLE_LCD_H_RES;
        if(data->point.y > EXAMPLE_LCD_V_RES)
        data->point.y = EXAMPLE_LCD_V_RES;
        data->state = LV_INDEV_STATE_PRESSED;
        //ESP_LOGE("TP","(%d,%d)",data->point.x,data->point.y);
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

static void example_increase_lvgl_tick(void *arg)
{
    /* Tell LVGL how many milliseconds has elapsed */
    lv_tick_inc(EXAMPLE_LVGL_TICK_PERIOD_MS);
}

static bool example_lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux && "bsp_display_start must be called first");

    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

static void example_lvgl_unlock(void)
{
    assert(lvgl_mux && "bsp_display_start must be called first");
    xSemaphoreGive(lvgl_mux);
}

static void example_lvgl_port_task(void *arg)
{
    ESP_LOGI(TAG, "Starting LVGL task");
    uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        // Lock the mutex due to the LVGL APIs are not thread-safe
        if (example_lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            // Release the mutex
            example_lvgl_unlock();
        }
        if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}
#ifdef Backlight_Testing
void example_backlight_test_task(void *arg)
{
    for(;;)
    {
        setUpduty(LCD_PWM_MODE_255);
        vTaskDelay(pdMS_TO_TICKS(1000));
        setUpduty(LCD_PWM_MODE_200);
        vTaskDelay(pdMS_TO_TICKS(1000));
        setUpduty(LCD_PWM_MODE_150);
        vTaskDelay(pdMS_TO_TICKS(1000));
        setUpduty(LCD_PWM_MODE_100);
        vTaskDelay(pdMS_TO_TICKS(1000));
        setUpduty(LCD_PWM_MODE_50);
        vTaskDelay(pdMS_TO_TICKS(1000));
        setUpduty(LCD_PWM_MODE_0);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif
void app_main(void)
{
    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions

    lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);

    ESP_LOGI(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = 
    {
        .data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0,
        .data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1,
        .sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK,
        .data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2,
        .data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3,
        .max_transfer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(EXAMPLE_PIN_NUM_LCD_CS,
                                                                                example_notify_lvgl_flush_ready,
                                                                                &disp_drv);
    sh8601_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    // Attach the LCD to the SPI bus
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_LOGI(TAG, "Install SH8601 panel driver");
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh8601(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    i2c_master_Init(); //I2C_Init
#if EXAMPLE_USE_TOUCH
    lcd_touch_init();
#endif

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    //alloc draw buffers used by LVGL
    //it's recommended to choose the size of the draw buffer(s) to be at least 1/10 screen sized
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1);
    lv_color_t *buf2 = (lv_color_t *)heap_caps_malloc(EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2);
    //initialize LVGL draw buffers
    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, EXAMPLE_LCD_H_RES * EXAMPLE_LVGL_BUF_HEIGHT);

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.rounder_cb = example_lvgl_rounder_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
    //Tick interface for LVGL (using esp_timer to generate 2ms periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &example_increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, EXAMPLE_LVGL_TICK_PERIOD_MS * 1000));

#if EXAMPLE_USE_TOUCH
    static lv_indev_drv_t indev_drv;           // Input device driver (Touch)
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = disp;
    indev_drv.read_cb = example_lvgl_touch_cb;
    lv_indev_drv_register(&indev_drv);
#endif

    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);
    xTaskCreate(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);

    ui_show_msg("Connecting to Wi-Fi...");

    // Bring up Wi-Fi: connect with stored creds, else run the SoftAP captive
    // portal. Seed from secrets.h in dev so we can skip the portal while iterating.
    dial_net_init();
    dial_net_seed(WIFI_SSID, WIFI_PASSWORD);
    if (!dial_net_have_creds())
        ESP_LOGW(TAG, "no Wi-Fi creds — join AP \"%s\" to set up", dial_net_ap_ssid());
    ui_show_msg("Connecting to Wi-Fi...");
    dial_net_bringup();
    ESP_LOGI(TAG, "Wi-Fi ready");
    ui_show_msg("Linking to Orion...");

    // OAuth runs in its own task with a generous stack: the TLS handshake plus
    // the token exchange (large JSON) overflow smaller stacks. The rotary knob
    // is started from inside that task once the UI + command queue exist.
    xTaskCreate(oauth_task, "oauth", 16384, NULL, 4, NULL);
}
