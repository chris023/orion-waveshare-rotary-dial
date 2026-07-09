/*
 * SCR_DIAL — the temperature dial (M1 functional port; M2 restyles it).
 * Arc drag + knob detents set the target °F; tap the pill to toggle power;
 * swipe left/right to show the other side. All input posts commands to the
 * worker queue and renders optimistically; the poll reconciles later.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"

static lv_obj_t *s_arc, *s_temp_lbl, *s_state_lbl, *s_zone_lbl, *s_stale_dot;
static zone_idx_t s_zone = ZONE_A;

// The setpoint currently shown (optimistic); -1 until first state arrives.
static int s_shown_f = -1;

static void render_center(int temp_f, bool on, bool online)
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

static void post_temp(int temp_f)
{
    s_shown_f = temp_f;
    dial_state_set_ui_temp(s_zone, temp_f);
    app_cmd_t cmd = { .kind = CMD_SET_TEMP, .zone = s_zone, .temp_f = temp_f };
    dial_cmd_post(&cmd);
}

// LVGL event callbacks already hold the LVGL mutex — no locking here.
static void arc_event_cb(lv_event_t *e)
{
    dial_state_stamp_input();
    int f = lv_arc_get_value(s_arc);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        char t[8];
        snprintf(t, sizeof(t), "%d", f);
        lv_label_set_text(s_temp_lbl, t);          // live feedback while dragging
    } else {                                        // LV_EVENT_RELEASED
        post_temp(f);
    }
}

static void power_event_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_TOGGLE_ON, .zone = s_zone };
    dial_cmd_post(&cmd);
}

static void create(lv_obj_t *scr, void *arg)
{
    s_zone = (zone_idx_t)(uintptr_t)arg;
    s_shown_f = -1;
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 320, 320);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, DIAL_TEMP_MIN_F, DIAL_TEMP_MAX_F);
    lv_arc_set_value(s_arc, (DIAL_TEMP_MIN_F + DIAL_TEMP_MAX_F) / 2);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_arc, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_RELEASED, NULL);

    s_zone_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_zone_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_zone_lbl, lv_color_hex(0x888888), 0);
    lv_label_set_text(s_zone_lbl, s_zone == ZONE_A ? "RIGHT SIDE" : "LEFT SIDE");
    lv_obj_align(s_zone_lbl, LV_ALIGN_CENTER, 0, -66);

    s_temp_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_temp_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_temp_lbl, lv_color_hex(0xff7043), 0);
    lv_label_set_text(s_temp_lbl, "--");
    lv_obj_align(s_temp_lbl, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *unit = lv_label_create(scr);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text(unit, "\xC2\xB0" "F");
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

    // Small amber dot at 12 o'clock while device truth is stale/unreachable.
    s_stale_dot = lv_obj_create(scr);
    lv_obj_set_size(s_stale_dot, 10, 10);
    lv_obj_set_style_radius(s_stale_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_stale_dot, lv_color_hex(0xe0b061), 0);
    lv_obj_set_style_border_width(s_stale_dot, 0, 0);
    lv_obj_align(s_stale_dot, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_add_flag(s_stale_dot, LV_OBJ_FLAG_HIDDEN);
}

static void destroy(void)
{
    s_arc = s_temp_lbl = s_state_lbl = s_zone_lbl = s_stale_dot = NULL;
}

static void on_state(const app_state_t *st)
{
    if (!s_arc || !st->have_state) return;
    const zone_state_t *z = &st->zones[s_zone];

    // Optimistic intent wins while set; otherwise follow the device.
    int f = (st->ui_temp_f[s_zone] >= 0) ? st->ui_temp_f[s_zone] : dial_c_to_f(z->temp_c);
    s_shown_f = f;
    lv_arc_set_value(s_arc, f);
    render_center(f, z->on, st->device_online);

    if (z->user_name[0]) {
        char name[36];
        snprintf(name, sizeof(name), "%s'S SIDE", z->user_name);
        // Upper-case for the label style (ASCII only — names come from Orion).
        for (char *p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        lv_label_set_text(s_zone_lbl, name);
    }

    bool stale = (st->phase != PH_READY) || !st->device_online;
    if (stale) lv_obj_clear_flag(s_stale_dot, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_add_flag(s_stale_dot, LV_OBJ_FLAG_HIDDEN);
}

static bool on_knob(int detents)
{
    if (!s_arc || s_shown_f < 0) return false;
    int nf = s_shown_f + detents;
    if (nf < DIAL_TEMP_MIN_F) nf = DIAL_TEMP_MIN_F;
    if (nf > DIAL_TEMP_MAX_F) nf = DIAL_TEMP_MAX_F;
    if (nf == s_shown_f) {                          // at the range stop
        dial_haptics_play(HAPTIC_STOP);
        return true;
    }

    lv_arc_set_value(s_arc, nf);
    char t[8];
    snprintf(t, sizeof(t), "%d", nf);
    lv_label_set_text(s_temp_lbl, t);
    post_temp(nf);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    zone_idx_t other = (s_zone == ZONE_A) ? ZONE_B : ZONE_A;
    // Commit the side choice BEFORE navigating: the nav policy follows
    // ui_zone, so an uncommitted swipe would be undone by the next state
    // commit (the poll would yank the view back to the previous side).
    dial_state_set_ui_zone(other);
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)other,
                 dir == LV_DIR_LEFT ? LV_SCR_LOAD_ANIM_MOVE_LEFT
                                    : LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_dial = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
