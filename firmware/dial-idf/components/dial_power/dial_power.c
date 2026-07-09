#include "dial_power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/ledc.h"
#include "dial_state.h"
#include "dial_haptics.h"

static const char *TAG = "power";

// Backlight is LEDC timer 3 / channel 1 (lcd_bl_pwm_bsp); we install the fade
// service and drive the same channel with hardware fades.
#define BL_MODE    LEDC_LOW_SPEED_MODE
#define BL_CHANNEL LEDC_CHANNEL_1

#define DIM_AFTER_US      (30LL * 1000000)   // 30s idle -> dimmed
#define STANDBY_AFTER_US  (90LL * 1000000)   // 90s idle -> standby
#define FADE_MS           400

// Duty targets (8-bit). Night values keep the room dark.
typedef struct { uint8_t active, dimmed, standby; } duty_set_t;
static const duty_set_t DUTY_DAY   = { 255, 90, 25 };
static const duty_set_t DUTY_NIGHT = { 140, 40, 6  };

static volatile dial_power_level_t s_level = DPWR_ACTIVE;
static volatile bool s_night;
static SemaphoreHandle_t s_lvl_mux;   // apply_level runs from power task AND input paths

static void fade_to(uint8_t duty)
{
    ledc_set_fade_with_time(BL_MODE, BL_CHANNEL, duty, FADE_MS);
    ledc_fade_start(BL_MODE, BL_CHANNEL, LEDC_FADE_NO_WAIT);
}

static const duty_set_t *duties(void) { return s_night ? &DUTY_NIGHT : &DUTY_DAY; }

static void apply_level(dial_power_level_t lvl)
{
    xSemaphoreTake(s_lvl_mux, portMAX_DELAY);
    if (lvl != s_level) {
        const duty_set_t *d = duties();
        switch (lvl) {
        case DPWR_ACTIVE:  fade_to(d->active);  break;
        case DPWR_DIMMED:  fade_to(d->dimmed);  break;
        case DPWR_STANDBY: fade_to(d->standby); break;
        }
        s_level = lvl;
    }
    xSemaphoreGive(s_lvl_mux);
}

static void power_task(void *arg)
{
    (void)arg;
    for (;;) {
        int64_t idle = esp_timer_get_time() - dial_state_last_input_us();
        dial_power_level_t want =
            (idle >= STANDBY_AFTER_US) ? DPWR_STANDBY :
            (idle >= DIM_AFTER_US)     ? DPWR_DIMMED  : DPWR_ACTIVE;
        apply_level(want);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void dial_power_start(void)
{
    s_lvl_mux = xSemaphoreCreateMutex();
    configASSERT(s_lvl_mux);
    ledc_fade_func_install(0);
    fade_to(duties()->active);
    xTaskCreate(power_task, "power", 2560, NULL, 2, NULL);
}

dial_power_level_t dial_power_level(void) { return s_level; }

// Synchronous on the level (not a deferred flag): the input handler asking is
// the same event that should wake the screen, so a standby-time input fades
// the backlight up immediately and gets consumed. Later events in the same
// gesture see ACTIVE and act normally.
bool dial_power_wake_consumes(void)
{
    if (s_level != DPWR_STANDBY) return false;
    apply_level(DPWR_ACTIVE);
    return true;
}

void dial_power_set_night(bool night)
{
    if (s_night == night) return;
    s_night = night;
    dial_haptics_set_night(night);
    // Re-apply the current level with the new duty table.
    dial_power_level_t lvl = s_level;
    s_level = (dial_power_level_t)-1;
    apply_level(lvl);
    ESP_LOGI(TAG, "night mode %s", night ? "on" : "off");
}
