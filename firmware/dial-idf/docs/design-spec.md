# JUDGING

## Scores

| Criterion | D0 INSTRUMENT | D1 THERMAL FIELD | D2 NIGHTSTAND OBJECT |
|---|---|---|---|
| (a) 3am glanceability | **9** — constant-color numeral + parallel shape channel is the strongest "read it without thinking" system | **7** — field color is great pre-attentive state, but numeral = accent[state] means the *night cooling numeral is #883818 on #100C08* — the primary readout dims exactly when you need it | **8** — always-on clock = zero wake latency (genuine 3am win); hero numeral never state-tinted; 0.5°C decimals slightly hurt the glance |
| (b) Touch DLS (72px+, safe zone, occlusion) | **8** — 92⌀ power at reach-radius 148 < 150 ✓; single touch target; clean | **8** — 160×88 pill in the arc gap ✓; sheet is opaque solid ✓ | **9** — ⌀88 power at reach 144 ✓; only spec that *reasons about occlusion* (off-glass bezel, feedback lands above the finger); layout identical across sides so the power reach never moves |
| (c) Knob-first fit | **8** — needle+flick synced to haptic is very knob-native, BUT wake-detent-*applied* contradicts the project's already-resolved rule and risks accidental 3am setpoint changes | **8** — consumed wake detent (matches roadmap); 90ms zoom bump fine | **8** — "arc updates instantly, zero tween, 1:1 with the physical bezel" is the single best knob principle in any direction; standby bezel-nudge behavior underspecified |
| (d) LVGL 8.4 / ESP32 feasibility | **7** — FLAGS: per-state needle-*tip shapes* aren't supported by `lv_meter` needle_line (needs needle_img/custom draw); marching-dash segmented arc = custom draw event; both non-trivial | **7** — FLAGS: "only the thin ring redraws" is **false** in LVGL 8.4 — a style-opa change on the arc indicator invalidates the whole 320×320 arc object, so the 3.6s breathing loop is a continuous near-full-screen redraw on a QSPI panel; haptic IDs #27/#29/#18 are mislabeled (self-acknowledged) | **9** — all stock widgets; dashed line, peek, always-on all cheap; haptic IDs verified correct against ROM Library 6 |
| (e) First-party polish | **9** — fixed-slot tabular numerals, one font/two faces, *correct* DRV2605 IDs, Kollsman-bug idiom | **8** — real motion budget, checkable blue-cap rule; "20% richer field on your side" is a gimmick that doubles palette tokens | **9** — "clock that controls temperature" identity, one substitution rule reused 3×, chassis ring persisting across faces — the most *designed system* |
| (f) Night mode (no blue, cooling reads) | **9** — cooling carried entirely by shape/motion; dimmest overall emission | **8** — B≤0x18 hard cap is the best *checkable* rule of the three, but the value-ladder dims the numeral (see a) | **8** — pale sand `#C8B888` has B=0x88=136: **violates its own no-blue intent** — a light desaturated color still drives blue subpixels hard; day-ivory→night-black is exactly the "inverted dark mode" D1 warns against |
| **Total** | **50** | **46** | **51** |

**Winner: Direction 2 (Nightstand Object)** — base for the spec below, with heavy grafts.

---

# THE WINNING SPEC — "Nightstand Instrument"

**Base:** D2's chassis, always-on standby, layout skeleton, touch reasoning, haptic mechanism.
**Grafted from D0:** full day/night palette + numeral-never-recolored rule (fixes D2's ivory-day inversion and blue-heavy night sand), shape-channel grammar table, the single 88px Montserrat font (replaces Space Grotesk — one family everywhere, no font-sourcing friction), offline treatment, range-stop elastic, night haptic rate-limit, swipe-mirrors-the-bed + wake-to-last-side.
**Grafted from D1:** consumed wake detent (matches the resolved roadmap rule; overrides D0), B≤0x18 checkable night rule (applied to the D0 palette, with offending values corrected), boost folded into the state line (kills the badge-collision class), ghost-ring positioning method (avoids custom arc draws), "WATER" caption wording, error-stays-loud-at-night, `lv_obj_align_to` for width-varying neighbors.
**Feasibility fixes:** D1's breathing loop replaced by an 18×18 chevron pulse (tiny invalidation region); D0's needle-tip shapes and marching dash replaced by arc + ghost ring + chevron shape channel (zero custom draw); D1's field tint dropped entirely (flat `bg`, trivial redraw budget); D1's wrong haptic IDs replaced with the verified D0/D2 set; D0's unit/numeral overlap at "110" fixed with real tabular-width math.

Platform: LVGL 8.4 / ESP-IDF, 360×360 round RGB565 IPS, ESP32-S3-Knob, encoder GPIO8/7 (off-glass rotating bezel), CST816 touch, DRV2605 @ I²C 0x5A (`drv2605_dev_handle` in `i2c_bsp`). Restyles `components/dial_ui/scr_dial.c`; maps onto `dial_state.h` (`thermal_state`, `temp_c`, `actual_c`, `DIAL_TEMP_MIN_F`=55, `DIAL_TEMP_MAX_F`=110). Units: **°F**, 1°/detent, clamped live to the zone's reported range if the API supplies one.

## 1. Design thesis

A bedside clock that happens to control temperature (D2), rendered with instrument discipline (D0): the resting state is always the time, every control face grows out of the same persistent hairline ring, and the one big number is a fact — never recolored by mood. Color is a contract for state, carried in parallel by shape at all times, so 3am never depends on hue.

## 2. Palette

Rule 1 (all modes): every value quantizes exactly to RGB565 — R,B ≡ 0 mod 8; G ≡ 0 mod 4. Rule 2 (night only, grafted from D1): **blue channel ≤ 0x18 (24) on every swatch** — a checkable rule, not per-color judgment.

**Day** (D0's set + D2's identity metals):

| Token | Hex | Role |
|---|---|---|
| `bg` | `#101418` | screen background |
| `surface` | `#181C20` | pill, power disc, sheet |
| `track` | `#202830` | chassis ring, arc track (drawn @ 70% opa) |
| `ink-primary` | `#F0F0E8` | numeral, clock — never state-tinted |
| `ink-secondary` | `#888C88` | name, unit, captions, ghost ring |
| `accent-heat` | `#E86018` | heating |
| `accent-cool` | `#3888C8` | cooling (day only) |
| `neutral-holding` | `#587868` | at target |
| `neutral-standby` | `#585858` | zone off |
| `warning` | `#E82818` | faults only — never thermal |
| `stale` | `#C89838` | freshness dot only |
| `identity-home` | `#C8A050` | brass underline (yours) |
| `identity-partner` | `#98A0A8` | pewter underline (partner) |

**Night** (D0's warm family, corrected to B≤0x18):

| Token | Hex | Notes |
|---|---|---|
| `bg` | `#100C08` | |
| `surface` | `#201810` | |
| `track` | `#281C10` | |
| `ink-primary` | `#C87818` | ember (was `#C87840`, B=64 → capped) |
| `ink-secondary` | `#785018` | |
| `accent-heat` | `#E88818` | brightest thing on screen at night |
| `accent-cool` | `#886018` | dim warm khaki — hue does zero work; shape does (see grammar) |
| `neutral-holding` | `#504018` | |
| `neutral-standby` | `#302818` | |
| `warning` | `#C83010` | B=16 ✓; pulses, full-strength |
| `stale` | `#A07818` | |
| `identity (both)` | `#906818` | solid line = yours, **dashed** = partner (pattern replaces hue) |

**State grammar — two parallel channels, always on (D0 graft):** the numeral/clock is `ink-primary` unconditionally; state color appears only on the arc indicator, state pill, power ring, and presence dots. Every state also carries a shape, using stock LVGL symbol glyphs (no custom subset):

| State | Glyph | Extra cue |
|---|---|---|
| Heating | `LV_SYMBOL_UP` ▲ | glyph pulses (see §6) |
| Cooling | `LV_SYMBOL_DOWN` ▼ | glyph pulses |
| Holding | `LV_SYMBOL_MINUS` ▬ | static |
| Standby | `LV_SYMBOL_STOP` ○ | indicator @ 30% opa |
| Offline | `LV_SYMBOL_CLOSE` × | numeral 45% opa, indicator hidden, pill "OFFLINE" in `warning`, stale dot solid |

Night cooling therefore reads by: ▼ glyph, its pulse, and the dim-khaki pill — never blue, never a dimmed numeral (D1's failure mode fixed).

Night window: Orion sleep schedule via MCP (bedtime −30min → wake +30min), manual override in quick-actions. Backlight PWM (`lcd_bl_pwm_bsp`) tiers compound with the palette swap: day ceiling / night floor / 150ms "night-active" intermediate on any input, decaying after 8s (D2). Display never fully black.

## 3. Typography

One custom font, D0's exactly: **`dial_font_num_88`** — Montserrat SemiBold, 88px, 4bpp, charset `0-9:` (11 glyphs, one contiguous range):

```
lv_font_conv --font Montserrat-SemiBold.ttf --size 88 --bpp 4 \
  --range 0x30-0x3A --format lvgl --output dial_font_num_88.c
```

Serves both heroes: Home setpoint and Standby clock. Digit advance ≈ 52px at this size → "110" ≈ 156px, "10:47" ≈ 240px — both inside the visible disk with margin (verify once on hardware).

| Font | Use |
|---|---|
| Montserrat 12 | micro captions (last-sync, boost seconds) |
| Montserrat 16 | side name, water caption, state-pill word, date, peek setpoints' labels |
| Montserrat 20 | unit `°F` (subordinate to the reading, per instrument convention) |
| Montserrat 24 | quick-actions rows, peek numerals |
| Montserrat 48 | boot/error fallback numeral only |
| dial_font_num_88 | Home setpoint, Standby clock |

## 4. HOME FACE (360×360, center 180,180; safe zone r150; rim band r150–180 indicators only)

| # | Element | Position | Size | Color | Font |
|---|---|---|---|---|---|
| 1 | Background | full | 360×360 | `bg` (flat — no gradient) | — |
| 2 | Chassis ring → arc track | center, r=165, w=16 | `lv_arc`, rotation 135, bg angles 0–270 (90° gap at 6 o'clock) | `track` @ 70% | — |
| 3 | Arc indicator | same geometry, rounded caps | angle = 135 + 270·(set−55)/55 | state accent; 30% opa in standby, 100% otherwise; **zero tween — 1:1 with bezel** | — |
| 4 | Ghost ring (measured) | 10px hollow ring, 2px stroke, positioned at x=180+165·cos θ, y=180+165·sin θ, θ = same formula on `actual_f` | ⌀10 | `ink-secondary` @ 50% day / 45% night; hidden if `actual_c<0` | — |
| 5 | Side name | (180,64) | ≤160×18, caps, +2px tracking | `ink-secondary` | Mont 16 |
| 6 | Identity underline | (180,82) | 60×3; solid=yours, dashed=partner (night: same hue both, pattern differs) | identity token | — |
| 7 | Water caption | (180,98) | "WATER 71°" / "WATER —" | `ink-secondary` @ 80% | Mont 16 |
| 8 | Setpoint numeral | fixed 210×92 container centered (180,150); text centered, **fixed anchor — digits share advance width, no reflow jitter** | — | `ink-primary` always | dial_font_num_88 |
| 9 | Unit `°F` | fixed (266,122); "110" right edge ≈ x258 → ≥8px clearance (verify on hw) | — | `ink-secondary` | Mont 20 |
| 10 | State pill | centered (180,214) | auto ≤160×26, radius 13, `surface` fill, 1px state-accent border; 18×18 leading glyph + word ("HEATING"); **boost folds in as prefix** `⚡ 12:40 ·` (no floating badge) | text/icon/border = state accent | Mont 16 |
| 11 | Power disc | (180,280) | ⌀88 (reach 144 < 150 ✓); `surface` fill, 2px ring: state accent when ON, `track` when off; 28px power glyph `ink-primary`/`ink-secondary` | — | — |
| 12 | Staleness dot | (180,26) rim band | ⌀10 | `stale`; hidden unless `phase != PH_READY \|\| !device_online`; 300ms fade | — |
| 13 | Page dots | (172,340)/(188,340) | ⌀6, filled=current | `ink-secondary` / `track` | — |

Occlusion (D2): rotation is off-glass — never occludes. The one tap target (power) is bottom-of-safe-zone, so all feedback (numeral, pill, arc) lands above the finger. Layout identical between sides except name/underline/page-dots — power reach never moves.

## 5. STANDBY FACE

Always on; never black. A clock, not a dashboard — no temperatures resting on screen.

| Element | Position | Font | Color |
|---|---|---|---|
| Chassis hairline ring | r=165, w=2 | — | `track` (same ring that lights up as the Home arc — one object, several faces) |
| Clock `H:MM` | (180,168) | dial_font_num_88 | day `ink-primary`; night `neutral-holding` `#504018` — deliberately dimmer than ember so the room stays dark |
| Date "WED · JUL 9" | (180,232) | Mont 16 | `ink-secondary` |
| Presence dots ×2 | (164,104)/(196,104), ⌀8 | — | left=partner, right=you (matching physical bed sides); hollow=standby, solid=holding/heating/cooling, colored by that zone's state token |

**Peek (D2):** tap → both zone setpoints ("68° · 72°", Mont 24) slide in below the date, 180ms in / 900ms hold / 150ms out. **Wake:** tap → Home on *last-actively-controlled* zone (D0), 220ms `FADE_ON`. Knob detent → wakes and is **consumed, never applied** (D1 / roadmap rule — a dark-screen detent must not change the bed). Auto-return to Standby after 45s idle.

## 6. Transitions & motion

Motion budget (D1's discipline): full-screen animation only on face changes; the only continuous animation is an 18×18 glyph — a trivially small invalidation region.

| Motion | Trigger | Spec |
|---|---|---|
| Standby ↔ Home | tap / consumed detent | `LV_SCR_LOAD_ANIM_FADE_ON`, 220ms ease-out |
| Home ↔ Partner | swipe L/R | `MOVE_LEFT/RIGHT`, 220ms (direction mirrors the physical bed, §8) |
| Home → Standby | swipe down / 45s idle | `MOVE_BOTTOM`, 200ms |
| Quick-actions | long-press anywhere | opaque `surface` sheet, bottom 55%, slides up 180ms ease-out, down 150ms |
| Detent tick | every knob click | arc indicator: instant, zero tween; numeral: `transform_zoom` 256→266→256 over 90ms ease-out; synced haptic |
| Range stop (55/110) | blocked detent | numeral+indicator nudge 4px in blocked direction, spring back 140ms one-overshoot; range-stop haptic |
| State chevron pulse | continuous while heating/cooling | pill glyph opa 60%↔100%, 1.2s day / 2.4s night ping-pong — **only the 18×18 icon redraws** (replaces D1's infeasible whole-arc breathing) |
| Power toggle | tap | indicator angular length animates to/from 0 at 135°, 200ms ease-in-out |
| State word change | telemetry | 120ms cross-fade |
| Clock minute tick | — | instant swap, no animation (deliberate) |
| Warning pulse | fault | pill opa 100↔55%, 1s period, rate-limited |

## 7. Haptics (DRV2605L, ROM Library 6 / LRA — IDs verified)

| Event | Effect | Feel |
|---|---|---|
| Detent | **#26** Sharp Tick 3 – 60% | short enough for fast spins |
| Range stop | **#10** Double Click – 100% | shape-distinct wall, not just quieter |
| Confirm (power, boost, quick-action) | **#1** Strong Click – 100% | "a command left the device" |
| Face-switch arrival | **#25** Sharp Tick 2 – 80% | settle cue |
| Error / comms lost | **#47** Buzz 1 – 100% | texturally unpleasant on purpose |

**Night attenuation — one mechanism (D2):** at dusk, reprogram the DRV2605 rated/overdrive-voltage registers ~35% lower once; every effect plays quieter with no per-event branching. **Exceptions:** error is excluded from the trim — a safety fault must wake a foggy person (D1) — and range-stop/error are debounced to ≤1 fire/second at night (D0), so spinning against the limit half-asleep never becomes an alarm.

## 8. Partner-side differentiation

Four redundant signals, layout never mirrored (power reach is muscle memory):
1. **Name, always** — actual first name from the API, never LEFT/RIGHT.
2. **Identity underline** — brass vs pewter by day; same warm hue at night, solid vs dashed (the hue→pattern substitution rule, reused).
3. **Page dots** — position confirms which face.
4. **Swipe mirrors the bed** (D0) — `zone_a`/`zone_b` mapped at setup so swiping right reveals the person who actually sleeps to your right; and the dial always wakes to the last-controlled side, so a wrong name on screen means you haven't touched anything yet.

## 9. What makes this first-party, not hobbyist

1. **One 11-glyph font, two hero faces, zero reflow** — fixed-slot tabular numerals; clock and setpoint share weight, kerning, DNA.
2. **One substitution rule, applied three times** — hue→value/shape/pattern governs night cooling, warning-vs-heat, and night identity. A system, not band-aids.
3. **A chassis that persists** — the same r=165 ring is a hairline on Standby and the lit gauge on Home; the object doesn't change, its face does.
4. **A motion budget you can audit** — continuous animation confined to an 18×18 glyph; full-screen redraws only on face transitions; every hex lands bit-exact in the framebuffer; every night swatch machine-checkable at B≤0x18.
5. **Interaction rules that respect 3am** — consumed wake detents, rate-limited night haptics, error the sole loud thing, and a numeral whose color never lies.

---

# BUILD CHECKLIST (ordered)

1. Bake `dial_font_num_88.c` (`lv_font_conv` command in §3); add to `dial_ui` CMakeLists.
2. `dial_palette.h` — day/night token tables as `lv_color_hex` constants + `palette_set_mode()`; static-assert/script check: RGB565-exact all modes, B≤0x18 night.
3. Night-mode service: sleep-window from MCP schedule + override flag; drives palette swap, backlight PWM tiers (ceiling/floor/night-active decay), haptic trim.
4. Chassis ring: shared `lv_arc` style (r=165; w=2 standby / w=16 home, rotation 135, bg 0–270).
5. Home screen (`scr_dial.c` restyle): flat `bg`, arc track+indicator (zero-tween value set), state-opa rule (30%/100%).
6. Ghost ring: 10×10 `lv_obj`, 2px border, transparent fill; `ghost_position(actual_f)` trig placement; hide on `actual_c<0`.
7. Labels: side name, identity underline (`lv_obj` 60×3 + dashed `lv_line` variant), water caption, numeral (fixed 210×92 container, `dial_font_num_88`), unit `°F` at (266,122).
8. State pill: pill `lv_obj` + 18×18 symbol label + word label; state→(glyph, accent) map incl. OFFLINE; boost prefix path.
9. Power disc: ⌀88 `lv_btn`, state ring, glyph, confirm haptic, indicator collapse anim.
10. Staleness dot + 300ms fade; page dots.
11. Standby screen: clock label (same font asset), date label, two presence dots, peek overlay (in/hold/out anims), wake/consume-detent logic, 45s idle timer.
12. Quick-actions bottom sheet: opaque `surface` panel, y-position `lv_anim`, rows in Mont 24, night-override toggle.
13. Animations module: detent zoom bounce (256→266→256/90ms), range-stop spring (4px/140ms), chevron pulse (1.2s/2.4s), state-word crossfade, warning pulse.
14. DRV2605 driver on `drv2605_dev_handle`: effect map {26,10,1,25,47}, dusk voltage-trim write, night ≤1Hz debounce for range-stop/error.
15. On-hardware verification pass: "110"+°F clearance at (266,122); standby clock bezel occlusion at extreme x; detent-to-arc latency feels 1:1; haptic effect audit vs TI Table 11; night palette measured for perceived blue.