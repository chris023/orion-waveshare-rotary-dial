# Linking over a relay

Linking the dial to your Orion account is an OAuth 2.1 **Authorization Code +
PKCE** exchange with **Dynamic Client Registration** — the dial registers
itself, shows a QR code, you approve it once in your phone's browser, and the
dial ends up holding its own access + refresh tokens. That part is unchanged
(see [ORION_MCP.md](ORION_MCP.md) for how the OAuth surface was discovered).

What changed is *where the authorization code is delivered*. It used to come
back **inbound**, to an HTTP server on the dial itself; now it lands in a tiny
hosted **relay** that the dial **polls** for it. This doc is the design record
for that move — why the old path failed, why the textbook fix wasn't available,
and what the relay does (and pointedly does not) hold.

## The problem: the code had to come *inbound*

The old flow registered `redirect_uri = http://orion-dial-xxxx.local/callback`
— the dial's *own* on-LAN HTTP server, reachable by its mDNS name. After you
approved the consent page, Orion 302'd your **phone** to that `.local` address,
and the phone delivered the code by connecting *into* the dial:

```
phone ──approve──► Orion ──302──► http://orion-dial-xxxx.local/callback?code=…
                                          │ (phone connects INTO the dial)
                                          ▼
                                    dial's httpd  ──►  token exchange
```

That inbound hop is the fragile part. It needs the phone and the dial on the
same L2 segment, mDNS resolving, and — the real killer — the dial actually
reachable on port 80 from the phone. Any of the following breaks it, silently,
with no error the user can act on:

- **Weak or roamed Wi-Fi.** Seen live: the dial at −79 dBm with Wi-Fi
  power-save on dropped **100% of inbound** packets while its own *outbound*
  MCP polling kept working fine. The phone's redirect never arrived; the QR
  screen just spun.
- **Guest / client-isolated SSIDs**, which forbid station-to-station traffic
  by design — the phone can reach the internet but never the dial.
- **The phone on cellular** (or simply a different network) — extremely common,
  because scanning a QR code doesn't require being on any particular Wi-Fi, so
  people scan it on whatever network their phone happens to be on.

In all of these the consent *succeeds* at Orion, but the code can't get home.
The result was an infinite loader and a link that never completes — the single
most common failure people hit, and the root of the "the QR does nothing"
support thread.

## Why not the device grant (RFC 8628)

The textbook fix for "an input-constrained device that can't receive a
redirect" is the OAuth **Device Authorization Grant** (RFC 8628): the dial
would poll Orion's *own* token endpoint for its token and never need a redirect
at all. Orion doesn't support it. Their authorization-server metadata is
explicit (from [ORION_MCP.md](ORION_MCP.md)):

```json
"grant_types_supported": ["authorization_code", "refresh_token"]
```

No `urn:ietf:params:oauth:grant-type:device_code`; every device endpoint we
probed returned `404`. It's their server, so we can't add it. We have to stay
inside `authorization_code` + PKCE — which means a redirect has to land
*somewhere*. The fix is to make that somewhere reachable outbound-only.

## The fix: a hosted PKCE code-relay ("mailbox")

Keep Authorization Code + PKCE exactly as-is, but move the redirect target off
the dial and onto a small hosted relay, and have the dial **poll** the relay
for the code instead of receiving it. Linking becomes **outbound-only** — it
works anywhere the dial already has internet, which we know it does, because
that's the same path its MCP traffic takes.

This is viable because Orion accepts an off-device HTTPS redirect. Verified
live against their DCR + authorize endpoints: registering with
`redirect_uri = https://<relay>/cb` was accepted, and `/oauth/authorize`
returned `307 → /login?redirect=…` with the relay URL **preserved** intact —
not `invalid redirect_uri`. So after you log in, Orion *will* send your phone
to the relay.

### The flow

```
  dial                         relay (hosted)                phone / Orion
   │                                │                              │
   │ 1. DCR: redirect=<relay>/cb    │                              │
   │    make PKCE verifier +        │                              │
   │    challenge + random state    │                              │
   │                                │                              │
   │ 2. show QR (authorize URL:     │                              │
   │    redirect=<relay>/cb,        │   ─── scan ──►               │
   │    state, code_challenge)      │                              │
   │                                │              3. approve at Orion
   │                                │  ◄── GET /cb?code&state ───   │  (Orion
   │                                │     store {state → code}      │   302s the
   │                                │     "✓ return to your dial"   │   phone)
   │                                │                              │
   │ 4. GET /poll?state ───────────►│                              │
   │    (outbound HTTPS, repeats)   │  delete on read (single-use) │
   │  ◄──────────── {code} ─────────│                              │
   │                                │                              │
   │ 5. token exchange DIRECT with Orion (code + verifier + redirect_uri)
   │    ──────────────────────────────────────────────────► mcp.orionsleep.com
   │  ◄──────────────── access + refresh tokens ─────────────────────
```

1. **Register.** The dial does DCR with `redirect_uri = https://<relay>/cb` — a
   single stable constant, the same for the whole fleet — and generates a PKCE
   verifier/challenge plus a high-entropy `state` (≥128-bit random).
2. **Show the QR.** Same QR screen as before; the authorize URL now carries the
   relay redirect, the `state`, and the `code_challenge`.
3. **Approve.** You approve at Orion in your phone's browser. Orion 302s the
   **phone** to `https://<relay>/cb?code=…&state=…`. The relay files the code
   under its `state` (short TTL, single-use) and shows the phone a friendly
   "✓ Dial linked — return to your dial" page.
4. **Poll.** The dial polls `GET https://<relay>/poll?state=…` (outbound HTTPS)
   until the code is there; the relay hands it over once and deletes it.
5. **Exchange.** The dial does the PKCE token exchange **directly** with Orion
   (`mcp.orionsleep.com/oauth/token`: code + verifier + redirect_uri) and gets
   its tokens. **The tokens never touch the relay.**

Steps 1–3 and 5 are the OAuth flow the dial already ran; only the delivery of
the code in step 4 is new. The `state` the dial already generated and verifies
for CSRF binding does double duty as the relay mailbox key.

## What the relay holds — and what it doesn't

The relay is deliberately a dumb, forgetful mailbox. At most it ever holds **one
short-lived, single-use, PKCE-bound authorization code**, keyed by `state`, for
about two minutes. That's the whole of its knowledge.

- **The code is useless on its own.** Redeeming it requires the PKCE *verifier*,
  which is generated on the dial and **never leaves it**. A relay compromise,
  or anyone who intercepts a code in flight, still cannot mint a token.
- **No tokens, no PII.** Access and refresh tokens are exchanged dial↔Orion
  directly; the relay never sees them, nor your account, email, or device data.
- **`state` is a bearer secret.** It's both the mailbox key and the CSRF binding
  the dial checks on the returned code, so it's ≥128-bit random — unguessable,
  and single-use.
- **Minimal, forgetful, quiet.** The relay validates and size-caps `code`/`state`,
  evicts each entry on a per-object TTL, deletes on first read, rate-limits
  polling, and **never logs code or state values**. Orion error redirects
  (`?error=…`) are turned into a friendly failure page and clear the entry too.

Under Cloudflare (the reference host) each mailbox is a **Durable Object keyed
by `state`**, not KV — because the `/cb` write comes from the phone's edge PoP
and the `/poll` read from the dial's, and KV's cross-PoP write→read lag (up to
~60s) could let the dial miss its own code. A Durable Object routes both to one
strongly-consistent instance. The relay itself lives under [`relay/`](../relay/);
this doc is the *why*, that directory is the *how*.

## What this fixes — and what it doesn't

**Fixes:** linking no longer depends on the phone reaching the dial. It works on
cellular, on guest and client-isolated Wi-Fi, and through weak/roamed signal —
anywhere the dial has working internet. It also retires the whole on-device
port-80 HTTP server and its mDNS name, which existed only for this callback.

**Does not address:** *how often* a re-link is needed. This change is about *how*
a re-link completes — the relay makes each one **reliable and inbound-free** —
not how often Orion asks for one.

Orion ends sign-in sessions periodically on its side. A capture on 2026-10-06
caught one end-to-end: the session ended server-side at what looks like a fixed
lifetime, about **30 days after linking**, with the access token's lifetime
clamped down as that point approached. The dial's refresh path was healthy
throughout, and nothing it sends can extend the session. That is a single
observation, so treat ~30 days as indicative rather than a guaranteed figure.
From firmware 1.6.0 (first in 1.6.0-beta.1) the dial handles this calmly: instead
of a setup QR it shows a quiet "Sign-in expired" screen (the bed keeps running its
schedule meanwhile), and the owner renews from it in under a minute.

### Rollout

Already-linked dials are untouched: token **refresh** carries no `redirect_uri`,
so nothing about it changes, and they keep running on their existing tokens. The
relay redirect is adopted the next time a dial re-links — `forget()` wipes the
`oauth` NVS namespace (including any cached `.local` redirect), so a re-link
naturally mints a fresh DCR client against the relay redirect and you consent
once. There is no flag day and no coordinated cutover.

## Deploy-time settings

Two things are pinned into the shipping image once the relay is stood up:

- **The relay base URL** — the stable `https://<relay>` constant the redirect
  and poll are built from.
- **The relay's edge-certificate root CA** — the dial does HTTPS to the relay,
  so that root has to be in the firmware trust bundle (`orion_root_ca.pem`,
  under the cert-sentinel CI check). Confirm the exact root from the deployed
  certificate rather than assuming it.

See [ARCHITECTURE.md](ARCHITECTURE.md) for where linking sits in the boot phases
and the worker's state machine.
