# Orion Dial link relay

A tiny hosted "mailbox" that lets the bedside dial finish OAuth linking
**outbound-only**. It is a standalone service — nothing here is flashed to the
dial — deployed once for the whole fleet.

> Not affiliated with, endorsed by, or supported by Orion Sleep or Cloudflare.
> Independent, community-built.

## Why this exists

Linking uses `authorization_code` + PKCE + Dynamic Client Registration. The
redirect used to point at an HTTP server on the **dial itself**
(`http://orion-dial-xxxx.local/callback`), so the phone had to reach the dial
_inbound_ over the LAN to hand back the code. That is impossible on cellular, on
guest / client-isolated SSIDs, and on weak or roamed Wi-Fi (seen live at −79 dBm
with power-save on: 100% inbound packet loss) — the link just never completed.

RFC 8628 device grant (the dial polls Orion for its own token) would be the
textbook fix, but Orion's server does not support it — confirmed:
`grant_types_supported: ["authorization_code","refresh_token"]`, every device
endpoint 404s.

So this relay stays inside `authorization_code` + PKCE and only moves the
**redirect target** off the dial:

1. Orion redirects the **phone** to `https://<RELAY>/cb?code=…&state=…`. The
   relay stores `{state → code}` (single-use, ~120 s TTL) and shows the phone a
   "you can close this" page.
2. The **dial** polls `GET https://<RELAY>/poll?state=…` (outbound HTTPS, the
   same direction the MCP client already talks) until it gets `{"code":"…"}`,
   which the relay deletes on read.
3. The dial does the PKCE token exchange **directly with Orion**. Tokens never
   touch the relay — it only ever holds a short-lived, single-use code that is
   useless without the dial's verifier, which never leaves the dial.

`state` is a ≥128-bit random value minted by the dial. It is both the mailbox
key and the CSRF binding the dial already verifies on the returned code, so the
relay treats it as a bearer secret: strict size caps, single-use reads, a
per-mailbox poll cap, and **it never logs a `code` or `state` value**.

## What's here

| File | Role |
| --- | --- |
| `src/handler.ts` | Host-agnostic routing, validation, and responses. Web platform only. |
| `src/mailbox.ts` | The Cloudflare glue: the `Mailbox` Durable Object + the store adapter. |
| `src/index.ts` | Worker entry — wires the glue into the router. |
| `wrangler.toml` | DO binding + SQLite migration, `main`, pinned `compatibility_date`. |
| `test/relay.test.ts` | vitest suite, run in the real workerd runtime. |

**Why a Durable Object and not KV:** KV is eventually consistent across PoPs, so
the `/cb` write (the phone's edge) and the `/poll` read (the dial's edge) can
land on different replicas and the dial would miss its own code. A DO keyed by
`state` (`idFromName(state)`) forces both requests onto one strongly-consistent
instance. The SQLite-backed migration (`new_sqlite_classes`) is what makes that
object storable on the **free** Workers plan.

**Portability:** the routing and validation in `handler.ts` speak only
`Request`/`Response`/`URL` and a small `RelayStore` interface. Moving to another
host is a rewrite of `mailbox.ts` + `index.ts` (the glue) against the same
interface — one file's worth of work — leaving the routes, caps, and pages as-is.

## Develop and test

```bash
cd relay
npm install
npm test          # vitest run — the suite executes inside workerd via
                  # @cloudflare/vitest-pool-workers, so the DO, its TTL alarm,
                  # and SQLite storage are the real thing
npm run typecheck # tsc --noEmit
npm run dev       # wrangler dev — a local relay on http://localhost:8787
```

Sanity-check the routes against `wrangler dev`:

```bash
curl "http://localhost:8787/poll?state=abc"                 # {"status":"pending"}
curl "http://localhost:8787/cb?code=demo&state=abc"          # HTML "Dial linked"
curl "http://localhost:8787/poll?state=abc"                 # {"code":"demo"}
curl "http://localhost:8787/poll?state=abc"                 # {"status":"pending"} (single-use)
```

## Deploy runbook

You need a Cloudflare account (the free Workers plan is enough — the DO is
SQLite-backed).

```bash
cd relay
npm install
npx wrangler login          # one-time; opens a browser for OAuth
npx wrangler deploy
```

`wrangler deploy` prints the live URL, e.g.

```
Deployed orion-dial-relay triggers (0.42 sec)
  https://orion-dial-relay.<your-subdomain>.workers.dev
```

**Capture that origin** — with no trailing slash — it is the fleet-wide
`ORION_DIAL_RELAY_BASE` (see "Firmware constants" below). Confirm it's live:

```bash
RELAY="https://orion-dial-relay.<your-subdomain>.workers.dev"
curl -s "$RELAY/poll?state=deadbeef"    # -> {"status":"pending"}
curl -s "$RELAY/"                        # -> the "nothing to do here" page
```

### Custom domain (recommended for a shipping image)

A `*.workers.dev` hostname and its edge cert are stable in practice, but a
custom domain you control makes both the URL **and** the cert issuer
predictable across the years these dials stay in the field. If you route the
Worker at, say, `relay.example.com`, use that as `ORION_DIAL_RELAY_BASE` and
read its cert (next section) instead of the `workers.dev` one.

## Read the relay's root CA (required — the firmware pins roots)

The dial does HTTPS to the relay and verifies it against a **curated** set of
embedded roots (the IDF v6.0 `esp_crt_bundle` is broken on this toolchain, so
the firmware ships its own PEM list). The relay edge cert's **root CA must be
one of those roots**, or the dial's poll fails TLS.

Find the root the edge cert chains to. `s_client` prints the leaf and
intermediates (servers don't send the root), so read the issuer of the topmost
intermediate — that names the root:

```bash
HOST="orion-dial-relay.<your-subdomain>.workers.dev"   # or your custom domain
echo | openssl s_client -connect "$HOST:443" -servername "$HOST" -showcerts 2>/dev/null \
  | awk '/BEGIN CERT/{n++} {print > ("cert-" sprintf("%02d",n) ".pem")}'
# The top intermediate is the highest-numbered cert-NN.pem; its issuer = the root:
last=$(ls cert-*.pem | sort | tail -1)
openssl x509 -noout -issuer -subject -in "$last"
```

Then verify the presented chain already builds against the roots the firmware
embeds — no news is good news:

```bash
PEM="../firmware/dial-idf/components/dial_oauth/orion_root_ca.pem"
cat cert-*.pem > chain.pem
openssl verify -CAfile "$PEM" -untrusted chain.pem cert-01.pem
#   cert-01.pem: OK   -> the relay's root is already trusted; nothing to add.
```

Cloudflare edge certs today chain to **Google Trust Services** (GTS Root R4 /
R1) or **Let's Encrypt** (ISRG Root X1) — and all three of those roots are
**already embedded** in `orion_root_ca.pem`, so most likely `verify` prints
`OK` and there is nothing to change. If it does **not** verify, the relay's root
is missing: obtain that exact root as PEM from the CA (verify it — check its
SHA-256 fingerprint and that it self-issues), append it under a one-line label
in `orion_root_ca.pem`, and re-run `openssl verify` until it's `OK`.

Any edit to `orion_root_ca.pem` trips the **cert-sentinel** CI
(`.github/workflows/certs.yml`), which re-verifies the live chains against the
embedded anchors and flags any anchor inside 12 months of expiry. Let it run;
add the relay host to that job's host list if you want it watched too.

## Firmware constants to set afterward

Two things in `firmware/dial-idf/components/dial_oauth/` must match the deploy
before you build a shipping image. (Both are outside this directory — set them
in the firmware stream.)

1. **`dial_link_config.h` → `ORION_DIAL_RELAY_BASE`** — replace the
   `https://SET-AFTER-DEPLOY.invalid` placeholder with the captured relay origin
   (no trailing slash). The redirect (`…/cb`) and poll (`…/poll`) URLs derive
   from it, so this one constant is the whole wiring.
2. **`orion_root_ca.pem`** — must contain the relay edge cert's root CA, per the
   section above (often already present; confirm with `openssl verify`).

## Operational notes

- **Never logs secrets.** No route logs a `code` or `state` value; a caught
  error returns a bare 500. Platform request-logging (Workers Logs, the
  `[observability]` block) is left **off** on purpose — it records the request
  URL, and `code`/`state` ride in the query string, so enabling it would persist
  them to a log store.
- **TTL / single-use.** A stored code lives ~120 s (`TTL_MS` in `handler.ts`),
  self-evicting via the DO's `alarm()`, and is deleted the instant it's polled.
- **Rate cap.** Each mailbox caps its own poll rate (`POLL_MAX_PER_WINDOW` in
  `mailbox.ts`) and returns `429 {"status":"slow_down"}` with `Retry-After: 1`.
- **Abuse / rate-limiting.** That app-layer cap is per-mailbox (per `state`) on
  `/poll` only: it does nothing against a flood of *distinct* states, and `/cb`
  has no cap at all. To protect the free-tier request quota against such a flood,
  add a Cloudflare rate-limiting rule (or WAF) keyed on **IP + path** for `/cb`
  and `/poll` at the edge — the in-app cap does not cover distinct-state floods.
- **`compatibility_date` is pinned, never `Date.now()`** — the relay must not
  behave differently because of the day it deployed. Bump it deliberately.
- **This does not change how often a dial needs to re-link** — Orion ends
  sign-in sessions periodically on its side (observed once at ~30 days after
  linking). The relay makes each re-link reliable and inbound-free.
