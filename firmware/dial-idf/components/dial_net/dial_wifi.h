#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * Wi-Fi for the Orion dial: NVS-backed credentials + STA connect + a SoftAP
 * captive-portal provisioning flow (no phone app required).
 *
 * Typical use from app_main:
 *   dial_net_init();
 *   if (!dial_net_have_creds()) { <show setup UI>; }
 *   dial_net_bringup();   // connects, running the portal first if needed
 */

// Connection lifecycle events, delivered from the Wi-Fi event task (keep
// handlers tiny and non-blocking — post to a queue / set state, don't work).
typedef enum {
    DIAL_NET_EV_CONNECTING,   // trying stored creds
    DIAL_NET_EV_PORTAL,       // captive portal is up, waiting for creds
    DIAL_NET_EV_GOT_IP,       // connected (also fires on every reconnect)
    DIAL_NET_EV_LOST,         // established connection dropped; auto-retrying
} dial_net_event_t;
typedef void (*dial_net_event_cb_t)(dial_net_event_t ev);

// Register the (single) lifecycle listener. Call before dial_net_bringup.
void dial_net_on_event(dial_net_event_cb_t cb);

// Initialize NVS + netif + the Wi-Fi driver + event loop. Call once, early.
void dial_net_init(void);

// Dev convenience: if not already provisioned and ssid is a real (non-placeholder)
// value, seed NVS with it so setup can skip the portal. Safe to call always.
void dial_net_seed(const char *ssid, const char *pass);

// True if Wi-Fi credentials are stored in NVS.
bool dial_net_have_creds(void);

// Erase the stored SSID/password.
void dial_net_forget(void);

// "Change network": forget the credentials AND set a persistent flag that
// (a) suppresses dial_net_seed's dev-secrets injection and (b) forces the next
// dial_net_bringup() into the captive portal. Erasing creds alone is not
// enough — the seed would silently restore the build's own network on the next
// boot. Reboot after calling this; the flag clears once new creds connect.
void dial_net_request_setup(void);

// True while a dial_net_request_setup() is still outstanding (i.e. the device
// is on its way to, or sitting in, the setup portal by the user's request).
bool dial_net_setup_requested(void);

// Connect using stored creds; if none (or connecting fails), run the captive
// portal until the user submits working credentials. Blocks until connected.
void dial_net_bringup(void);

// The SoftAP SSID used during setup (e.g. "OrionDial-A1B2"). Valid after init.
const char *dial_net_ap_ssid(void);

// True while the station currently holds an IP lease.
bool dial_wifi_is_connected(void);

// Copy the current STA IPv4 address as a string (e.g. "192.168.0.48").
// Returns false if not connected.
bool dial_net_ip(char *out, size_t sz);
