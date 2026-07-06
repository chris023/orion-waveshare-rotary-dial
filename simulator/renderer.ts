/**
 * Canvas renderer for the round 360x360 knob screen. Kept framework-free and
 * close to how the eventual LVGL firmware UI will draw (arcs + labels), so this
 * doubles as the on-device UI design reference.
 */

export interface DialDisplay {
  label: string;
  power: 'on' | 'off' | 'standby';
  target: number | null;
  current: number | null;
  active: boolean;
  offline: boolean;
}

const TAU = Math.PI * 2;
// Gauge sweeps 270°, a gap at the bottom (like a thermostat dial).
const START = Math.PI * 0.75;
const SWEEP = Math.PI * 1.5;

/** Auto-detect the plausible range so the gauge/colour work for °C or °F values. */
function rangeFor(value: number): { min: number; max: number } {
  return value > 45 ? { min: 50, max: 113 } : { min: 10, max: 45 };
}

/** Cold→warm colour: blue (cold) to orange (warm). */
function tempColor(t: number): string {
  const clamp = Math.min(1, Math.max(0, t));
  const cold = [70, 150, 245];
  const warm = [245, 130, 45];
  const c = cold.map((v, i) => Math.round(v + (warm[i] - v) * clamp));
  return `rgb(${c[0]}, ${c[1]}, ${c[2]})`;
}

export function render(ctx: CanvasRenderingContext2D, size: number, d: DialDisplay): void {
  const cx = size / 2;
  const cy = size / 2;
  const r = size / 2;
  ctx.clearRect(0, 0, size, size);

  // Bezel + face.
  ctx.fillStyle = '#0b0d12';
  ctx.beginPath();
  ctx.arc(cx, cy, r, 0, TAU);
  ctx.fill();

  const on = d.power === 'on';
  const hasTarget = d.target !== null;
  const range = rangeFor(d.target ?? d.current ?? 20);
  const norm = hasTarget ? (d.target! - range.min) / (range.max - range.min) : 0;
  const accent = !on ? '#3a4150' : tempColor(norm);

  // Gauge track.
  const gaugeR = r * 0.82;
  ctx.lineWidth = size * 0.05;
  ctx.lineCap = 'round';
  ctx.strokeStyle = '#20242e';
  ctx.beginPath();
  ctx.arc(cx, cy, gaugeR, START, START + SWEEP);
  ctx.stroke();

  // Gauge value arc.
  if (on && hasTarget) {
    ctx.strokeStyle = accent;
    ctx.beginPath();
    ctx.arc(cx, cy, gaugeR, START, START + SWEEP * Math.min(1, Math.max(0, norm)));
    ctx.stroke();
  }

  // Label (zone).
  ctx.fillStyle = on ? '#aeb6c6' : '#5b6270';
  ctx.font = `600 ${Math.round(size * 0.075)}px system-ui, sans-serif`;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(d.label.toUpperCase(), cx, cy - size * 0.24);

  if (d.offline) {
    ctx.fillStyle = '#8b93a3';
    ctx.font = `600 ${Math.round(size * 0.09)}px system-ui, sans-serif`;
    ctx.fillText('OFFLINE', cx, cy + size * 0.02);
    return;
  }

  // Big value.
  if (!on) {
    ctx.fillStyle = '#5b6270';
    ctx.font = `700 ${Math.round(size * 0.16)}px system-ui, sans-serif`;
    ctx.fillText('OFF', cx, cy + size * 0.01);
  } else {
    const big = hasTarget ? `${Math.round(d.target!)}°` : '—';
    ctx.fillStyle = '#f2f5fb';
    ctx.font = `250 ${Math.round(size * 0.28)}px system-ui, sans-serif`;
    ctx.fillText(big, cx, cy + size * 0.02);
  }

  // Current temp + regulating indicator.
  if (d.current !== null) {
    ctx.fillStyle = '#727a8a';
    ctx.font = `500 ${Math.round(size * 0.058)}px system-ui, sans-serif`;
    ctx.fillText(`now ${Math.round(d.current)}°`, cx, cy + size * 0.23);
  }
  if (on && d.active) {
    ctx.fillStyle = accent;
    ctx.beginPath();
    ctx.arc(cx, cy + size * 0.315, size * 0.014, 0, TAU);
    ctx.fill();
  }
}
