# Orion Dial — UI Design Spec: "Nightstand Object"

Platform: LVGL 8.4 / ESP-IDF, Waveshare ESP32-S3-Knob-Touch-LCD-1.8, round 360×360
RGB565 IPS @ ~200dpi, CST816 capacitive touch, rotating outer bezel (no-button
encoder) on GPIO8/7, DRV2605 LRA haptic driver on I²C 0x5A.

---

## 1. Design thesis

The dial is a bedside clock that happens to control temperature, not a
thermostat that happens to tell the time — it should look at home next to a
lamp and a glass of water, not on a workbench. Every control surface reads as
something that grew out of the clock face for a moment and will settle back
into it, so the object's resting state — and its emotional center of gravity —
is always the time, never a menu.

---

## 2. Palette

All values are chosen so the byte pair quantizes losslessly into RGB565 (R and
B channels are multiples of 8 → exact in 5 bits; G is a multiple of 4 → exact
in 6 bits). What you specify in hex is exactly what the panel draws — no
driver-level rounding, no banding surprises.

### State-color grammar (applies in both modes)

Two independent axes carry meaning, so status is legible by **color and by
saturation/shape** separately (the second axis is what survives at night):

- **Hue family = direction of thermal travel.** Warm (amber/orange) = heating,
  moving toward a higher setpoint. The "cool" family = cooling, moving toward a
  lower setpoint. It is never encoded as literal blue — see the night-mode
  note below.
- **Saturation/weight = whether anything is happening.** Stone (flat, inert) =
  standby/off. Sage (muted, settled) = holding at target. Fully saturated
  accent = actively heating or cooling. So a glance at *saturation alone*
  answers "is the bed doing something," and hue answers "which way."
- **Warning is never color-only.** It shares a family with heat but is always
  paired with motion (a pulsing dot, not a static tint) so it can't be
  misread as "heating."

### Day mode

| Role | Hex | R/G/B (dec) | Notes |
|---|---|---|---|
| `bg-day` | `#F0E8D8` | 240/232/216 | warm ivory canvas — paper, not screen-white |
| `surface-day` | `#F8F0E0` | 248/240/224 | raised elements (power disc) |
| `track-day` | `#E0D4B8` | 224/212/184 | arc groove, unlit ring |
| `text-primary-day` | `#201C18` | 32/28/24 | warm near-black ink |
| `text-secondary-day` | `#888078` | 136/128/120 | warm gray, captions |
| `heat-accent-day` | `#E07040` | 224/112/64 | heating (terracotta, not alarm-red) |
| `cool-accent-day` | `#3890B0` | 56/144/176 | cooling (slate teal) |
| `holding-day` (sage) | `#98A890` | 152/168/144 | settled at target |
| `standby-day` (stone) | `#C8C0B0` | 200/192/176 | zone off |
| `warning-day` | `#C04030` | 192/64/48 | fault/stale — always paired with pulse motion |
| `identity-home-day` (brass) | `#C8A050` | 200/160/80 | Home occupant tag |
| `identity-partner-day` (pewter) | `#98A0A8` | 152/160/168 | Partner occupant tag |

### Night mode — "no blue light, cooling still reads"

The hard constraint: zero blue-hazard hue, but heating vs. cooling must still
be told apart at a glance, half-asleep. The answer is to **retire hue as the
carrier at night** and move direction-of-travel onto **value + saturation +
shape**, the same substitution principle used for warning above:

- Heating stays a saturated warm ember (`#E86828`) — brighter, more saturated,
  paired with an upward chevron.
- Cooling becomes a **pale, desaturated sand** (`#C8B888`) — same warm family,
  zero blue, but visibly lighter and flatter than the heating ember — paired
  with a downward chevron and the arc's indicator sweeping counter-clockwise
  instead of clockwise. Hue never leaves the amber family; direction is told
  by brightness + saturation + iconography + motion, not color.

| Role | Hex | R/G/B (dec) | Notes |
|---|---|---|---|
| `bg-night` | `#100C08` | 16/12/8 | near-black, B channel forced to its floor |
| `surface-night` | `#201810` | 32/24/16 | |
| `track-night` | `#281C18` | 40/28/24 | barely-there groove |
| `text-primary-night` | `#E0B078` | 224/176/120 | warm ember — the room's only light source |
| `text-secondary-night` | `#886038` | 136/96/56 | dimmed ember |
| `heat-accent-night` | `#E86828` | 232/104/40 | heating: saturated ember |
| `cool-accent-night` | `#C8B888` | 200/184/136 | cooling: pale desaturated sand |
| `holding-night` | `#686048` | 104/96/72 | dim warm stone-brown |
| `standby-night` | `#302C20` | 48/44/32 | nearly invisible against bg |
| `warning-night` | `#B02818` | 176/40/24 | pulses; full-strength even at night |
| `identity-home-night` (brass, solid line) | `#906830` | 144/104/48 | |
| `identity-partner-night` (same hue, **dashed** line) | `#906830` | 144/104/48 | pattern differs, not hue |

Backlight is dimmed to a low nonzero night floor as a comfort measure, but
that is secondary — dimming a white LED backlight doesn't change its spectrum
much. The actual blue-light fix is the pixel-color substitution above.

---

## 3. Typography

Stock Montserrat (4bpp AA), sizes exactly as enabled — no in-between sizes:

| Size | Use |
|---|---|
| Montserrat 28 | unit letter ("C") next to the hero numeral; quick-action labels |
| Montserrat 24 | side name ("Chris" / partner's name); date on standby face |
| Montserrat 20 | thermal-state word (HOLDING/HEATING/COOLING/OFF), tracked +2px |
| Montserrat 16 | measured/actual-temp caption, AM/PM tag, footers |
| Montserrat 12 | hidden diagnostics face only (build/IP) — never in user-facing UI |
| Montserrat 48 | unused for hero content — too small to be "the" number; reserved for any future list/detail face |

**Custom large font — "Dial Numeral"** (the one 60–90pt budget item):

- Base: a geometric-humanist sans with single-story digits (e.g. Space
  Grotesk Medium) — DIN/Braun-adjacent, not a display/slab face.
- Size: **78px**, weight Medium, **tabular** (fixed digit advance width) so
  neither the clock's minutes nor the setpoint's tenths cause any reflow.
- Character set: `0123456789:.°` only — nothing else, to keep the glyph
  cache small.
- 4bpp AA, generated once, reused verbatim by both hero faces (Standby's
  clock digits and Home's setpoint digits never run at different sizes,
  which is what makes a single font budget entry sufficient for two "hero"
  moments that are never shown simultaneously):

```
lv_font_conv --font SpaceGrotesk-Medium.ttf --size 78 --bpp 4 \
  --format lvgl --lv-include lvgl.h \
  --range 0x30-0x39,0x3A,0x2E,0xB0 \
  -o dial_numeral_78.c
```

---

## 4. HOME FACE — exact layout (360×360, origin top-left, center = 180,180)

Safe interactive zone = circle, radius 150 from center (diameter 300). Rim
band = radius 150→180 (30px), indicators/arcs only, never a tap target.

| # | Element | Position | Size | Color role | Font |
|---|---|---|---|---|---|
| 1 | Side name ("Chris") | box (90,46)–(270,78), centered text | 180×32 | `text-primary` | Montserrat 24 |
| 1b | Identity underline | rect centered (150,80)–(210,83) | 60×3 | `identity-home`/`identity-partner` (solid day; solid/dashed night) | — |
| 2 | Staleness dot | center (300,58) | ⌀10 (r5) | `holding` = fresh, `warning` = stale, **pulses** (scale 100→130→100%, 1s loop) only when stale | — |
| 3 | Boost-active glyph (placeholder) | center (60,58) | 20×20 | `heat-accent`; slot reserved empty (undrawn) when boost inactive | — |
| 4 | Measured/actual temp caption | box (90,86)–(270,106), centered | 180×20 | `text-secondary`; text e.g. "actual 20.8°" | Montserrat 16 |
| 5 | Setpoint hero numeral | box (60,112)–(300,206), centered | 240×94 | `text-primary` (never accent-tinted — legibility over grammar for the biggest element); text e.g. "21.5°" | Dial Numeral 78 |
| 5b | Unit letter "C" | anchored baseline-right of numeral, ~(224,140)–(248,168) | — | `text-secondary` | Montserrat 24 |
| 6 | Thermal-state pill (icon + word) | icon 18×18 at (140,208); word box (162,208)–(268,230) | — | color = current state accent (`heat` / `cool` / `holding` / `standby`); icon: chevron-up (heating) / chevron-down (cooling) / dash (holding) / open ring (off) | Montserrat 20, +2px tracking |
| 7 | Arc — track | center (180,180), r=165, width=16 | start 135°→ end 45° (270° sweep, clockwise, 90° gap centered at bottom) | `track` | — |
| 7b | Arc — indicator | same geometry | angle = `135 + (setpoint − zone.min) / (zone.max − zone.min) × 270`, rounded caps | state accent (matches pill) | — |
| 7c | Ghost tick (measured temp) | radial tick, r=157→173 (16px long × 6px wide) | angle = same formula using measured value | `text-secondary` (deliberately recessive vs. the accent-colored setpoint indicator) | — |
| 8 | Power control | center (180,280), r=44 | ⌀88 (meets ≥72, and the ≥88 primary-action guidance) | disc = `surface`; glyph = `text-primary`; thin 2px inner ring in state-accent color when zone is ON, absent when OFF | icon 32px |
| 9 | Face page indicator | two dots at (172,340) and (188,340), ⌀6, gap 16 | — | filled = current face (`text-secondary`), empty = `track` | — |

Range default is 13–44°C at 0.5°C per detent (bound live to whatever
`min`/`max` the zone reports at connect time — the arc formula above never
hardcodes the bound).

Layout is **identical** between Home and Partner faces except items 1, 1b,
and 9 — see §8. The physical spot you reach for "power" or "warmer" never
moves depending on whose face is up; that consistency matters more than
mirrored handedness for a half-asleep hand.

Occlusion note: the encoder is the **physical rotating bezel**, off-glass —
rotating the knob never covers the touch surface at all. The only on-glass
occlusion is the power tap, and its target sits at the bottom of the safe
zone specifically so the resulting state feedback (numeral, pill, arc) all
land well above the finger's approach path from the bottom edge.

---

## 5. STANDBY FACE — the hero clock

- **Time**: Dial Numeral 78, centered at (180,180), 12-hour format ("11:47"),
  tabular so minute ticks never reflow.
- **AM/PM tag**: Montserrat 16, `text-secondary`, small, right of and below
  the time.
- **Date**: "Tue 9 Jul", Montserrat 20, `text-secondary`, centered ~40px below
  the time block.
- **Ambient state dot**: one 8px dot centered above the time, only signal
  beyond the clock. Colored by the single most "active" state across both
  zones (heat > cool > holding > nothing‑drawn for standby-off-both). This is
  the entire state surface on this face — no numerals, no arc data.
- **Hairline ring**: a permanent, undecorated 1px circle at r=170 in the
  `track` color, present on every face including Standby. It never carries
  data here — it's the object's chassis, the same ring that becomes the lit,
  16px-wide data arc on Home/Partner. The continuity is deliberate: one
  object, several faces, not N unrelated screens.
- **Tap** ("peek"): two small numerals (Montserrat 24, one per occupant's
  current setpoint) slide in from the left and right edges over 180ms,
  hold 900ms, slide back out over 150ms. Nothing is committed — pure peek.
- **Long-press**: same global quick-actions bottom sheet as other faces.
- **Swipe left/right**: jumps straight to Home/Partner (skips the peek).
- **Swipe down**: opens a minimal diagnostics face (Wi-Fi/battery/build) —
  repurposed since Standby is the root and "back" has nowhere else to go.

### Day vs. night

| | Day | Night |
|---|---|---|
| bg | `bg-day` | `bg-night` |
| time | `text-primary-day` | `text-primary-night` (amber — the room's night-light) |
| date | `text-secondary-day` | `text-secondary-night` |
| ring | `track-day` | `track-night` (near-invisible) |
| ambient dot | day state colors | night state colors (never blue) |
| backlight | full/auto | dimmed to a low nonzero floor (comfort only) |

### Wake behavior

The display **never goes fully black**. There is no "screen off" state — only
a day brightness ceiling and a night brightness floor, both nonzero, with the
clock as the permanent resting image (zero wake latency for the 3am reach).
Any touch or the smallest bezel rotation ramps brightness to an intermediate
"night-active" tier (brighter than the floor, still warm/capped, never a full
daytime blast) within ~150ms, holds ~8s after the last input, then decays
back to the night floor.

---

## 6. Transitions & motion

| Trigger | Animation | Duration | Easing |
|---|---|---|---|
| Swipe L/R (Home↔Partner, Standby↔Home) | `LV_SCR_LOAD_ANIM_MOVE_LEFT/RIGHT` | 220ms | ease-out |
| Swipe down (back) | `LV_SCR_LOAD_ANIM_MOVE_BOTTOM` (current face exits downward, revealing what's under it — "sliding back into the clock") | 200ms | ease-out |
| Long-press → quick actions | opaque bottom sheet, 360×160 panel slides up (y: 360→200) | 180ms | ease-out |
| Quick actions dismiss | slides back down (y: 200→360) or 6s timeout | 150ms | ease-in |
| Knob detent (rotation) | arc angle updates **instantly, no tween** — must stay 1:1 with the physical bezel; numeral gets a 100%→104%→100% scale micro-bounce; state pill briefly brightens ~15% and fades | numeral 90ms, pill flash 60ms | — |
| Range-stop (hit min/max) | arc indicator overscoots ~6px past the end and springs back | 140ms | overshoot |
| Power toggle OFF | indicator arc's angular length animates down to zero (collapses to a dot at 135°) | 200ms | ease-in-out |
| Power toggle ON | indicator arc grows from 135° out to the setpoint angle | 200ms | ease-in-out |
| Standby "peek" | two numerals slide in/hold/out (see §5) | 180/900/150ms | ease-out / — / ease-in |
| Clock minute tick | **no animation** — instant digit swap | 0ms | deliberate: a clock doesn't ease |

Full-face swaps run at the platform's ~25–40fps full-screen budget; in-place
Home/Partner updates (arc, numeral, pill) target the ~50fps partial-update
budget.

---

## 7. Haptic vocabulary (DRV2605, ROM Library 6 / LRA)

| Event | Effect ID | Name | Night handling |
|---|---|---|---|
| Detent (per encoder step) | **26** | Sharp Tick 3 – 60% | same ID; global rated-voltage trim ~35% lower |
| Range-stop (hit min/max) | **7** | Soft Bump – 100% | same ID, trimmed |
| Confirm (power toggle, boost engage, long-press accept) | **10** | Double Click – 100% | same ID, trimmed |
| Face-switch arrival (swipe lands) | **25** | Sharp Tick 2 – 80% | same ID, trimmed |
| Error (comms failure, rejected action) | **48** | Buzz 2 – 80% | **not trimmed** — errors stay full-strength at night; they're rare and actionable, so muting them is the wrong trade |

Night attenuation is a single mechanism applied at the dusk/dawn transition,
not a per-event branch: the DRV2605's rated/overdrive-voltage registers are
reprogrammed to a "night" profile (~35% lower output) once, so every effect
ID plays quieter without changing *which* effect fires — the vocabulary's
identity stays intact, only its loudness changes. The error effect is
explicitly excluded from that trim.

The detent effect is chosen from the "Sharp Tick" family specifically because
it's the shortest/crispest in the ROM library and tolerates rapid repeated
firing during a fast spin without the pulses smearing together, unlike the
longer "Click"/"Buzz" families.

---

## 8. Partner-side differentiation

Three redundant, low-cognition signals, deliberately not relying on layout
mirroring (reaching for "power" should always be the same physical gesture
regardless of whose face is up):

1. **Name** — the occupant's first name is always on screen (element #1),
   non-negotiable per the product's own labeling requirement.
2. **Identity underline** (#1b) — a thin 3px rule under the name. Day: solid
   brass (Home) vs. solid pewter (Partner) — two muted, desaturated hues that
   don't collide with the more saturated heat/cool accents. Night: same warm
   family for both (no blue anywhere), differentiated by **pattern** instead
   — solid line for Home, dashed line for Partner. This reuses the exact
   hue→shape substitution principle from §2's night cooling color, applied a
   second time to a second problem — one rule, two jobs.
3. **Face page dots** (#9) — two small dots at the bottom rim; the filled one
   says which face is up, glanceable without reading the name at all.

Rotation is never ambiguous regardless: the interaction model scopes
"rotate = temperature of the visible side" unconditionally, so there's no
failure mode where turning the knob on Partner's face touches Home's zone.

---

## 9. What makes this first-party, not hobbyist

1. **One tabular custom font doing two jobs.** "Dial Numeral" is
   hand-fit (fixed advance width, baseline-tuned colon and decimal point) and
   reused byte-for-byte between the clock and the setpoint — a hobbyist build
   reaches for stock Montserrat 48 and lives with reflow; this doesn't.
2. **One substitution rule, reused three times.** "When hue is unavailable or
   ambiguous, move the signal to value/saturation/shape/pattern" is applied
   identically to night cooling (§2), warning vs. heating (§2), and partner
   identity at night (§8). That's a designed *system*, not three unrelated
   band-aids bolted on as edge cases came up.
3. **A chassis that persists across faces.** The 1px hairline ring is present
   and undecorated on Standby and becomes the lit 16px data arc on Home/
   Partner — the same ring, not a different widget per screen. It's the
   detail that makes the object read as one thing with several faces instead
   of N screens that happen to share a build.

Bonus: every named color above is deliberately RGB565-quantizable (R/B ≡ 0
mod 8, G ≡ 0 mod 4), so the hex in this document is what actually lands in
the framebuffer — no dithering surprises to chase on real hardware.
