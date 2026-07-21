# Host tests

Small, dependency-free tests that compile against the firmware headers with a
plain host compiler — no ESP-IDF, no board.

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
