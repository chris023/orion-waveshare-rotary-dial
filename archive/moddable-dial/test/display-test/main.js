/*
 * ST77916 bring-up — STATIC, high-contrast test pattern.
 *
 * Four fixed horizontal stripes, top -> bottom: WHITE, RED, GREEN, BLUE, on a
 * black background. No cycling, no timers touching the screen — so what you see
 * is unambiguous. This resolves two questions at once:
 *   - Is the pixel pipeline working at all? (do distinct stripes appear?)
 *   - How are colors mapped? (report which stripe looks which color — that tells
 *     me the byte order (BE/LE) and whether the 0x21 inversion is right.)
 *
 * Backlight: LEDC PWM on GPIO47 at high duty (the panel's backlight needs a PWM
 * signal, not static DC). Module-scope reference so it isn't GC'd.
 */

import {} from "piu/MC";
import PWM from "pins/pwm";

const backlight = new PWM({ pin: 47 });
// ~50% duty. At 93% the panel showed cross-hatch + fade (backlight current likely
// sags a shared rail); 50% earlier kept the backlight steady. Testing that here.
backlight.write(512);

const stripe = (top, color) =>
  new Content(null, {
    left: 0,
    right: 0,
    top,
    height: 90,
    skin: new Skin({ fill: color }),
  });

const application = new Application(null, {
  skin: new Skin({ fill: "black" }),
  contents: [
    stripe(0, "white"),
    stripe(90, "red"),
    stripe(180, "green"),
    stripe(270, "blue"),
  ],
});
trace(`display ${application.width} x ${application.height} — static WRGB stripes\n`);
