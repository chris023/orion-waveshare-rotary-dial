/*
 * Rotor list (see dial_list.h). The zoom/fade pass runs on every
 * LV_EVENT_SCROLL tick, so it tracks drag, snap settle, and the knob's
 * animated scroll_to alike. Transforms are visual-only in LVGL 8.4 — layout
 * and hit-testing keep the full-size row rectangles, so shrunk edge rows
 * remain >=72px touch targets (small-round-screen DLS floor).
 *
 * Opacity uses the plain `opa` style: v8.4's draw-descriptor init resolves
 * it RECURSIVELY up the parent chain (lv_obj_get_style_opa_recursive), so
 * fading the row object alone fades its child labels with it.
 */
#include "dial_list.h"

// Zoom/fade curve, in distance-from-center d (px of the row center from the
// viewport center). Full size at center; ~66% and dimmed at one screen
// radius out. 152 = the rest distance of the second neighbor for 76px rows —
// beyond that everything is already off-panel anyway.
#define ROTOR_RANGE   152
#define ZOOM_FULL     256
#define ZOOM_MIN      168
#define OPA_FULL      255
#define OPA_MIN       100

static void rotor_update(lv_obj_t *list)
{
    lv_area_t la;
    lv_obj_get_coords(list, &la);
    lv_coord_t mid = (la.y1 + la.y2) / 2;

    uint32_t n = lv_obj_get_child_cnt(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(list, i);
        lv_area_t ra;
        lv_obj_get_coords(row, &ra);
        lv_coord_t d = (ra.y1 + ra.y2) / 2 - mid;
        if (d < 0) d = -d;
        if (d > ROTOR_RANGE) d = ROTOR_RANGE;

        // Quadratic ease: neighbors stay near full size, the falloff lands
        // on the far rows — reads as curvature rather than a linear taper.
        int32_t q = ((int32_t)d * d) / ROTOR_RANGE;   // 0..ROTOR_RANGE
        lv_coord_t zoom = ZOOM_FULL - (ZOOM_FULL - ZOOM_MIN) * q / ROTOR_RANGE;
        lv_opa_t   opa  = OPA_FULL - (OPA_FULL - OPA_MIN) * d / ROTOR_RANGE;

        lv_obj_set_style_transform_pivot_x(row, LV_PCT(50), 0);
        lv_obj_set_style_transform_pivot_y(row, LV_PCT(50), 0);
        lv_obj_set_style_transform_zoom(row, (int16_t)zoom, 0);
        lv_obj_set_style_opa(row, opa, 0);
    }
}

static void scroll_cb(lv_event_t *e)
{
    rotor_update(lv_event_get_target(e));
}

lv_obj_t *dial_list_create(lv_obj_t *parent, lv_coord_t row_h)
{
    lv_obj_t *list = lv_obj_create(parent);
    lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    // Pad so row 0 rests dead-center at scroll 0 (and the last row at max
    // scroll): pad = half the viewport minus half a row. This is also the
    // "empty space top/bottom" that keeps resting rows inside the round
    // panel's wide band instead of hugging the square framebuffer's edges.
    lv_coord_t pad = 360 / 2 - row_h / 2;   // fixed 360px panel, same as every screen's CX/CY
    lv_obj_set_style_pad_top(list, pad, 0);
    lv_obj_set_style_pad_bottom(list, pad, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_snap_y(list, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(list, scroll_cb, LV_EVENT_SCROLL, NULL);
    // The knob's row math needs the pitch; children can't carry it (their
    // user_data belongs to the screens' own tap bindings).
    lv_obj_set_user_data(list, (void *)(uintptr_t)row_h);
    return list;
}

void dial_list_settle(lv_obj_t *list)
{
    lv_obj_update_layout(list);
    rotor_update(list);
}

bool dial_list_knob(lv_obj_t *list, int detents)
{
    if (!list || detents == 0) return false;
    int n = (int)lv_obj_get_child_cnt(list);
    if (n == 0) return false;
    lv_coord_t row_h = (lv_coord_t)(uintptr_t)lv_obj_get_user_data(list);
    if (row_h <= 0) return false;

    // With pad_top = center - row_h/2, the snap position of row i is simply
    // scroll_y = i * row_h — so "current focus" is the nearest multiple,
    // which stays honest even when read mid-animation (repeated detents
    // accumulate from wherever the scroll currently is, not from a stale
    // remembered index).
    int cur = (lv_obj_get_scroll_y(list) + row_h / 2) / row_h;
    if (cur < 0) cur = 0;
    if (cur > n - 1) cur = n - 1;
    int target = cur + detents;
    if (target < 0) target = 0;
    if (target > n - 1) target = n - 1;
    if (target == cur) return false;

    lv_obj_scroll_to_y(list, target * row_h, LV_ANIM_ON);
    return true;
}
