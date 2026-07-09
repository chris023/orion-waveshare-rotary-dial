#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * OAuth 2.1 client for the Orion MCP server (https://mcp.orionsleep.com/),
 * ported from firmware/reference-dial/dial.ts. Public client, PKCE (S256),
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

// Copy the stored access token. Returns false if none.
bool dial_oauth_access_token(char *out, size_t sz);

// Refresh the access token using the stored refresh_token. Returns true on success.
bool dial_oauth_refresh(const oauth_disc_t *disc, const char *client_id);

// Interactive authorization (on-device consent):
//  1) start: generate PKCE + state, build the authorize URL (for the on-screen
//     QR), and start the LAN callback HTTP server. Fills url_out.
bool dial_oauth_start_authorize(const oauth_disc_t *disc, const char *client_id,
                                const char *redirect_uri, char *url_out, size_t url_sz);
//  2) finish: block until the phone approves and the callback delivers a code,
//     then exchange it for tokens (stored in NVS). Returns true on success.
bool dial_oauth_finish_authorize(const oauth_disc_t *disc, const char *client_id,
                                 const char *redirect_uri, int timeout_ms);
//  cleanup the callback server (call after finish, success or not).
void dial_oauth_stop_authorize(void);

// The most recent token-endpoint error (HTTP status + body snippet), for display.
const char *dial_oauth_last_error(void);

// The embedded PEM trust anchors for mcp.orionsleep.com, so the MCP client can
// reuse the same verified-TLS config instead of the (broken) IDF cert bundle.
const char *dial_oauth_root_ca(void);
