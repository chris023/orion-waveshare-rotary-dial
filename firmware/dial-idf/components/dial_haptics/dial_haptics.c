#include "dial_haptics.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs.h"
#include "i2c_bsp.h"

static const char *TAG = "haptics";

/* DRV2605 registers */
#define REG_STATUS        0x00
#define REG_MODE          0x01
#define REG_WAVESEQ1      0x04
#define REG_WAVESEQ2      0x05
#define REG_GO            0x0C
#define REG_FEEDBACK      0x1A
#define REG_CONTROL3      0x1D
#define REG_LIBRARY       0x03
#define REG_RATED_VOLTAGE 0x16
#define REG_OD_CLAMP      0x17
#define REG_A_CAL_COMP    0x18
#define REG_A_CAL_BEMF    0x19

#define MODE_INTERNAL_TRIGGER 0x00
#define MODE_AUTO_CAL         0x07

// REG_OD_CLAMP scales the overdrive-voltage ceiling (DRV2605 datasheet
// §8.6.3) -- the actual LRA drive amplitude, independent of which waveform
// plays. FIRM is today's conservative-drive default, unchanged. SOFT is
// ~65% of it (0x59 = 89, vs 0x89 = 137) -- inside the 60-70% the level was
// specced to land in, paired with FX[].night for a profile that's audibly
// and tactilely softer, not just quieter-feeling because of the effect
// choice alone.
#define REG_OD_CLAMP_FIRM 0x89
#define REG_OD_CLAMP_SOFT 0x59

/*
 * LRA effect selection (library 6). Two variants per effect: day, and a
 * lighter night set so a 2am adjustment doesn't buzz the nightstand.
 * Numbers from the DRV2605 ROM library (TI datasheet table 11.2).
 */
typedef struct { uint8_t day; uint8_t night; } fx_pair_t;
static const fx_pair_t FX[] = {
    [HAPTIC_TICK]    = { .day = 24, .night = 26 },  // sharp tick 1 / tick 3 (lighter)
    [HAPTIC_STOP]    = { .day = 10, .night = 7  },  // double click / soft bump
    [HAPTIC_CONFIRM] = { .day = 1,  .night = 7  },  // strong click / soft bump
    [HAPTIC_ERROR]   = { .day = 16, .night = 16 },  // 1000ms alert (kept: errors are rare)
};

static QueueHandle_t  s_q;
static bool           s_present;
static volatile bool  s_night;
static volatile haptic_level_t s_level = HAPTIC_LEVEL_AUTO;   // matches dial_state's own default

static bool wr(uint8_t reg, uint8_t val)
{
    return i2c_write_buff(drv2605_dev_handle, reg, &val, 1) == 0;
}

static bool rd(uint8_t reg, uint8_t *val)
{
    return i2c_read_buff(drv2605_dev_handle, reg, val, 1) == 0;
}

// Two profiles, FIRM and SOFT (effect variant + overdrive clamp travel
// together, always): LOW is SOFT unconditionally, HIGH is FIRM
// unconditionally (both are the user's explicit, time-of-day-independent
// choice — HIGH really does mean firm at 3am if that's what they picked),
// and AUTO is the adaptive one, following `night` (dial_power's sleep-
// window flag, via dial_haptics_set_night()).
static inline bool is_soft(haptic_level_t level, bool night)
{
    switch (level) {
    case HAPTIC_LEVEL_LOW:  return true;
    case HAPTIC_LEVEL_HIGH: return false;
    case HAPTIC_LEVEL_AUTO:
    default:                return night;
    }
}

// Rewrites REG_OD_CLAMP for the current level+night combination. A plain
// register write -- the chip stays in MODE_INTERNAL_TRIGGER throughout, so
// this never touches (let alone re-runs) auto-calibration, which is audible
// and whose results live in NVS (cal_load/cal_store below), untouched by
// the level. Called from dial_haptics_set_level (every level change) and
// dial_haptics_set_night (every actual day/night transition — see
// dial_power_set_night's early-return, so this isn't a per-tick cost); in
// LOW/HIGH the write it issues is simply the same value already there.
static void apply_od_clamp(void)
{
    if (!s_present) return;
    wr(REG_OD_CLAMP, is_soft(s_level, s_night) ? REG_OD_CLAMP_SOFT : REG_OD_CLAMP_FIRM);
}

// Auto-calibration results persist in NVS so we run the (audible) cal once.
static bool cal_load(uint8_t *comp, uint8_t *bemf, uint8_t *fb)
{
    nvs_handle_t h;
    if (nvs_open("haptics", NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_u8(h, "comp", comp) == ESP_OK &&
              nvs_get_u8(h, "bemf", bemf) == ESP_OK &&
              nvs_get_u8(h, "fb", fb) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void cal_store(uint8_t comp, uint8_t bemf, uint8_t fb)
{
    nvs_handle_t h;
    if (nvs_open("haptics", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "comp", comp);
    nvs_set_u8(h, "bemf", bemf);
    nvs_set_u8(h, "fb", fb);
    nvs_commit(h);
    nvs_close(h);
}

static bool chip_setup(void)
{
    uint8_t status;
    if (!rd(REG_STATUS, &status)) {
        ESP_LOGW(TAG, "DRV2605 not responding — haptics disabled");
        return false;
    }
    ESP_LOGI(TAG, "DRV2605 status 0x%02x", status);

    wr(REG_MODE, MODE_INTERNAL_TRIGGER);      // exit standby
    // LRA mode (N_ERM_LRA=1), keep default brake/loop-gain bits.
    uint8_t fb;
    rd(REG_FEEDBACK, &fb);
    wr(REG_FEEDBACK, fb | 0x80);
    wr(REG_LIBRARY, 6);                        // LRA effect library
    // Closed-loop LRA defaults for a small coin LRA (conservative drive).
    wr(REG_RATED_VOLTAGE, 0x50);
    // s_level/s_night are their static defaults (AUTO, day) at this point
    // unless a caller somehow set them before init runs, which nothing does
    // today — main.c's real persisted level lands moments later via
    // dial_haptics_set_level(), which rewrites this same register. Direct
    // wr(), not apply_od_clamp(): s_present isn't set true until this
    // function returns (dial_haptics_init assigns it from chip_setup()'s
    // result), and reaching this line already proves the chip responded.
    wr(REG_OD_CLAMP, is_soft(s_level, s_night) ? REG_OD_CLAMP_SOFT : REG_OD_CLAMP_FIRM);

    uint8_t comp, bemf, cal_fb;
    if (cal_load(&comp, &bemf, &cal_fb)) {
        wr(REG_A_CAL_COMP, comp);
        wr(REG_A_CAL_BEMF, bemf);
        wr(REG_FEEDBACK, cal_fb);
        ESP_LOGI(TAG, "restored stored calibration");
    } else {
        wr(REG_MODE, MODE_AUTO_CAL);
        wr(REG_GO, 1);
        uint8_t go = 1;
        for (int i = 0; i < 120 && go; i++) {   // cal takes ~1s max
            vTaskDelay(pdMS_TO_TICKS(20));
            rd(REG_GO, &go);
        }
        // Persist only a fully successful read-back: a transient I2C failure
        // here must not become permanent bad calibration in NVS. The DIAG
        // bit (status bit 3) reports cal failure — skip storing then too.
        uint8_t status = 0xFF;
        bool reads_ok = rd(REG_A_CAL_COMP, &comp) && rd(REG_A_CAL_BEMF, &bemf) &&
                        rd(REG_FEEDBACK, &cal_fb) && rd(REG_STATUS, &status);
        wr(REG_MODE, MODE_INTERNAL_TRIGGER);
        if (reads_ok && !(status & 0x08) && !go) {
            cal_store(comp, bemf, cal_fb);
            ESP_LOGI(TAG, "auto-calibration done (comp=0x%02x bemf=0x%02x)", comp, bemf);
        } else {
            ESP_LOGW(TAG, "auto-calibration not stored (reads_ok=%d status=0x%02x go=%d)"
                          " — will retry next boot", reads_ok, status, go);
        }
    }
    return true;
}

static void haptics_task(void *arg)
{
    (void)arg;
    haptic_effect_t fx;
    for (;;) {
        if (xQueueReceive(s_q, &fx, portMAX_DELAY) != pdTRUE) continue;
        haptic_level_t level = s_level;
        if (level == HAPTIC_LEVEL_OFF) continue;
        bool soft = is_soft(level, s_night);
        uint8_t effect = soft ? FX[fx].night : FX[fx].day;
        // Re-issuing GO restarts the sequencer — a spin never lags the knob.
        wr(REG_WAVESEQ1, effect);
        wr(REG_WAVESEQ2, 0);
        wr(REG_GO, 1);
    }
}

void dial_haptics_init(void)
{
    s_present = chip_setup();
    if (!s_present) return;
    // Queue length 1 + overwrite: only the newest effect matters.
    s_q = xQueueCreate(1, sizeof(haptic_effect_t));
    configASSERT(s_q);
    // Above the network worker (3): a detent's tick is part of the input, and
    // must not queue behind a TLS handshake. Below LVGL (5) — the frame the
    // user sees still wins.
    xTaskCreate(haptics_task, "haptics", 2560, NULL, 4, NULL);
}

void dial_haptics_play(haptic_effect_t fx)
{
    if (!s_present || !s_q) return;
    xQueueOverwrite(s_q, &fx);
}

// dial_power's day/night flag. Only AUTO's effect/clamp choice actually
// depends on it (is_soft()'s AUTO branch) — LOW and HIGH are the user's
// explicit, global choices and must not be perturbed by the sleep window,
// so this is a no-op in those two levels beyond rewriting the clamp to the
// value already there. Still unconditionally stored and still unconditionally
// forwarded to apply_od_clamp(): dial_power_set_night only calls this on an
// actual transition (see its own early-return), so there is no per-tick cost
// to worry about, and keeping this call unconditional means a level change
// INTO Auto immediately reflects whatever day/night state was already true.
void dial_haptics_set_night(bool night)
{
    s_night = night;
    apply_od_clamp();
}

void dial_haptics_set_level(haptic_level_t level)
{
    s_level = level;
    apply_od_clamp();
}
