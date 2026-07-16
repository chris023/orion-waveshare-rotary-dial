/*
 * SCR_NETPICK — "which network is home Wi-Fi" picked ON THE DIAL, reached
 * from SCR_WIFI_PORTAL for anyone who'd rather turn a knob than hand a
 * password to a phone's captive-portal browser (the Nest-ring fallback path;
 * dial_wifi.h's on-device setup section explains why the entry points exist).
 *
 * A rotor list (dial_list.h), same snap-centered-focus/knob-walks-one-row
 * treatment as every other list screen — the knob is the only input this
 * screen assumes works, since the whole point is "no phone required".
 *
 * dial_net_scan_request() takes the radio off-channel, so it's fire-and-
 * forget from here (the provisioning task does the actual scan); this screen
 * just watches dial_net_scan_count() with a slow poll and rebuilds its rows
 * when the count moves, or after ~4s if the scan turned up the same count
 * it started with (an empty result re-confirmed isn't a stuck poll).
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_list.h"
#include "dial_wifi.h"

#define CY 180
#define ROW_H 76
#define SCAN_TIMEOUT_MS 4000

static lv_obj_t *s_list;
static lv_obj_t *s_title_lbl;
static lv_timer_t *s_poll_timer;
static bool s_scanning;
static int  s_shown_count;
static uint32_t s_scan_started_ms;

/* ---- row factory (scr_settings.c's idiom, minus the value column: no row
 * here carries a second piece of text) -------------------------------------*/

static lv_obj_t *make_row(lv_obj_t *parent, const char *label_txt, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    // 36px side insets, not 20: rows near the top/bottom of the rotor sit
    // where the round panel's chord is narrower (owner-reported clipping
    // on the first cut of these list screens).
    lv_obj_set_style_pad_hor(row, 36, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl, label_txt);
    lv_obj_center(lbl);

    return row;
}

/* ---- row actions ----------------------------------------------------------*/

static void back_row_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    ui_router_go(SCR_WIFI_PORTAL, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

// One callback for every network row; which network rides in user_data (the
// index, never a pointer into the scan table — that table gets rewritten
// under us by the next scan, an index is stable until the row is rebuilt).
static void network_row_cb(lv_event_t *e)
{
    dial_haptics_play(HAPTIC_TICK);
    int i = (int)(uintptr_t)lv_event_get_user_data(e);
    ui_router_go(SCR_PASSKEY, (void *)(uintptr_t)i, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void rebuild_rows(void);

static void rescan_row_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    dial_net_scan_request();
    s_scanning = true;
    s_scan_started_ms = lv_tick_get();
    rebuild_rows();   // flips this row to "Scanning..." right away
}

/* ---- palette ---------------------------------------------------------------*/

// Walks s_list generically, same reason scr_menu's apply_palette does: rows
// come and go with the scan result, so nothing here can be a fixed pointer.
static void apply_palette(void)
{
    const dial_palette_t *pal = PAL();
    lv_obj_t *scr = lv_obj_get_parent(s_list);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);

    uint32_t n = lv_obj_get_child_cnt(s_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(s_list, i);
        lv_obj_set_style_border_color(row, pal->track, 0);
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        lv_obj_set_style_text_color(lbl, pal->ink_primary, 0);
    }
}

/* ---- row rebuild -------------------------------------------------------------*/

// The one place rows get (re)built: initial create(), a rescan tap (into the
// "Scanning..." state), and the poll timer noticing the scan landed. Always
// re-settles on the first network (focus_idx 1) rather than trying to
// preserve whatever was focused — the list composition just changed under
// the user, so "first real choice" is a safer landing than "row N" when N
// might now be a different network or the Rescan row itself.
static void rebuild_rows(void)
{
    lv_obj_clean(s_list);

    make_row(s_list, LV_SYMBOL_LEFT "  Back", back_row_cb, NULL);

    int n = dial_net_scan_count();
    if (n == 0) {
        make_row(s_list, "No networks found", NULL, NULL);
    } else {
        for (int i = 0; i < n; i++)
            make_row(s_list, dial_net_scan_ssid(i), network_row_cb, (void *)(uintptr_t)i);
    }

    // Non-clickable while a scan is outstanding: tapping it again would just
    // fire a second dial_net_scan_request() into the same in-flight one.
    make_row(s_list, s_scanning ? "Scanning..." : LV_SYMBOL_REFRESH "  Rescan",
              s_scanning ? NULL : rescan_row_cb, NULL);

    s_shown_count = n;
    apply_palette();
    dial_list_settle(s_list, 1);   // open on the first network, not on Back
}

/* ---- rescan poll -------------------------------------------------------------*/

// Runs the whole time this screen is up (cheap when idle: two reads and a
// return), not just while a scan is outstanding — that way a rescan started
// right before the timer would fire is never missed by a half-alive timer.
static void poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_scanning) return;
    bool count_moved = dial_net_scan_count() != s_shown_count;
    bool timed_out   = lv_tick_elaps(s_scan_started_ms) >= SCAN_TIMEOUT_MS;
    if (!count_moved && !timed_out) return;
    s_scanning = false;
    rebuild_rows();
}

/* ---- vtable ------------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    s_scanning = false;
    s_shown_count = 0;

    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_list = dial_list_create(scr, ROW_H);

    // Created right after the list (a sibling on scr, not a child of it) so
    // it draws over rows scrolling beneath — z-order is fixed at this
    // creation point and survives every later rebuild_rows() clean/refill of
    // s_list's own children. Must exist before rebuild_rows() runs, though:
    // apply_palette() (called at the end of rebuild_rows) recolors it.
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, "CHOOSE NETWORK");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    rebuild_rows();
    s_poll_timer = lv_timer_create(poll_timer_cb, 500, NULL);
}

static void destroy(void)
{
    if (s_poll_timer) { lv_timer_del(s_poll_timer); s_poll_timer = NULL; }
    s_list = NULL;
    s_title_lbl = NULL;
    s_scanning = false;
    s_shown_count = 0;
}

static void on_state(const app_state_t *st)
{
    (void)st;
    if (!s_list) return;
    apply_palette();
}

static bool on_knob(int detents)
{
    if (!s_list) return false;
    int r = dial_list_knob(s_list, detents);
    if (r) dial_haptics_play(r > 0 ? HAPTIC_TICK : HAPTIC_STOP);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_RIGHT) return false;
    ui_router_go(SCR_WIFI_PORTAL, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_netpick = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
