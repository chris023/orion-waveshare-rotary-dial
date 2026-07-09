/*
 * SCR_CONNECTING — boot/progress text, driven entirely by the connection
 * phase. Also serves PH_DEGRADED with the last error + retry countdown.
 * SCR_ERROR shares this implementation (registered separately for clarity
 * of navigation intent).
 */
#include "ui_screens_internal.h"

static lv_obj_t *s_label;
static lv_obj_t *s_sub;

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_label = lv_label_create(scr);
    lv_obj_set_width(s_label, 300);
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_label, LV_ALIGN_CENTER, 0, -12);
    lv_label_set_text(s_label, "");

    s_sub = lv_label_create(scr);
    lv_obj_set_width(s_sub, 300);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0x808080), 0);
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_16, 0);
    lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, 28);
    lv_label_set_text(s_sub, "");
}

static void destroy(void) { s_label = s_sub = NULL; }

static void on_state(const app_state_t *st)
{
    if (!s_label) return;
    const char *main_txt = "";
    char sub_txt[160] = "";

    switch (st->phase) {
    case PH_BOOT:              main_txt = "Starting up..."; break;
    case PH_WIFI_CONNECTING:   main_txt = "Connecting to Wi-Fi..."; break;
    case PH_WIFI_LOST:
        main_txt = "Wi-Fi lost";
        snprintf(sub_txt, sizeof(sub_txt), "Reconnecting...");
        break;
    case PH_OAUTH_DISCOVER:    main_txt = "Linking to Orion..."; break;
    case PH_MCP_CONNECTING:    main_txt = "Connecting to your bed..."; break;
    case PH_DEGRADED:
        main_txt = "Orion unreachable";
        if (st->retry_in_s > 0)
            snprintf(sub_txt, sizeof(sub_txt), "%s\nRetrying in %ds",
                     st->phase_err, st->retry_in_s);
        else
            snprintf(sub_txt, sizeof(sub_txt), "%s\nRetrying...", st->phase_err);
        break;
    default:                   main_txt = "..."; break;
    }
    lv_label_set_text(s_label, main_txt);
    lv_label_set_text(s_sub, sub_txt);
}

const ui_screen_t scr_connecting = {
    .create = create, .destroy = destroy, .on_state = on_state,
};
