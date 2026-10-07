# Changelog

What changed in each release of the dial firmware, in the terms you'd notice
using it. Versions match the `dial-vX.Y.Z` tags and the number the dial shows
under Menu → About.

This file is the source of the release notes on GitHub: the release workflow
extracts the section matching the tag and publishes it as that release's
description, and refuses to publish a tag with no section here. Add the new
section in the same commit that bumps `PROJECT_VER`.

Releases marked **(beta)** were prereleases, visible only to dials with
"Beta builds" turned on.

## 1.6.0 — 2026-10-07

The 1.6.0 stable release, rolling up 1.6.0-beta.1.

### Added

- **A calm "Sign-in expired" screen.** Every so often Orion ends the dial's
  sign-in on its side (the one time we've measured it, that was about 30 days
  after linking), and the dial can't extend it. Until now that moment looked
  exactly like first-time setup: a full-screen QR code appeared out of nowhere,
  which read as "something broke." Now the dial shows a quiet screen titled
  **"Sign-in expired"**, reading "Your bed is still running its schedule.
  Renew whenever you're ready." (it is: the topper keeps following its
  schedule without the dial), with a single **Renew sign-in** button. Renewing
  takes under a minute.
  - The QR code only appears after you tap **Renew sign-in**. Swipe right to go
    back without renewing.
  - If you leave the QR code up and walk away, the dial returns to the calm
    screen on its own after 5 minutes.
  - While it waits, the dial stops contacting Orion and the sign-in relay; it
    only checks for updates if you ask from the menu.
  - Swipe left for the menu (Re-link, Wi-Fi, About, Update) as usual.

### Changed

- **No more "Orion unreachable" flash when the sign-in ended while the dial was
  off.** If your sign-in expired while the dial was unplugged, it now goes
  straight to the calm screen when it starts up.
- **Fewer false alarms at startup.** When the dial's first sign-in check after
  starting up is turned down, it now retries once quietly on "Linking to
  Orion..." instead of flashing "Orion unreachable", and it uses the same
  two-in-a-row rule as the running dial before deciding the sign-in has
  ended.

### Fixed

- **Re-link, Change network and Factory reset could get stuck on the link
  screens** when an update check was still pending. They now go through right
  away.
- **Check for updates now works from the link and Sign-in expired screens.**
  Before, Menu → Update there only gave a buzz and did nothing.
- **The link screens now dim at night like the rest of the dial.** They used
  to stay at daytime brightness all night.

## 1.6.0-beta.1 — 2026-10-07 (beta)

### Added

- **A calm "Sign-in expired" screen.** Every so often Orion ends the dial's
  sign-in on its side (the one time we've measured it, that was about 30 days
  after linking), and the dial can't extend it. Until now that moment looked
  exactly like first-time setup: a full-screen QR code appeared out of nowhere,
  which read as "something broke." Now the dial shows a quiet screen titled
  **"Sign-in expired"**, reading "Your bed is still running its schedule.
  Renew whenever you're ready." (it is: the topper keeps following its
  schedule without the dial), with a single **Renew sign-in** button. Renewing
  takes under a minute.
  - The QR code only appears after you tap **Renew sign-in**. Swipe right to go
    back without renewing.
  - If you leave the QR code up and walk away, the dial returns to the calm
    screen on its own after 5 minutes.
  - While it waits, the dial stops contacting Orion and the sign-in relay; it
    only checks for updates if you ask from the menu.
  - Swipe left for the menu (Re-link, Wi-Fi, About, Update) as usual.

### Changed

- **No more "Orion unreachable" flash when the sign-in ended while the dial was
  off.** If your sign-in expired while the dial was unplugged, it now goes
  straight to the calm screen when it starts up.
- **Fewer false alarms at startup.** When the dial's first sign-in check after
  starting up is turned down, it now retries once quietly on "Linking to
  Orion..." instead of flashing "Orion unreachable", and it uses the same
  two-in-a-row rule as the running dial before deciding the sign-in has
  ended.

### Fixed

- **Re-link, Change network and Factory reset could get stuck on the link
  screens** when an update check was still pending. They now go through right
  away.
- **Check for updates now works from the link and Sign-in expired screens.**
  Before, Menu → Update there only gave a buzz and did nothing.
- **The link screens now dim at night like the rest of the dial.** They used
  to stay at daytime brightness all night.

## 1.5.0 — 2026-09-02

The 1.5.0 stable release, rolling up the 1.5.0 betas. Much of it is the work of
Phil Topness (@topness-msft), landed with thanks.

### Added

- **Rail-push Boost.** From the dial face, keep turning the knob past the cold
  (or hot) end of the arc and hold for a moment: the dial arms, buzzes, and
  starts a Boost — no need to open the Boost screen first. Turn firmly the other
  way to cancel a running one. The Boost length you last chose is remembered.
- **A segmented level ring on the relative scale.** The relative (−10…+10) face
  reads as 21 discrete steps along a cool-to-warm ramp, with the current level
  lit and a thin mark showing where the bed's actual water temperature sits.
- **Tap to switch scales** between the absolute (temperature) and relative
  (−10…+10) faces.

### Changed

- **Linking works from any network now — even cellular.** Pairing used to need
  your phone on the *same* Wi-Fi as the dial: the approval on Orion's site
  redirected back to an address only reachable on your home network, so a phone
  on cellular data, a guest network, or a network that isolates devices would
  just spin forever with nothing on the dial saying why. Approval now comes back
  over the internet instead, so it completes wherever your phone is. The dial no
  longer runs a web server during setup at all — which also retires a class of
  pairing failure where that server ran the dial out of network connections.
- **A quieter off-state.** A side that's turned off settles into a calmer face
  instead of holding a bright, active look.

### Fixed

- **Linking dead-ends and setup traps.** Setup no longer strands you on a screen
  with no way forward, and a re-link started from the link screen no longer
  crashes on the way back.
- **Re-link no longer gets stuck on some upgraded dials.** A dial paired on an
  older version could, after updating, keep trying to finish a re-link against
  the old same-network address and never complete. It now cleanly switches over.
- **The schedule loads correctly at boot.**

> Linking is now reliable and network-independent. It does **not** change how
> *often* you're asked to sign in — we haven't yet root-caused that ~weekly
> cadence, so it may not go away with this update.

## 1.5.0-beta.2 — 2026-09-02 (beta)

### Changed

- **Linking works from any network now — even cellular.** Pairing used to need
  your phone on the *same* Wi-Fi as the dial: the approval on Orion's site
  redirected back to an address only reachable on your home network, so a phone
  on cellular data, a guest network, or a network that keeps devices from
  talking to each other would just spin forever with nothing on the dial saying
  why (a real problem people hit — most recently on a dial that had roamed onto
  a weak, far corner of the Wi-Fi). Approval now comes back over the internet
  instead, so it completes wherever your phone is. The dial no longer runs a
  little web server during setup at all — which also retires a class of pairing
  failure where that server ran the dial out of network connections mid-link.
- **Clearer wording on the scan screen.** The hint under the QR code and its
  "still waiting" note no longer tell you to get on a particular Wi-Fi network,
  because they no longer need to.

### Fixed

- **Re-link no longer gets stuck on some upgraded dials.** A dial that had
  paired on an older version could, after this update, keep trying to finish a
  re-link against the old same-network address and never complete. Re-linking
  now cleanly switches such a dial over to the new flow.

> This makes each sign-in reliable and network-independent. It does **not**
> change how *often* you're asked to sign in — and we haven't yet root-caused
> that ~weekly cadence, so it may not go away with this update.

## 1.5.0-beta.1 — 2026-08-31 (beta)

Community-driven: most of this release is the work of Phil Topness
(@topness-msft), landed with thanks.

### Added

- **Rail-push Boost.** From the dial face, keep turning the knob past the cold
  (or hot) end of the arc and hold for a moment: the dial arms, buzzes, and
  starts a Boost — no need to open the Boost screen first. Turn firmly the
  other way to cancel one that's running. The Boost length you last chose is
  remembered.
- **A segmented level ring on the relative scale.** The relative (−10…+10) face
  now reads as 21 discrete steps along a cool-to-warm colour ramp instead of one
  continuous arc, with the current level lit and a thin mark showing where the
  bed's actual water temperature sits.
- **Tap the number to switch scales.** Tapping the big temperature numeral
  toggles between the absolute (°F) and relative (level) scales — the same
  choice under Settings → Scale.

### Changed

- **A quieter "off" face.** When a side is switched off, the dial no longer
  shows live controls: the arc and numeral fade back and the power button
  breathes, so an off side reads as off at a glance — and the knob won't quietly
  change a setting on a side that isn't running.

### Fixed

- **Linking no longer dead-ends.** Right after you approved the dial on your
  phone, it could get stuck showing the QR code again — the secure token
  exchange couldn't get a network socket and failed silently. The dial now frees
  the setup server's sockets first and completes the link reliably. (This is
  behind some "have to keep signing in" reports, where the sign-in itself
  wouldn't take.)
- **The setup screen is no longer a trap.** From the link screen you can swipe
  to the menu and open Wi-Fi settings, and "Change network", "Re-link", and
  "Factory reset" take effect immediately during setup instead of waiting out
  the QR window. If a link fails, the dial says why instead of just showing the
  code again.
- **No more surprise restart while re-linking.** A rare crash that rebooted the
  dial while it sat on the link screen is fixed.
- **Tonight's schedule loads at boot**, not about half an hour later.

The absolute face keeps its water-level wash.

## 1.4.2 — 2026-08-05

One beta cut, verified on hardware, promoted to stable:

### Fixed

- **The Rotation setting works again.** Tapping Rotation in Settings had
  been refusing with an error buzz and an unchanged value: 1.4.0's
  anti-tearing work quietly grew the memory that turning to 90° or 270°
  requested at the moment of the tap, past what the dial has left once
  Wi-Fi is up. The rotation now reserves a smaller working buffer once at
  power-on, so the setting can't run out of memory mid-life. A dial already
  sitting on a rotated screen keeps its orientation exactly as before.

## 1.4.2-beta.1 — 2026-08-05 (beta)

### Fixed

- **The Rotation setting works again.** Tapping Rotation in Settings had
  been refusing with an error buzz and an unchanged value: 1.4.0's
  anti-tearing work quietly grew the memory that turning to 90° or 270°
  requested at the moment of the tap, past what the dial has left once
  Wi-Fi is up. The rotation now reserves a smaller working buffer once at
  power-on, so the setting can't run out of memory mid-life. A dial already
  sitting on a rotated screen keeps its orientation exactly as before.

## 1.4.1 — 2026-08-05

### Changed

- **The Brightness rows are now Day, Night (in use), and Night (clock)** —
  after a report of someone dimming "Night" to 0 and wondering why the clock
  kept glowing. The two night settings were too easy to mistake for each
  other: Night (in use) is the dial while you're actually using it at night;
  Night (clock) is the clock face that glows while you sleep. The pickers'
  captions now say the same thing, so a wrong tap corrects itself.
- **Night (clock) at zero now reads "Off"** — in the Brightness menu and in
  the picker — because that's what it is: the standby clock goes fully dark,
  and a touch or a turn of the knob still wakes the dial.

## 1.4.0 — 2026-08-04

### Added

- **Boost from the dial face.** A snowflake and a flame now sit at either end
  of the temperature arc — tap one to start a timed cool or heat boost. They
  respond the instant you tap, rather than waiting on the bed to answer.
- **A status chip** above the power button, telling you what your last change
  actually does: "Holding" if it stays put until you touch it again, or
  "Until 6:30" if the schedule takes over at its next step. During a boost it
  shows when the boost ends, with an ✕ to cancel. Tapping it opens the
  Schedule/Hold picker.
- **Long-press the power button** to switch between Schedule and Hold without
  going through Settings.
- **Screen timeout** in Settings — how long the dial waits before the standby
  clock takes over: 5s, 15s, 30s, or 1m.
- **An "Update available" line** on the dial and standby faces, so an update
  is discoverable without going looking for it. Hidden at night.

### Changed

- **The screen no longer sleeps while you're in the middle of something.**
  Joining a network, typing a password, checking for an update, installing
  one, reading the update offer — all of these hold the display awake, and
  the idle countdown starts fresh when the task finishes rather than while it
  was still running.
- **Update checks run every 6 hours** instead of once a day, spread out
  across devices so they don't all ask GitHub at the same moment.
- **The update offer now appears when you wake the screen** — at most once a
  day, never during your sleep window. The previous conditions were tight
  enough that for many dials it would realistically never have appeared at
  all.
- **Auto-update keeps the screen dark** while it installs unattended, and no
  longer waits for both sides of the bed to be off. That condition protected
  nothing — the dial is a remote control, not the bed's controller — and
  quietly meant auto-update never ran for anyone who uses their topper during
  the day.
- **Temperature limits come from Orion** rather than the firmware's own
  assumed 55 °F floor, so the dial's range matches what your topper actually
  supports.
- **A side that is off with a schedule starting later today** now reads
  "⏸ Until 8:00" instead of "Holding", which was misleading — nothing was
  being held.
- The quick-actions screen (long-press anywhere on the dial face) is gone;
  Away mode moved into Settings, and boost moved to the arc.
- Turning the knob is silent. The encoder's mechanical detents are the
  feedback; the motor only fires at the end of a range.

### Fixed

- **Cancelling a boost could switch your side of the bed off**, instead of
  returning it to the temperature it was at before the boost started.
- **Text on the dial could tear** while turning the knob — most visible on
  the large temperature numerals.
- The display could sleep mid-task when two things were holding it awake at
  once and one of them finished.

## 1.3.0 — 2026-07-29

### Added

- **Night clock brightness**, separate from the other two. The standby clock
  is what glows in a dark bedroom all night, so how bright it is has nothing
  to do with how bright the dial should be when you pick it up. Settings →
  Brightness is now Day / Night / Night clock.

### Changed

- **All three brightness levels are 0–100% in 1% steps** and can be set by
  dragging as well as turning — the same touch handle the temperature dial
  uses. Turning accelerates when you spin fast, so the full range takes about
  a second while single detents still land exactly.
- **Day and Night now reach 0%.** The night clock has its own curve rather
  than a scale factor: it was previously three indistinguishable values
  across the whole slider, a control whose entire travel was invisible. 0 is
  genuinely off — touch or a detent still wakes the dial.
- **Fresh dials start at Day 30%, Night 0%, Night clock 20%.**
- **Haptics default to Low**, and the knob no longer buzzes on every detent.

## 1.2.0 — 2026-07-29

Four beta cuts, verified on hardware, promoted to stable. On top of 1.1.0:

### Added

- **An update prompt** that can only appear when interrupting is harmless —
  never in the sleep window, never mid-adjustment, once a day at most.
- **Auto-update** (off by default), installing in the quiet window after your
  scheduled wake time. Two failed installs of the same version stop further
  attempts, so a bad release can't put a dial into a nightly reboot loop.
- **Skip this version** and an **Installed** row on the Update screen.
- **Adjustment mode gets its own screen**, with prose that changes with your
  choice — a setting whose effect lands hours later can't be explained by a
  label and a one-word value.

### Changed

- **The dial writes tonight's schedule instead of holding a fixed
  temperature.** Turning the knob adjusts the *current* phase, so the
  schedule's later steps still fire — matching what the phone app does.
  Hold mode keeps the old behavior.
- **Update moves out of About into its own menu entry**, now the single place
  update behavior is controlled. Brightness collapses into a submenu.
- **Haptics gain Off / Low / High / Auto** and default to Low. Auto is firm
  by day, soft during your sleep window.
- The Tonight face is gone; its schedule data still drives the write path.

### Fixed

- **Linking could die mid-handshake, and update checks could return
  "HTTP -1" forever.** Both were the same thing: the dial ran out of internal
  memory whenever two secure connections overlapped. TLS buffers now come
  from the 8 MB PSRAM, which also cut handshake time roughly in half.
- The beta channel could never complete a check once the project had a dozen
  releases — the response outgrew its buffer.
- Downloads retry once before reporting failure, failed checks clear
  themselves instead of sticking until a power cycle, and a check now shows
  that it's working rather than looking like a dead button.

## 1.2.0-beta.4 — 2026-07-29 (beta)

### Changed

- The Update screen reads top to bottom as the decision it exists for: is
  there an update, what do I have, do I want to refuse this one, how should
  this behave in future. "Check for updates" takes the slot the knob opens
  on, which had been held by an inert info row.
- "Skip this version" no longer exists when there's nothing to skip (it was
  permanent and inert, which reads as broken), and it names the version it
  would refuse.

## 1.2.0-beta.3 — 2026-07-29 (beta)

### Added

- The update prompt and auto-update, per `docs/SPEC-update-prompt.md`.
  "Later" defers 23 hours rather than 24, so the prompt walks around the
  clock instead of pinning itself to whatever moment was inconvenient the
  first time.

### Fixed

- OTA downloads retry once before reporting failure, and both HTTP clients
  release their sockets first so the downloader isn't competing with them for
  memory.

## 1.2.0-beta.2 — 2026-07-29 (beta)

### Fixed

- **The beta channel could never complete an update check.** GitHub's release
  list had grown past the dial's 64 KB response cap; the request is now
  bounded to the newest few releases, which are the only ones that could win
  the comparison anyway.

### Changed

- Haptics default to Low. Devices upgrading from the old on/off setting land
  on Low with no migration; anyone who deliberately picked Auto keeps it.

## 1.2.0-beta.1 — 2026-07-29 (beta)

### Changed

- **Menu restructure.** Menu is Back / Settings / Update / Wi-Fi / About, with
  the knob opening on the row people actually use. Settings leads with
  Adjustment mode and Brightness, then the set-once display preferences, with
  the destructive rows last.
- "Temp until" becomes its own **Adjustment mode** screen.
- The Tonight face is removed; its schedule data layer stays.

### Fixed

- **"Check for updates" returned HTTP -1 whenever the dial was linked** — the
  build was allocating every TLS buffer from scarce internal RAM, so a second
  concurrent connection couldn't get memory and died mid-handshake. Buffers
  now come from PSRAM.
- A tap on "Check for updates" shows "Checking…" for the duration instead of
  looking like nothing happened.

## 1.1.0 — 2026-07-28

### Added

- **Adjustment mode: Schedule (default) or Hold.** Turning the dial used to
  hold that temperature for the rest of the night, while the phone app
  adjusts the current phase of tonight's schedule so the later phases still
  fire. The dial now does what the app does. Every precondition that isn't
  met falls back to the old behavior — a missed override is an annoyance, a
  wrong temperature at 3am is not.
- **A beta channel.** "Beta builds" on the Update screen. Beta dials graduate
  onto matching stable by themselves and never downgrade. The web flasher
  gets an opt-in checkbox.
- **Haptics: Off / Low / High / Auto** over two real motor profiles.
- **Update and Brightness become submenus.** About returns to what its name
  promises.

### Fixed

- **Onboarding's worst trap:** the pairing approval has to reach the dial on
  your LAN, so a phone on cellular or a guest network could never deliver it
  — and the dial said nothing, leaving an endless spinner on the phone. The
  consent screen now names the network to scan from, and after 45 quiet
  seconds offers a dismissible explanation of the actual cause.
- **A DHCP lease change could silently force a QR re-link.** The pairing
  callback used the dial's IP address; it now uses a stable
  `orion-dial-xxxxxx.local` name.
- A stack that was too small to add two sub-screens without boot-looping the
  dial — caught on hardware before release.

## 1.0.11 — 2026-07-28

### Fixed

- **A dial that sat on "Orion unreachable" for hours, where no reboot,
  re-link, or reflash helped.** The server admits one connection per device
  at a time and the dial had two HTTP clients fighting over that slot; each
  now keeps a single connection and hands it back before the other needs it.
  Verified on hardware: hours of failure became linking in seconds.
- A connect-failure loop could exhaust the display memory and crash-reboot
  the dial.
- **A dial with no device state is no longer trapped on the error screen** —
  Re-link, Wi-Fi change, and software update are the tools that fix a stuck
  dial, and must be reachable from one.
- A failed update check is no longer permanent until a power cycle.
- Connection retries pace themselves more gently, since bursts of handshakes
  are what connection-rate filters punish.

## 1.0.10 — 2026-07-28

### Fixed

- **A good update could silently roll back.** The new firmware marked itself
  valid only after a successful connection to Orion — 30–60s after reboot,
  and dependent on Wi-Fi, TLS and pairing all succeeding. A power cycle
  inside that window reverted it. It now confirms itself once the system has
  demonstrably booted healthy. While unconfirmed, the dial says "Finalizing
  update — keep powered" rather than keeping it a secret.

### Changed

- **Updates are findable.** When one is available the main menu grows an
  "Install *version*" row, instead of the control living only behind About.
- **Brightness gets a full-screen picker** — big numeral, rim arc, knob
  detents, and the backlight previewing live as you turn. Leaving the screen
  is the commit.

## 1.0.9 — 2026-07-28

### Changed

- **This device may outlive its maintainer,** so 1.0.9 hardens the one
  failure an update can't heal: expired TLS trust anchors, since updates need
  TLS too. The embedded roots go from 9 to 18, each one verified
  cryptographically against a root already trusted rather than on the
  strength of a download. The earliest load-bearing expiry is now 2031.
- A new monthly CI check verifies the live certificate chains against those
  anchors and fails a year before any of them would strand a device.
- **Certificate failures now say so:** "Secure connection failed / This
  firmware may be too old", instead of the generic "Orion unreachable". A
  dial rebooting years from now explains itself.

## 1.0.8 — 2026-07-28

### Added

- **Day and Night brightness settings** (community request), scaling the
  built-in backlight tables. No setting can black out the panel.

### Fixed

- **A dial that worked and then stopped working, permanently** (community
  report). A pairing token that dies in normal use — expired, revoked, or
  out of sync — used to strand the dial forever with every control silently
  dead and no way back. It now recognizes a genuinely dead token and returns
  to the pairing QR by itself, while a passing network blip no longer forces
  re-pairing.

## 1.0.7 — 2026-07-21

### Added

- **Relative temperature scale.** Read the setpoint as Orion's own −10…+10
  levels instead of a temperature. Fresh dials default to relative; dials
  upgrading keep absolute, so an unattended update never changes what the big
  number means.

### Fixed

- **Swiping between screens could drag the temperature.** The arc is now
  display-only, with a discrete draggable handle at the setpoint as the sole
  temperature target — so swiping and setting are mutually exclusive by where
  your finger starts.

## 1.0.6 — 2026-07-17

### Fixed

- **Fresh setup no longer sits through "Orion unreachable" cycles** before
  the pairing QR appears. The steps that must finish before the QR can be
  built now retry quickly and quietly through the post-Wi-Fi warmup, falling
  back to slow retries only if Orion is genuinely down.
- The pairing success page in your phone's browser shows its checkmark
  instead of garbled characters.

## 1.0.5 — 2026-07-17

### Changed

- **Installing an update takes over the screen** — ring, percent, "Keep the
  dial plugged in", then "Restarting…" — consuming input until the dial
  reboots.

### Fixed

- A confirmed install could look ignored, inviting a second confirm.

## 1.0.4 — 2026-07-17

### Added

- **Flash the dial from your browser.** A GitHub Pages installer page using
  ESP Web Tools, so the first flash needs no toolchain — plug in over USB-C
  and click Install.

### Changed

- Documentation reframed around browser flashing, with the build-from-source
  path folded away for developers.

## 1.0.3 — 2026-07-16

### Fixed

- **Updates couldn't download at all.** GitHub redirects release assets to a
  long signed URL that didn't fit in the dial's request buffer, so every
  install failed before downloading a byte.

## 1.0.2 — 2026-07-16

### Fixed

- Long status text ("Tap again to confirm", download progress, errors) drew
  straight through the row labels behind it. Rows now stack their status on
  their own line.
- Manual update checks wait for the clock to sync instead of silently
  proceeding, and say so.

## 1.0.1 — 2026-07-16

### Changed

- The network picker centers its rows like the rest of the menus.
- The pairing QR screen joins the dark palette, with the code itself kept on
  a white card so phone cameras still see real contrast at night.

### Fixed

- Version numbering: every development build had also reported 1.0.0, so
  updates could never be offered until the number moved.

## 1.0.0 — 2026-07-15

First public release. A standalone bedside dial for an Orion Sleep dual-zone
topper: Wi-Fi setup and Orion pairing happen on the device, and it talks to
Orion directly — no phone app, no hub, no cloud service to run yourself.
