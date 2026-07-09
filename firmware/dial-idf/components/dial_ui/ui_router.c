#include "ui_router.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dial_power.h"

static const char *TAG = "ui_router";

static const ui_screen_t *s_screens[SCR_COUNT];
static screen_id_t        s_current = SCR_COUNT;   // none yet
static void              *s_current_arg;
static lv_timer_t        *s_dispatch;
static uint32_t           s_rendered_gen;
// dial_power's idle clock drives the standby/dial split independently of any
// state commit; polling it here (cheap: a volatile read) lets idle->standby
// and wake->dial transitions happen on their own tick instead of waiting for
// the next device-state generation bump.
static dial_power_level_t s_rendered_power_level = DPWR_ACTIVE;

// Knob detents accumulated from the decoder's esp_timer task; drained by the
// dispatcher in the LVGL task. Guarded by a spinlock: the decoder must never
// block (it shares its task with lv_tick_inc), and this critical section is
// a handful of instructions.
static portMUX_TYPE s_knob_mux = portMUX_INITIALIZER_UNLOCKED;
static int32_t      s_knob_accum;

static ui_nav_policy_t s_nav_policy;
void ui_router_set_nav_policy(ui_nav_policy_t policy) { s_nav_policy = policy; }

void ui_router_register(screen_id_t id, const ui_screen_t *scr)
{
    configASSERT(id < SCR_COUNT);
    s_screens[id] = scr;
}

void ui_router_knob_input(int detents)
{
    taskENTER_CRITICAL(&s_knob_mux);
    s_knob_accum += detents;
    taskEXIT_CRITICAL(&s_knob_mux);
}

screen_id_t ui_router_current(void) { return s_current; }

// Swipe gestures arrive on the screen object; forward to the active screen.
// All four directions are forwarded (SCR_QUICK/SCR_BOOST dismiss on a down
// swipe) — screens that only care about left/right (scr_dial) filter the rest
// out themselves and return false.
static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    if (dir == LV_DIR_NONE) return;
    const ui_screen_t *scr = (s_current < SCR_COUNT) ? s_screens[s_current] : NULL;
    if (scr && scr->on_gesture && scr->on_gesture(dir))
        dial_state_stamp_input();
}

// Re-entering the current screen with a DIFFERENT arg rebuilds it (the dial
// screen swaps zones this way); with the same arg it's a no-op, so
// phase-driven navigation can call this every tick harmlessly.
void ui_router_go(screen_id_t id, void *arg, lv_scr_load_anim_t anim)
{
    configASSERT(id < SCR_COUNT && s_screens[id]);
    if (id == s_current && arg == s_current_arg) return;

    const ui_screen_t *old = (s_current < SCR_COUNT) ? s_screens[s_current] : NULL;
    if (old && old->destroy) old->destroy();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);
    s_current = id;
    s_current_arg = arg;
    s_screens[id]->create(scr, arg);

    // auto_del frees the previous screen (and its widgets) after the animation.
    lv_scr_load_anim(scr, anim, anim == LV_SCR_LOAD_ANIM_NONE ? 0 : 220, 0, true);

    // Render current state immediately so the new screen never shows stale "--".
    app_state_t st;
    dial_state_get(&st);
    s_rendered_gen = st.generation;
    if (s_screens[id]->on_state) s_screens[id]->on_state(&st);
}

// Runs in the LVGL task every 50ms: drain knob detents into the active screen
// and re-render when the state generation moved. This is the ONLY place the
// worker's commits reach LVGL, so the worker never needs the LVGL lock.
static void dispatch_tick(lv_timer_t *t)
{
    (void)t;
    const ui_screen_t *scr = (s_current < SCR_COUNT) ? s_screens[s_current] : NULL;
    if (!scr) return;

    taskENTER_CRITICAL(&s_knob_mux);
    int32_t detents = s_knob_accum;
    s_knob_accum = 0;
    taskEXIT_CRITICAL(&s_knob_mux);

    if (detents != 0 && scr->on_knob && scr->on_knob((int)detents))
        dial_state_stamp_input();

    app_state_t st;
    dial_state_get(&st);
    dial_power_level_t power_level = dial_power_level();
    bool gen_changed   = (st.generation != s_rendered_gen);
    bool power_changed = (power_level != s_rendered_power_level);
    if (gen_changed || power_changed) {
        s_rendered_gen = st.generation;
        s_rendered_power_level = power_level;
        if (s_nav_policy) {
            void *arg = s_current_arg;
            screen_id_t want = s_nav_policy(&st, &arg);
            ui_router_go(want, arg, LV_SCR_LOAD_ANIM_FADE_ON);  // no-op if unchanged
            scr = s_screens[s_current];
        }
        if (scr && scr->on_state) scr->on_state(&st);
    }
}

void ui_router_start(screen_id_t first, void *arg)
{
    configASSERT(!s_dispatch);
    s_dispatch = lv_timer_create(dispatch_tick, 50, NULL);
    ui_router_go(first, arg, LV_SCR_LOAD_ANIM_NONE);
    ESP_LOGI(TAG, "router up, screen %d", first);
}
