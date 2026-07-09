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
// worker's poll gate. 64-bit stores tear on Xtensa, so guard with the mutex —
// contention is rare and sub-microsecond.
static int64_t s_last_input_us;

void dial_state_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(16, sizeof(app_cmd_t));
    configASSERT(s_mux && s_cmd_q);
    memset(&s_state, 0, sizeof(s_state));
    s_state.phase = PH_BOOT;
    for (int z = 0; z < ZONE_COUNT; z++) {
        s_state.ui_temp_f[z]       = -1;
        s_state.zones[z].actual_c  = -1.0f;
    }
}

void dial_state_restore_prefs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t zone = 0;
    if (nvs_get_u8(h, "zone", &zone) == ESP_OK && zone < ZONE_COUNT) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        s_state.ui_zone = (zone_idx_t)zone;
        s_state.generation++;
        xSemaphoreGive(s_mux);
    }
    nvs_close(h);
}

void dial_state_set_ui_temp(zone_idx_t zone, int temp_f)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.ui_temp_f[zone] = temp_f;
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

void dial_cmd_post(const app_cmd_t *cmd)
{
    dial_state_stamp_input();
    xQueueSend(s_cmd_q, cmd, 0);
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
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_last_input_us = esp_timer_get_time();
    xSemaphoreGive(s_mux);
}

int64_t dial_state_last_input_us(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int64_t v = s_last_input_us;
    xSemaphoreGive(s_mux);
    return v;
}
