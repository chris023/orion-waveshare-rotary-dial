/**
 * Round-dial UI in Piu. Mirrors the simulator's renderer.ts (the visual spec):
 * zone label, big target °F, gauge arc, current temp, ⚡ boost, plus a setup mode
 * that shows a QR code for the one-time OAuth consent.
 *
 * The app/touch/state plumbing here is solid; the pixel drawing is intentionally
 * minimal (fillColor + drawString) and marked TODO(hw) — port the exact look from
 * simulator/renderer.ts once the display is up (arcs via commodetto/outline).
 */

import {} from "piu/MC";
// @ts-expect-error - Moddable data module without bundled typings
import qrCode from "qrcode";
// @ts-expect-error - Moddable native module without bundled typings
import Time from "time";
import { ORION } from "config";

const LONG_PRESS_MS = 600;

// Piu constructors are globals provided by the piu manifest. Typed loosely here
// because @moddable/typings coverage of Piu varies by version.
declare const Application: any;
declare const Skin: any;
declare const Style: any;
declare const Port: any;
declare const Behavior: any;

export interface DialView {
  label: string;
  power: "on" | "off";
  targetF: number | null;
  currentF: number | null;
  active: boolean;
  relief: "heat" | "cool" | null;
  offline: boolean;
}

export interface UI {
  setView(view: DialView): void;
  showSetupQR(url: string): void;
  clearSetup(): void;
}

const BLACK = 0x000000;
const WHITE = 0xf2f5fb;
const MUTED = 0x727a8a;
const ACCENT = 0xf5824d;

export function makeUI(handlers: { onTap: () => void; onLongPress: () => void }): UI {
  let view: DialView = {
    label: ORION.label,
    power: "off",
    targetF: null,
    currentF: null,
    active: false,
    relief: null,
    offline: true,
  };
  let setup: { size: number; bits: Uint8Array } | undefined;

  const behavior = new Behavior();
  // Distinguish tap from long-press by press duration. VERIFY: Piu touch event
  // names/signatures (onTouchBegan/onTouchEnded) on your SDK version.
  let pressStart = 0;
  behavior.onTouchBegan = (): void => {
    pressStart = Time.ticks;
  };
  behavior.onTouchEnded = (): void => {
    if (Time.ticks - pressStart >= LONG_PRESS_MS) handlers.onLongPress();
    else handlers.onTap();
  };
  behavior.onDraw = (port: any): void => {
    const w = port.width;
    const h = port.height;
    port.fillColor(BLACK, 0, 0, w, h);
    const cx = w >> 1;

    if (setup) {
      drawQR(port, setup.bits, setup.size, w, h);
      port.drawString("Scan to link Orion", port.style, MUTED, 0, h - 28, w, 24);
      return;
    }

    // TODO(hw): match simulator/renderer.ts — gauge ring + value arc via
    // commodetto/outline, coloured by temperature; below is a minimal readout.
    port.drawString(view.label.toUpperCase(), port.style, view.power === "on" ? WHITE : MUTED, 0, h * 0.18, w, 32);

    if (view.offline) {
      port.drawString("OFFLINE", port.style, MUTED, 0, cx, w, 40);
      return;
    }
    const big = view.power === "off" ? "OFF" : view.targetF === null ? "--" : `${Math.round(view.targetF)}°`;
    port.drawString(big, port.style, view.power === "on" ? WHITE : MUTED, 0, h * 0.36, w, 96);

    if (view.currentF !== null) {
      port.drawString(`now ${Math.round(view.currentF)}°`, port.style, MUTED, 0, h * 0.7, w, 24);
    }
    if (view.relief) {
      port.drawString("⚡ BOOST", port.style, ACCENT, 0, h * 0.78, w, 24);
    }
  };

  const screen = new Port(null, {
    left: 0,
    right: 0,
    top: 0,
    bottom: 0,
    active: true,
    Behavior: () => behavior,
    Skin: new Skin({ fill: BLACK }),
    Style: new Style({ font: "600 28px Open Sans", horizontal: "center", vertical: "middle" }),
  });

  const application = new Application(null, {
    displayListLength: 8192,
    touchCount: 1, // wired to the CST816 driver via the platform config
    contents: [screen],
    skin: new Skin({ fill: BLACK }),
  });
  void application;

  const invalidate = (): void => screen.invalidate();

  return {
    setView(next: DialView): void {
      view = next;
      invalidate();
    },
    showSetupQR(url: string): void {
      const code = qrCode({ input: url, maxVersion: 8 }) as ArrayBuffer & { size: number };
      setup = { size: (code as unknown as { size: number }).size, bits: new Uint8Array(code) };
      invalidate();
    },
    clearSetup(): void {
      setup = undefined;
      invalidate();
    },
  };
}

/** Draw a QR code (ArrayBuffer of 0/1 cells) centered, scaled to fit. */
function drawQR(port: any, bits: Uint8Array, size: number, w: number, h: number): void {
  const scale = Math.max(1, Math.floor((Math.min(w, h) * 0.7) / size));
  const dim = size * scale;
  const ox = (w - dim) >> 1;
  const oy = (h - dim) >> 1;
  port.fillColor(WHITE, ox - scale, oy - scale, dim + 2 * scale, dim + 2 * scale);
  for (let y = 0; y < size; y += 1) {
    for (let x = 0; x < size; x += 1) {
      if (bits[y * size + x]) port.fillColor(BLACK, ox + x * scale, oy + y * scale, scale, scale);
    }
  }
}
