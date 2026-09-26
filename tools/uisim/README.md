# UI simulator (host)

Compiles the **real** `firmware/main/ui.c` against LVGL v9.2's software
renderer and dumps exact 240×240 frames as PPM images — the same pixels the
GC9A01 shows, without flashing a device. This is how the README screen
mockups (`docs/assets/screens/`) were produced.

```
./build.sh /tmp/frames     # fetch deps (once), build, run the scenario
```

| File | Screen |
|---|---|
| `01_boot.ppm` | boot splash (λ, version) |
| `02_offline.ppm` | not connected |
| `03_home_running.ppm` | home carousel — running agent (blue ring) |
| `04_home_waiting.ppm` | home — waiting agent (amber ring) |
| `05_question_single.ppm` | question screen, single choice |
| `06_question_multi.ppm` | question screen, multi-select + Done row |
| `07_home_done_toast.ppm` | home — done agent (green ring) + toast |
| `08_home_empty.ppm` | home — empty tab (no staged agent) |

What is real and what is simulated:

- **Real**: `ui.c` unmodified (every layout, color, font, string), LVGL v9.2
  with the device's trimmed configuration (`lv_conf.h` mirrors
  `sdkconfig.defaults`: RGB565, 32 KB pool, Montserrat 14/20/28 only), same
  240×48×2 draw-buffer geometry.
- **Simulated**: the cable_client backend (`sim_cable.c` — an in-memory
  agent/question store driven by the scenario) and the platform
  (`sim_freertos.c` — FreeRTOS queues/tasks over pthreads, display lock,
  buzzer). The scenario in `sim_main.c` fires the same UI callbacks the link
  reader task fires on device.

PPM is trivial to convert: `convert 01_boot.ppm 01_boot.png`, or Pillow
(`Image.open`). The `docs/` render pipeline masks the square frame into the
panel's circle and adds the device bezel.
