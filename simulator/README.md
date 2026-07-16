# Dial UI simulator

Renders the real firmware UI — the unmodified sources in
`firmware/dial-idf/components/dial_ui/` — on your computer, no board
required, into 360x360 circular PNGs.

```bash
cmake -B build -S simulator
cmake --build build
./build/dial_sim
```

PNGs land in `docs/screens/`. LVGL 8.4.0 is fetched automatically unless
`firmware/dial-idf/managed_components/lvgl__lvgl` already exists locally
(force a fresh fetch with `-DDIAL_SIM_FORCE_FETCH_LVGL=ON`).
