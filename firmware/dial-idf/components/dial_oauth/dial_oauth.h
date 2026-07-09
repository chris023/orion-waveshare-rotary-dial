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

// Foundation smoke test: discover + register + log. Requires Wi-Fi up.
void dial_oauth_test(const char *redirect_uri);
