#include "ui_screens.h"
#include "ui_screens_internal.h"

void ui_screens_register_all(void)
{
    ui_router_register(SCR_CONNECTING, &scr_connecting);
    ui_router_register(SCR_ERROR, &scr_connecting);   // same rendering, distinct id
    ui_router_register(SCR_WIFI_PORTAL, &scr_wifi_portal);
    ui_router_register(SCR_OAUTH_QR, &scr_oauth_qr);
    ui_router_register(SCR_DIAL, &scr_dial);
    ui_router_register(SCR_TONIGHT, &scr_tonight);
    ui_router_register(SCR_STANDBY, &scr_standby);
    ui_router_register(SCR_QUICK, &scr_quick);
    ui_router_register(SCR_BOOST, &scr_boost);
    ui_router_register(SCR_WELCOME, &scr_welcome);
    ui_router_register(SCR_SIDEPICK, &scr_sidepick);
    ui_router_register(SCR_SETTINGS, &scr_settings);
}
