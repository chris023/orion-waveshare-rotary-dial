# assets/

Drop the TLS **root CA certificate** here as `orion-ca.der` (DER/binary form).
`src/net.ts` loads it via `new Resource("orion-ca.der")` and pins it for the
HTTPS connection to `mcp.orionsleep.com`.

`mcp.orionsleep.com` is behind Cloudflare, which typically chains to **ISRG Root
X1** (Let's Encrypt). Fetch the current root and convert to DER:

```bash
# Inspect the chain to confirm the root:
openssl s_client -connect mcp.orionsleep.com:443 -servername mcp.orionsleep.com -showcerts </dev/null

# ISRG Root X1 -> DER:
curl -s https://letsencrypt.org/certs/isrgrootx1.pem | \
  openssl x509 -outform der -out orion-ca.der
```

If the chain shows a different root (e.g. Cloudflare's own or Google Trust
Services), use that root's DER instead. The build won't succeed until this file
exists (it's referenced by `manifest.json` `resources`).

> This file is intentionally not committed (see the repo `.gitignore` for `.der`).
