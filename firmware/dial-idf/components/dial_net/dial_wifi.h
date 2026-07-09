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

// Initialize NVS + netif + the Wi-Fi driver + event loop. Call once, early.
void dial_net_init(void);

// Dev convenience: if not already provisioned and ssid is a real (non-placeholder)
// value, seed NVS with it so setup can skip the portal. Safe to call always.
void dial_net_seed(const char *ssid, const char *pass);

// True if Wi-Fi credentials are stored in NVS.
bool dial_net_have_creds(void);

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
