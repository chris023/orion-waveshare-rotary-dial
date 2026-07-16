#pragma once
#include "dial_state.h"

/*
 * sim_state.c implements the dial_state.h API the real components/dial_state
 * (FreeRTOS mutex + NVS + esp_timer) provides, but as a single-threaded
 * global over one app_state_t — there is no worker task here, so "the store
 * mutex" is just "the only thread there is."
 *
 * This header adds ONE thing the real dial_state.h doesn't: direct access to
 * the live struct, so main.c's scenarios can set fields no setter in the
 * dial_ui-facing API covers (zones[], have_state, serial, ap_ssid, oauth_url,
 * ota, ...) before calling ui_router_go. Screens never see this header —
 * only main.c does.
 */
app_state_t *sim_state_ptr(void);

// Reset to a blank-but-valid baseline (zero, generation 0). main.c calls this
// between scenarios so nothing bleeds across screenshots.
void sim_state_reset(void);
