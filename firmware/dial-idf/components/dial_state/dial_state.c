#include "dial_state.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static SemaphoreHandle_t s_mux;
static app_state_t       s_state;

// Written from LVGL task (touch), knob decoder task, and worker; read from the
// worker's poll gate. 64-bit stores tear on Xtensa, so guard with the mutex —
// contention is rare and sub-microsecond.
static int64_t s_last_input_us;

void dial_state_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    configASSERT(s_mux);
    memset(&s_state, 0, sizeof(s_state));
    s_state.phase = PH_BOOT;
    for (int z = 0; z < ZONE_COUNT; z++) {
        s_state.ui_temp_f[z]       = -1;
        s_state.zones[z].actual_c  = -1.0f;
    }
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
