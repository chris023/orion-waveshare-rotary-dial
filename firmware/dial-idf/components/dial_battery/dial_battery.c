#include "dial_battery.h"

#include <stdint.h>
#include <stdlib.h>

#include "dial_state.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dial_battery";

// Odd count so the median is a real sample rather than a mean of two.
#define DIAL_BATTERY_SAMPLES 9

// A cell moves slowly and the conversion is not free. 30s is far finer than
// anything a battery does and still cheap.
#define DIAL_BATTERY_PERIOD_MS 30000

// GPIO1 on the ESP32-S3. Waveshare's demo configures exactly this.
#define BATT_ADC_UNIT     ADC_UNIT_1
#define BATT_ADC_CHANNEL  ADC_CHANNEL_0
#define BATT_ADC_ATTEN    ADC_ATTEN_DB_12
#define BATT_ADC_BITWIDTH ADC_BITWIDTH_12

// R62/R63 are 10K/10K, so the pin carries half the rail.
#define BATT_DIVIDER_NUM 2

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_ready;

void dial_battery_init(void)
{
    if (s_ready) return;

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = BATT_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = BATT_ADC_ATTEN,
        .bitwidth = BATT_ADC_BITWIDTH,
    };
    err = adc_oneshot_config_channel(s_adc, BATT_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel: %s", esp_err_to_name(err));
        return;
    }

    // Curve fitting is the S3's scheme and is what Waveshare's demo uses. If
    // eFuse calibration is missing the create call fails; we keep running and
    // fall back to the ideal-transfer maths in read() rather than dying.
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id  = BATT_ADC_UNIT,
        .atten    = BATT_ADC_ATTEN,
        .bitwidth = BATT_ADC_BITWIDTH,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration (%s), using ideal transfer",
                 esp_err_to_name(err));
        s_cali = NULL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready on ADC%d ch%d (GPIO1), divider x%d, cali=%s",
             BATT_ADC_UNIT + 1, BATT_ADC_CHANNEL, BATT_DIVIDER_NUM,
             s_cali ? "curve" : "ideal");
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

bool dial_battery_read(int *rail_mv, int *raw)
{
    if (!s_ready) return false;

    int s[DIAL_BATTERY_SAMPLES];
    int n = 0;
    for (int i = 0; i < DIAL_BATTERY_SAMPLES; i++) {
        int v;
        if (adc_oneshot_read(s_adc, BATT_ADC_CHANNEL, &v) == ESP_OK) s[n++] = v;
    }
    if (n == 0) return false;

    // Insertion-sort-by-qsort on nine ints is not worth hand-rolling.
    qsort(s, n, sizeof(s[0]), cmp_int);
    int median = s[n / 2];

    int pin_mv;
    if (s_cali) {
        if (adc_cali_raw_to_voltage(s_cali, median, &pin_mv) != ESP_OK) return false;
    } else {
        // ADC_ATTEN_DB_12 tops out near 3.3V across 4096 counts.
        pin_mv = (int)((int64_t)median * 3300 / 4096);
    }

    if (rail_mv) *rail_mv = pin_mv * BATT_DIVIDER_NUM;
    if (raw)     *raw     = median;
    return true;
}

/*
 * Resting-voltage curve for a 1S LiPo, clipped at the bottom to where this
 * board's buck gives up rather than where the cell does. Ascending by mV so
 * the walk below can stop at the first point it fits under.
 *
 * The knee between 3900 and 3750 is the real shape of the chemistry: over
 * half the usable capacity lives in 150mV. Reporting that as a straight line
 * is what makes cheap devices sit at "70%" for hours and then die.
 */
static const struct { int mv, pct; } BATT_CURVE[] = {
    { 3500,   0 }, { 3550,   5 }, { 3650,  10 }, { 3700,  15 },
    { 3750,  20 }, { 3790,  30 }, { 3820,  40 }, { 3850,  50 },
    { 3900,  60 }, { 3950,  70 }, { 4000,  80 }, { 4100,  90 },
    { 4200, 100 },
};
#define BATT_CURVE_N ((int)(sizeof(BATT_CURVE) / sizeof(BATT_CURVE[0])))

int dial_battery_pct(int rail_mv, bool charging)
{
    // On USB the rail is the charger. There is nothing here to read a level
    // out of, and guessing one would be worse than admitting it.
    if (charging) return DIAL_BATTERY_PCT_UNKNOWN;

    if (rail_mv <= BATT_CURVE[0].mv) return 0;
    if (rail_mv >= BATT_CURVE[BATT_CURVE_N - 1].mv) return 100;

    for (int i = 1; i < BATT_CURVE_N; i++) {
        if (rail_mv > BATT_CURVE[i].mv) continue;
        int mv0 = BATT_CURVE[i - 1].mv, mv1 = BATT_CURVE[i].mv;
        int p0  = BATT_CURVE[i - 1].pct, p1 = BATT_CURVE[i].pct;
        return p0 + ((rail_mv - mv0) * (p1 - p0)) / (mv1 - mv0);
    }
    return 100;
}

/* ---- sampler --------------------------------------------------------------*/

static void sample_once(bool *charging)
{
    int mv;
    if (!dial_battery_read(&mv, NULL)) return;

    // Hysteresis: cross USB_ON to latch charging, fall below USB_OFF to drop
    // it. A rail sitting exactly on one threshold would otherwise toggle the
    // icon every tick.
    if (*charging) {
        if (mv < DIAL_BATTERY_MV_USB_OFF) *charging = false;
    } else {
        if (mv >= DIAL_BATTERY_MV_USB_ON) *charging = true;
    }

    dial_state_set_battery(mv, dial_battery_pct(mv, *charging), *charging);
}

static void battery_task(void *arg)
{
    (void)arg;
    // Seed from the first reading rather than assuming a state, so a dial
    // booted on battery does not show a charging icon for its first tick.
    int mv = 0;
    bool charging = dial_battery_read(&mv, NULL) ? (mv >= DIAL_BATTERY_MV_USB_ON) : false;

    for (;;) {
        sample_once(&charging);
        vTaskDelay(pdMS_TO_TICKS(DIAL_BATTERY_PERIOD_MS));
    }
}

void dial_battery_start(void)
{
    dial_battery_init();
    if (!s_ready) {
        ESP_LOGE(TAG, "not ready, sampler not started");
        return;
    }
    xTaskCreate(battery_task, "batt", 3072, NULL, 2, NULL);
}
