# Host tests

Small tests using a plain host compiler — no board. The relative-scale test is
dependency-free; the authentication test uses the project's existing cJSON.

## `test_dial_auth.c`

Compiles the real OAuth, MCP, and HTTP fallback implementations against a mock
ESP-IDF transport and synthetic credentials. Covers missing/Bearer challenges,
fragmented and cached response bytes, `400/401 + invalid_grant` classification,
other HTTP errors, stale statuses, TLS failures, successful handle reuse, and
bounded replay failures (open/write/headers/read, incomplete EOF, deadline, and
16 KiB response limit). No live Orion requests or device access.

Run after `idf.py build` has resolved cJSON, from `firmware/dial-idf`:

```sh
python test/run_auth_tests.py
```

Alternatively pass `--cjson /path/to/cJSON` (directory containing `cJSON.c` and
`cJSON.h`). `CC` can select a GCC-compatible host compiler. Expected output:
`all authentication-response assertions passed`.

## `test_dial_rel.c`

Pins the relative-temperature scale invariants in `components/dial_state/dial_state.h`
(the `DIAL_REL_F` / `DIAL_REL_LO_F` tables and `dial_rel_from_f` / `dial_rel_to_f`
/ `dial_rel_step` helpers): level round-trips, store-carrier stability, that each
level's wire °C lands strictly inside its bracket, one-detent-per-level stepping,
and the rails. Run it before touching those tables.

```sh
cc -I components/dial_state -Wall -o /tmp/test_dial_rel test/test_dial_rel.c -lm && /tmp/test_dial_rel
```

Expected output: `all relative-scale table assertions passed`.

The spliced `+` glyph in `dial_font_num_88` (relative `+N` levels) is verified
visually instead — the simulator's `dial-relative` scenario renders `+2`.
