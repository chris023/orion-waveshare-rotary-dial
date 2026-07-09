#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * The single app-state store. The network worker (and only it) commits device
 * truth; input handlers commit optimistic UI intent; the LVGL-side dispatcher
 * polls the generation counter and re-renders the active screen on change.
 *
 * Rules:
 *  - Readers take a full snapshot with dial_state_get() — never hold pointers
 *    into the store.
 *  - Writers mutate inside dial_state_commit()'s callback — the store mutex is
 *    held for the duration, so keep mutators tiny and never block in them.
 *  - The quiet-period input gate (dial_state_stamp_input / last_input_us) is
 *    the proven resync mechanism: the worker only reads the bed back after
 *    2.5s of no input, so a poll can never land mid-interaction.
 */

typedef enum {
    PH_BOOT = 0,
    PH_WIFI_CONNECTING,
    PH_WIFI_PORTAL,        // SoftAP captive portal is up, waiting for creds
    PH_WIFI_LOST,          // had Wi-Fi, lost it; supervisor is retrying
    PH_OAUTH_DISCOVER,     // discovery + client registration
    PH_OAUTH_WAIT_CONSENT, // QR on screen, waiting for phone approval
    PH_MCP_CONNECTING,     // token ok; opening MCP + finding the device
    PH_READY,              // steady state: command + poll loop
    PH_DEGRADED,           // net up but Orion calls failing; retrying w/ backoff
} conn_phase_t;

typedef enum { ZONE_A = 0, ZONE_B = 1, ZONE_COUNT = 2 } zone_idx_t;

typedef struct {
    float temp_c;            // setpoint (top-level zones[].temp)
    float actual_c;          // measured water temp (status.zones[].temp); <0 = unknown
    bool  on;
    char  thermal_state[12]; // "standby" | "holding" | (heating/cooling presumed)
    char  user_name[24];     // first name from list_devices zones[].user ("" = unknown)
} zone_state_t;

typedef struct {
    bool    active;
    bool    heat;            // else cool
    int64_t end_epoch_s;     // 0 = unknown end time
} relief_state_t;

typedef struct {
    // Connection / lifecycle
    conn_phase_t phase;
    char    phase_err[128];   // last human-readable error (offline/error screens)
    int     retry_in_s;       // seconds until the supervisor's next retry (0 = n/a)
    char    oauth_url[600];   // authorize URL while PH_OAUTH_WAIT_CONSENT
    char    ap_ssid[33];      // SoftAP name while PH_WIFI_PORTAL

    // Device truth (valid once have_state)
    bool    have_state;
    char    serial[16];
    bool    device_online;
    zone_state_t zones[ZONE_COUNT];
    relief_state_t relief;
    struct { bool error; char desc[96]; } safety;
    char    water_fill[12];

    // Wall clock
    bool    clock_valid;

    // UI intent (optimistic layer, kept apart from device truth)
    int     ui_temp_f[ZONE_COUNT];  // shown setpoint °F; -1 = follow device
    zone_idx_t ui_zone;             // which side the UI is showing (persisted)

    // Bumped on every commit; the UI dispatcher re-renders when it changes.
    uint32_t generation;
} app_state_t;

// Initialize the store (mutex + defaults). Call once before any other call.
void dial_state_init(void);

// Copy the current state under the store mutex.
void dial_state_get(app_state_t *out);

// Run `mutate` on the live state under the mutex, then bump the generation.
void dial_state_commit(void (*mutate)(app_state_t *st, void *arg), void *arg);

// Convenience: set the connection phase (+ optional error text, NULL to keep).
void dial_state_set_phase(conn_phase_t phase, const char *err);

// --- Input quiet-period gate (torn-read-safe on 32-bit) ---
void    dial_state_stamp_input(void);   // call on EVERY user input
int64_t dial_state_last_input_us(void);
