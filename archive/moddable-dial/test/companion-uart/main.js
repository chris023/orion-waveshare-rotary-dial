/*
 * Companion-UART probe — is the knob relayed from the companion ESP32?
 *
 * Per the Waveshare schematic, the S3<->companion UART is S3 GPIO48 (RX, from
 * companion IO23) / GPIO38 (TX, to companion IO18). If the single physical knob
 * is EC2 (on the companion) rather than EC1 (S3 GPIO8/7 — which read dead), the
 * companion's factory firmware likely streams encoder events to the S3 here.
 *
 * We open GPIO48 as a real UART @115200 and log any bytes received while the
 * knob turns. Clean bytes correlated with rotation => the knob is companion-
 * relayed (read it here). Nothing => companion isn't streaming unsolicited.
 */

import Serial from "embedded:io/serial";
import Timer from "timer";

let total = 0;

const uart = new Serial({
  port: 1, // UART1 (UART0 is the console; default was "port in use")
  receive: 48, // S3 RX  (ESP32S3_RX net, from companion IO23)
  transmit: 38, // S3 TX  (ESP32S3_TX net, to companion IO18)
  baud: 115200,
  format: "buffer",
  onReadable(count) {
    const bytes = new Uint8Array(this.read());
    total += bytes.length;
    let hex = "";
    for (let i = 0; i < bytes.length && i < 24; i++) {
      hex += bytes[i].toString(16).padStart(2, "0") + " ";
    }
    trace(`RX ${bytes.length}B: ${hex}(total ${total})\n`);
  },
});

trace("companion UART @115200 on GPIO48 (RX) — turn the knob\n");
Timer.repeat(() => {
  trace(`hb rx-total=${total}\n`);
}, 1000);
