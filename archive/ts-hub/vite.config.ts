import { defineConfig } from 'vite';

// The virtual dial simulator (simulator/) — a hot-reloading, browser-based stand-in
// for the physical Waveshare knob. Vitest uses vitest.config.ts, not this file.
export default defineConfig({
  root: 'simulator',
  server: { port: 5178, strictPort: true },
  build: { outDir: '../dist-simulator', emptyOutDir: true },
});
