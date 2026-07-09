#pragma once
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "ui_router.h"
#include "dial_state.h"

// Screen vtables defined one per file, gathered by ui_screens_register_all().
extern const ui_screen_t scr_connecting;
extern const ui_screen_t scr_wifi_portal;
extern const ui_screen_t scr_oauth_qr;
extern const ui_screen_t scr_dial;
