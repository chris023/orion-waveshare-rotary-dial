/**
 * Entry point. Wi-Fi is already connected by the Moddable net host (from
 * config.ssid/password) before this runs. Flow:
 *   render OFF -> ensureAuthorized (QR on first boot) -> MCP connect ->
 *   control loop: encoder rotates the setpoint (debounced), touch tap toggles,
 *   long-press boosts; poll device state to refresh the screen.
 *
 * NOTE(hw): the CST816 touch driver (src/cst816.ts) must be wired into Piu's
 * touch input for onTap/onLongPress to fire — see the board/platform config
 * (config.touch). Until then, drive tap/boost from serial for bring-up.
 */

// @ts-expect-error - Moddable native module without bundled typings
import Timer from "timer";
import { ensureAuthorized } from "oauth";
import { Orion, type ZoneState } from "orion";
import { Encoder } from "input";
import { makeUI, type DialView } from "ui";
import { ORION } from "config";
import { clamp } from "zones";

declare function trace(msg: string): void;

const ENCODER_POLL_MS = 60;
const STATE_POLL_MS = 15_000;
const WRITE_DEBOUNCE_MS = 300;

let desiredPower: "on" | "off" = "off";
let desiredTargetF = Math.round((ORION.tempMinF + ORION.tempMaxF) / 2);
let lastState: ZoneState | undefined;
let orion: Orion | undefined;
let writeTimer = 0;

const ui = makeUI({
  onTap: () => void toggle(),
  onLongPress: () => void boost(),
});

function currentView(): DialView {
  return {
    label: ORION.label,
    power: desiredPower,
    targetF: desiredTargetF,
    currentF: lastState?.currentF ?? null,
    active: lastState?.active ?? false,
    relief: lastState?.relief ?? null,
    offline: lastState ? !lastState.online : true,
  };
}
function render(): void {
  ui.setView(currentView());
}

async function toggle(): Promise<void> {
  if (!orion) return;
  desiredPower = desiredPower === "on" ? "off" : "on";
  render();
  try {
    await orion.setPower(desiredPower === "on");
  } catch (e) {
    trace(`setPower failed: ${e}\n`);
  }
}

async function boost(): Promise<void> {
  if (!orion) return;
  desiredPower = "on";
  render();
  try {
    await orion.boost();
  } catch (e) {
    trace(`boost failed: ${e}\n`);
  }
}

function onRotate(detents: number): void {
  if (detents === 0) return;
  desiredPower = "on";
  desiredTargetF = clamp(
    desiredTargetF + detents * ORION.tempStepF,
    ORION.tempMinF,
    ORION.tempMaxF,
  );
  render();
  if (writeTimer) Timer.clear(writeTimer);
  writeTimer = Timer.set(() => {
    writeTimer = 0;
    void flushWrite();
  }, WRITE_DEBOUNCE_MS);
}

async function flushWrite(): Promise<void> {
  if (!orion) return;
  try {
    await orion.setPower(true);
    await orion.setTemperature(desiredTargetF);
  } catch (e) {
    trace(`write failed: ${e}\n`);
  }
}

async function poll(): Promise<void> {
  if (!orion) return;
  try {
    lastState = await orion.getState();
    render();
  } catch (e) {
    trace(`poll failed: ${e}\n`);
  }
}

async function start(): Promise<void> {
  render(); // OFFLINE/OFF until connected

  const auth = await ensureAuthorized((url) => ui.showSetupQR(url));
  ui.clearSetup();

  orion = new Orion(auth);
  await orion.connect();

  lastState = await orion.getState();
  desiredPower = lastState.power;
  if (lastState.targetF !== null) desiredTargetF = lastState.targetF;
  render();

  const encoder = new Encoder();
  Timer.repeat(() => onRotate(encoder.readDetents()), ENCODER_POLL_MS);
  Timer.repeat(() => void poll(), STATE_POLL_MS);
  trace("orion knob ready\n");
}

start().catch((e) => trace(`fatal: ${e}\n`));
