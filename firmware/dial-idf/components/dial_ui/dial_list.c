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
 *
 * The knob tracks focus by COMMANDED index, not by reading the live scroll
 * offset: LVGL's scroll animation is a 200-400ms ease-out, so deriving the
 * current row from scroll_y mid-flight rounds to the OLD row for the first
 * ~100ms and silently eats detents during a brisk spin. The commanded index
 * lives in a per-list context and is invalidated whenever a scroll WE didn't
 * command begins (i.e. a finger drag) — after that the next detent re-bases
 * off the settled scroll position.
 */
#include "dial_list.h"

// Zoom/fade floors at the second neighbor's rest distance (2 * row_h — the
// falloff must scale with the row pitch or the 72px menu rows and 76px
// settings rows would get visibly different edge treatments).
#define ZOOM_FULL     256
#define ZOOM_MIN      168
#define OPA_FULL      255
#define OPA_MIN       100

typedef struct {
    lv_coord_t row_h;
    int        target;        // knob-commanded focus index, -1 = not owning
    bool       self_scroll;   // guards SCROLL_BEGIN fired by our own scroll_to
} rotor_ctx_t;

static void rotor_update(lv_obj_t *list)
{
    const rotor_ctx_t *ctx = lv_obj_get_user_data(list);
    lv_coord_t range = 2 * ctx->row_h;

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
        if (d > range) d = range;

        // Quadratic ease: neighbors stay near full size, the falloff lands
        // on the far rows — reads as curvature rather than a linear taper.
        int32_t q = ((int32_t)d * d) / range;   // 0..range
        lv_coord_t zoom = ZOOM_FULL - (ZOOM_FULL - ZOOM_MIN) * q / range;
        lv_opa_t   opa  = OPA_FULL - (OPA_FULL - OPA_MIN) * d / range;

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

// Any scroll we didn't start ourselves (a finger drag, elastic snap-back)
// takes ownership of the position away from the knob — the commanded index
// would otherwise go stale and the next detent would jump back to it.
static void scroll_begin_cb(lv_event_t *e)
{
    rotor_ctx_t *ctx = lv_obj_get_user_data(lv_event_get_target(e));
    if (!ctx->self_scroll) ctx->target = -1;
}

static void delete_cb(lv_event_t *e)
{
    lv_mem_free(lv_obj_get_user_data(lv_event_get_target(e)));
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

    rotor_ctx_t *ctx = lv_mem_alloc(sizeof(rotor_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    ctx->row_h = row_h;
    ctx->target = -1;
    ctx->self_scroll = false;
    lv_obj_set_user_data(list, ctx);

    lv_obj_add_event_cb(list, scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(list, scroll_begin_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(list, delete_cb, LV_EVENT_DELETE, NULL);
    return list;
}

void dial_list_settle(lv_obj_t *list)
{
    lv_obj_update_layout(list);
    rotor_update(list);
}

int dial_list_knob(lv_obj_t *list, int detents)
{
    if (!list || detents == 0) return 0;
    int n = (int)lv_obj_get_child_cnt(list);
    if (n == 0) return 0;
    rotor_ctx_t *ctx = lv_obj_get_user_data(list);

    // While a finger is actively dragging this list, the touch owns the
    // position — an absolute scroll_to here would yank the content out from
    // under the drag, then the next touch frame would keep dragging from
    // the jumped offset. Swallow the detent without feedback.
    for (lv_indev_t *ind = lv_indev_get_next(NULL); ind; ind = lv_indev_get_next(ind))
        if (lv_indev_get_scroll_obj(ind) == list) return 0;

    // Base off the commanded index while the knob owns the motion (detents
    // must accumulate even when they land mid-animation); otherwise derive
    // from the settled scroll position: pad_top = center - row_h/2 makes
    // row i's snap position exactly scroll_y = i * row_h.
    int base;
    if (ctx->target >= 0) {
        base = ctx->target;
    } else {
        base = (lv_obj_get_scroll_y(list) + ctx->row_h / 2) / ctx->row_h;
        if (base < 0) base = 0;
        if (base > n - 1) base = n - 1;
    }
    int target = base + detents;
    if (target < 0) target = 0;
    if (target > n - 1) target = n - 1;
    if (target == base) return -1;

    ctx->target = target;
    ctx->self_scroll = true;   // SCROLL_BEGIN from our own scroll_to must not clear target
    lv_obj_scroll_to_y(list, target * ctx->row_h, LV_ANIM_ON);
    ctx->self_scroll = false;
    return 1;
}
