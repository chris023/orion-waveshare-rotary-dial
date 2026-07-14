#include "dial_state.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "nvs.h"

#define NVS_NS "ui"

static SemaphoreHandle_t s_mux;
static QueueHandle_t     s_cmd_q;
static app_state_t       s_state;

// Written from LVGL task (touch), knob decoder task, and worker; read from the
// worker's poll gate. 64-bit stores tear on Xtensa, so guard with a SPINLOCK,
// not a mutex: the knob decoder runs in the shared esp_timer task (which also
// dispatches lv_tick_inc) and must never block on a held mutex.
static portMUX_TYPE s_input_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_input_us;

void dial_state_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(16, sizeof(app_cmd_t));
    configASSERT(s_mux && s_cmd_q);
    memset(&s_state, 0, sizeof(s_state));
    s_state.phase = PH_BOOT;
    s_state.haptics_enabled = true;   // matches dial_haptics.c's own default
    for (int z = 0; z < ZONE_COUNT; z++) {
        s_state.ui_temp_f[z]       = -1;
        s_state.zones[z].actual_c  = -1.0f;
    }
}

void dial_state_restore_prefs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t zone = 0, units = 0, haptics = 1, rot = 0;
    bool have_zone    = nvs_get_u8(h, "zone", &zone) == ESP_OK && zone < ZONE_COUNT;
    bool have_units   = nvs_get_u8(h, "units", &units) == ESP_OK;
    bool have_haptics = nvs_get_u8(h, "haptics", &haptics) == ESP_OK;
    bool have_rot     = nvs_get_u8(h, "rot", &rot) == ESP_OK && rot < 4;
    nvs_close(h);
    if (!have_zone && !have_units && !have_haptics && !have_rot) return;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (have_zone) {
        s_state.ui_zone     = (zone_idx_t)zone;
        // The "zone" key's mere existence means some earlier session already
        // established a default side — including upgrades from before
        // side_picked existed, so those devices never re-run SCR_SIDEPICK.
        s_state.side_picked = true;
    }
    if (have_units)   s_state.units_c        = (units != 0);
    if (have_haptics) s_state.haptics_enabled = (haptics != 0);
    if (have_rot)     s_state.rotation        = rot;
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

// Screen rotation, in quarter turns clockwise. Persisted here; actually applied
// by dial_display_set_rotation (this store doesn't know about the panel).
void dial_state_set_rotation(uint8_t quarters)
{
    quarters &= 3;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.rotation = quarters;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "rot", quarters);
        nvs_commit(h);
        nvs_close(h);
    }
}

/*
 * thermal_state is device truth and only lands on a poll, so anything the user
 * does — switching a zone on, turning the knob past the water temperature —
 * would leave the arc and pill rendering the OLD state until the next read.
 * Predict it from the two things already known: where the water is, and where
 * they just asked it to go. The next poll overwrites this with the truth, so a
 * wrong guess self-corrects within seconds.
 *
 * Uses the optimistic target (ui_temp_f) when one is pending, because that is
 * what the user is looking at — not the setpoint the device has been told about
 * so far. Caller must hold the lock.
 */
void dial_state_predict_thermal(app_state_t *st, zone_idx_t zone)
{
    zone_state_t *zs = &st->zones[zone];
    const char *s;
    if (!zs->on)               s = "standby";
    else if (zs->actual_c < 0) s = "holding";   // nothing measured to compare against
    else {
        float target_c = (st->ui_temp_f[zone] >= 0) ? dial_f_to_c(st->ui_temp_f[zone])
                                                    : zs->temp_c;
        float delta = target_c - zs->actual_c;
        s = (delta > 0.5f) ? "heating" : (delta < -0.5f) ? "cooling" : "holding";
    }
    strlcpy(zs->thermal_state, s, sizeof(zs->thermal_state));
}

void dial_state_set_ui_temp(zone_idx_t zone, int temp_f)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.ui_temp_f[zone] = temp_f;
    dial_state_predict_thermal(&s_state, zone);   // the pill follows the knob, not the poll
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

// Optimistic power flip, committed by the UI the instant the disc is tapped.
// The worker used to be the only one to set this — but it commits only AFTER
// the write to Orion returns, so the face sat unchanged for a whole TLS round
// trip while the user waited on a button they had already pressed.
void dial_state_set_zone_on(zone_idx_t zone, bool on)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.zones[zone].on = on;
    dial_state_predict_thermal(&s_state, zone);
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_set_ui_zone(zone_idx_t zone)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool changed = (s_state.ui_zone != zone);
    s_state.ui_zone = zone;
    xSemaphoreGive(s_mux);
    // No generation bump: the caller is the screen that already navigated —
    // re-rendering here would race the transition it just started.

    // Last side chosen survives reboot (owner D3). Writes are rare (one per
    // swipe) and this runs in the LVGL task, where a ~ms NVS write is fine.
    if (changed) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "zone", (uint8_t)zone);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

void dial_state_set_welcomed(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.welcomed = true;
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_set_side_picked(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.side_picked = true;
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_set_units_c(bool units_c)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.units_c = units_c;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    // Settings changes are rare taps in the LVGL task — a ~ms NVS write here
    // is fine (same reasoning as dial_state_set_ui_zone above).
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "units", units_c ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void dial_state_set_haptics_enabled(bool enabled)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.haptics_enabled = enabled;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "haptics", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void dial_cmd_post(const app_cmd_t *cmd)
{
    dial_state_stamp_input();
    if (xQueueSend(s_cmd_q, cmd, 0) != pdTRUE) {
        // Queue full (worker stuck in a slow network call through a 16-input
        // burst). For SET_TEMP the newest value must win: drop the oldest
        // command to make room. A lost TOGGLE would silently flip power, so
        // for toggles the drop-oldest is still the right bias (the oldest is
        // most likely an earlier temp step).
        app_cmd_t scrapped;
        xQueueReceive(s_cmd_q, &scrapped, 0);
        xQueueSend(s_cmd_q, cmd, 0);
    }
}

bool dial_cmd_receive(app_cmd_t *out, int timeout_ms)
{
    return xQueueReceive(s_cmd_q, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void dial_state_get(app_state_t *out)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mux);
}

void dial_state_commit(void (*mutate)(app_state_t *st, void *arg), void *arg)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    mutate(&s_state, arg);
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_set_phase(conn_phase_t phase, const char *err)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.phase = phase;
    if (err) strlcpy(s_state.phase_err, err, sizeof(s_state.phase_err));
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_stamp_input(void)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_input_mux);
    s_last_input_us = now;
    taskEXIT_CRITICAL(&s_input_mux);
}

int64_t dial_state_last_input_us(void)
{
    taskENTER_CRITICAL(&s_input_mux);
    int64_t v = s_last_input_us;
    taskEXIT_CRITICAL(&s_input_mux);
    return v;
}
