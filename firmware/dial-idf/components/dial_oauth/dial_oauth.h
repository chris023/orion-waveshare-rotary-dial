#pragma once
#include <stdbool.h>
#include <stddef.h>

// Relay origin + the fleet-wide redirect_uri (ORION_DIAL_RELAY_REDIRECT), used
// by the authorize/poll path below and by main's worker_task.
#include "dial_link_config.h"

/*
 * OAuth 2.1 client for the Orion MCP server (https://mcp.orionsleep.com/),
 * ported from the project's earlier TypeScript prototype
 * (reference-dial/dial.ts, in repo history). Public client, PKCE (S256),
 * Dynamic Client Registration, tokens persisted in NVS.
 *
 * This header currently exposes the non-interactive foundation:
 *   - PKCE code verifier/challenge
 *   - OAuth metadata discovery (/.well-known/...)
 *   - Dynamic Client Registration (client_id cached in NVS)
 * The interactive authorize/callback/token-exchange + refresh come next.
 */

typedef struct {
    char authorization_endpoint[192];
    char token_endpoint[192];
    char registration_endpoint[192];
    char resource[160];
} oauth_disc_t;

// PKCE: verifier = base64url(32 random bytes); challenge = base64url(SHA256(verifier)).
void dial_oauth_pkce(char *verifier, size_t vsz, char *challenge, size_t csz);

// GET the OAuth + protected-resource metadata. Returns true on success.
bool dial_oauth_discover(oauth_disc_t *out);

// Ensure a registered public client_id for this redirect_uri (register via DCR
// if not already cached in NVS for that URI). Fills client_id_out.
bool dial_oauth_ensure_client(const oauth_disc_t *disc, const char *redirect_uri,
                              char *client_id_out, size_t sz);

// True if a non-expired access token is stored in NVS.
bool dial_oauth_have_valid_access(void);

// The redirect_uri this device's cached client_id was REGISTERED with, or
// false when no client is cached yet. Callers that would otherwise present a
// different redirect_uri (e.g. after switching from a DHCP-IP URI to an mDNS
// hostname) must keep using this one while tokens exist: dynamic client
// registration is per-redirect_uri, so a changed URI mints a NEW client_id
// and the stored refresh token — issued to the OLD one — is rejected with
// invalid_grant "Client ID mismatch", forcing the user through the QR flow
// again. Only a device with no client (or one that has already been
// re-linked) should adopt a new form.
bool dial_oauth_cached_redirect(char *dst, size_t sz);

// Copy the stored access token. Returns false if none.
bool dial_oauth_access_token(char *out, size_t sz);

// Refresh the access token using the stored refresh_token. Returns true on success.
bool dial_oauth_refresh(const oauth_disc_t *disc, const char *client_id);

// Full unlink (Settings "Re-link Orion", and the auto-relink after a
// permanently-rejected refresh token): erase the access/refresh tokens AND the
// registered client_id + its cached redirect_uri. Clearing the client is what
// lets the next boot re-register a fresh DCR client against the current relay
// redirect (dial_link_config.h) — a device that first linked on pre-relay
// firmware has the old on-LAN .local redirect cached, and reusing it would send
// the re-link QR's callback to a server that no longer exists. The next boot
// finds no token to refresh and falls back to interactive consent, re-consenting
// once. A healthy device that keeps refreshing never calls this, so it is never
// forced to re-register.
//
// Does NOT clear the "lapsed" marker below: the auto-relink sets it right
// before calling this, and it has to survive. Callers that mean "unlinked on
// purpose" (Settings > Re-link) clear it themselves.
void dial_oauth_forget(void);

// "Session lapsed" marker (NVS "oauth"/"lapsed", u8): set when a dial that WAS
// linked loses its session for good (the refresh token was permanently
// rejected), so the UI can show a calm "Sign-in expired" screen instead of
// treating it like a never-linked dial. Survives dial_oauth_forget() and
// reboots; cleared by a successful renewal, Settings > Re-link, or factory
// reset (nvs_flash_erase). set_lapsed(false) erases the key.
void dial_oauth_set_lapsed(bool on);
bool dial_oauth_is_lapsed(void);

// True if a refresh token is stored. A lapsed marker alongside a live refresh
// token can only come from an interrupted write, so main.c trusts the token.
bool dial_oauth_have_refresh(void);

// Drop only the access token (keep the refresh token + client_id), so the next
// supervisor pass re-runs refresh instead of reusing a token the server has
// already rejected. See the note in dial_oauth_have_valid_access.
void dial_oauth_forget_access(void);

// Interactive authorization (on-device consent), via the hosted PKCE
// code-relay (dial_link_config.h). The redirect_uri is the relay's /cb: the
// phone's approval is redirected there (OUTBOUND, over the internet), and the
// dial COLLECTS the resulting code by polling the relay — it runs no inbound
// HTTP server any more, which is what made linking fail on guest/isolated/weak
// Wi-Fi where the phone could never reach the dial.
//  1) start: generate PKCE + state, build the authorize URL (for the on-screen
//     QR), and record the PKCE verifier + state for the poll/exchange below.
//     Fills url_out. Does NOT open any network listener.
bool dial_oauth_start_authorize(const oauth_disc_t *disc, const char *client_id,
                                const char *redirect_uri, char *url_out, size_t url_sz);

// Result of one relay poll (dial_oauth_poll_relay_once).
typedef enum {
    DIAL_RELAY_GOT = 0,   // the code was captured; dial_oauth_have_code() is now true
    DIAL_RELAY_PENDING,   // relay reached, no code deposited yet — keep polling
    DIAL_RELAY_ERROR,     // transient transport/HTTP/parse error — keep polling
} dial_relay_poll_t;

//  1b) poll: ONE outbound HTTPS GET to <RELAY>/poll?state=…; on {"code":"…"}
//     it captures the code (into the same buffer the old LAN callback filled,
//     with the same strict bounds) and raises dial_oauth_have_code(). Call it
//     once per consent slice. `disc` is accepted for call-site symmetry with
//     start/finish; the relay host is a fixed compile-time constant, not from
//     discovery.
dial_relay_poll_t dial_oauth_poll_relay_once(const oauth_disc_t *disc);

//  2) finish: exchange the captured code + PKCE verifier + redirect_uri for
//     tokens (stored in NVS). Returns true on success. timeout_ms is a short
//     grace window for a code already in hand; the poll above is what fetches
//     it, so callers pass 0 once dial_oauth_have_code() is true.
bool dial_oauth_finish_authorize(const oauth_disc_t *disc, const char *client_id,
                                 const char *redirect_uri, int timeout_ms);
//  true once the relay poll has delivered the code, so the consent wait can be
//  split into slices the caller does other work (servicing reboots) between.
bool dial_oauth_have_code(void);
//  end the authorize session: clears the transient state (the spent mailbox
//  key). No callback server to stop any more — kept as a stable call site, and
//  safe to call right before finish (it leaves the captured code + verifier
//  intact).
void dial_oauth_stop_authorize(void);

// The most recent token-endpoint error (HTTP status + body snippet), for display.
const char *dial_oauth_last_error(void);

// Drop this component's kept-alive connection. The dial talks to one host,
// and only one connection to it is ever wanted at a time: hold the token
// endpoint's socket open and the MCP client's own connection can be refused
// (field incident 2026-07-28 — oauth refreshed happily while every MCP
// handshake was RST). Call this after finishing a token operation, before
// handing the network back to dial_mcp.
void dial_oauth_release_connection(void);

// True if the LAST HTTP call this component made (discovery, registration, or
// a token-endpoint request) failed TLS certificate verification -- nonzero
// mbedtls verify flags on the transport -- rather than a routine network/HTTP
// problem. Distinct from dial_oauth_last_token_err_permanent (an OAuth-level
// invalid_grant): this is transport-level and available even before any
// token call. Signals the device's embedded trust anchors are likely stale.
bool dial_oauth_last_err_cert(void);

// True if the LAST token-endpoint failure (refresh or code exchange) was
// PERMANENT: HTTP 400/401 with an OAuth "error":"invalid_grant" body (RFC
// 6749 §5.2) -- the refresh token is dead and will never work again. False
// for transient failures (transport error, 408/429, 5xx, any other 4xx) and
// whenever the last token-endpoint call succeeded. Reflects only the most
// recent call; callers that need to debounce a one-off server fluke should
// count consecutive permanent results themselves.
bool dial_oauth_last_token_err_permanent(void);

// The embedded PEM trust anchors for mcp.orionsleep.com, so the MCP client can
// reuse the same verified-TLS config instead of the (broken) IDF cert bundle.
const char *dial_oauth_root_ca(void);
