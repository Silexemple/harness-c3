# SPEC — Harness C3 (firmware ESP32-C3-MINI-1U + GC9A01 240×240)

Single source of truth for the port of the Autonomous AI "Harness device" dial
(`github.com/autonomous-ai/openharness`, MIT) to low-cost ESP32-C3 hardware.

## 1. Goal & scope

A USB companion display that plugs into a computer running the Harness daemon and shows,
at a glance, what each coding agent is doing: working / waiting for an answer / done,
plus reading and answering agent questions from the device. No Wi-Fi, no account,
no touch, no microphone on this hardware revision.

The wire protocol is the upstream **cable** protocol (binary framing + JSON vocabulary).
Its complete normative description lives in `/mnt/agents/output/harness-c3-spec/PROTOCOL.md`
(extracted from upstream) — **that document wins any disagreement with this one**.

**In scope (v1):** hello/welcome handshake, agent list (active tab), fleet total,
notifications (`notif.replace`), questions (`question` / `question.close` / `answer`),
turn lifecycle for UI status, focus + agent.open, log frames, graceful unknown-message
handling, dual-slot OTA plumbing (partition table + rollback), web flasher.

**Out of scope (v1):** voice (no mic), machine wheel, swarms picker, touch scrollpad,
model/effort picker, accepting daemon-pushed firmware images (upstream images target
ESP32-S3 — accepting them is a brick risk; see §9).

## 2. Hardware target

| Item | Value |
|---|---|
| MCU | ESP32-C3-MINI-1U — RISC-V single core 160 MHz, 400 KB SRAM, 4 MB flash, native USB (USB-Serial-JTAG) on GPIO18(D−)/GPIO19(D+) |
| Display | GC9A01 round 240×240 IPS, SPI write-only (no MISO), RGB565 |
| Input | 2 push buttons, active-low, internal pull-ups |
| Optional | passive buzzer on a spare PWM GPIO |

Default pin map (all overridable in `menuconfig` under *Harness C3 Configuration*):

| Signal | GPIO | Note |
|---|---|---|
| SPI SCK | 4 | |
| SPI MOSI (SDA) | 5 | |
| LCD CS | 2 | strapping pin — CS idles high, safe at boot |
| LCD DC | 3 | |
| LCD RST | 10 | |
| LCD BL | 6 | LEDC PWM, active high |
| BTN_A | 7 | short = next/scroll, long = confirm/yes |
| BTN_B | 8 | short = back, on question = no/cancel. Strapping pin, pulled up — must not be held at boot |
| BUZZER | 1 | optional, `-1` disables |

## 3. Firmware architecture (ESP-IDF v5.5, target `esp32c3`)

```
main/
├── app_main.c        # init order, task wiring, OTA validity confirm, watchdog
├── cable_frame.c/.h  # ADAPTED from upstream (MIT — attribution header kept).
│                     # A5 48 framing, CRC16-CCITT-FALSE, host-compilable (no ESP-IDF deps)
├── cable_link.c/.h   # USB-Serial-JTAG transport: driver install, RX task, 32 KB ring,
│                     # log-framing on/off, host_present()
├── cable_client.c/.h # message layer: hello cadence, session state, agent store (max 8),
│                     # JSON vocabulary (cJSON), unknown-message counters
├── display.c/.h      # esp_lcd GC9A01 SPI panel (40 MHz default, 80 MHz Kconfig option),
│                     # LVGL v9 glue: 2× 240×48 RGB565 DMA draw buffers (~45 KB), flush cb,
│                     # esp_timer tick, backlight LEDC
├── ui.c/.h           # screens (see §6), dark round theme, LVGL task
├── buttons.c/.h      # 10 ms poll, debounce, short/long events → queue to UI task
├── buzzer.c/.h       # optional LEDC beep patterns
└── Kconfig.projbuild # all pins, SPI freq, agent count, buzzer
```

Init order: NVS → display+LVGL → UI (shows boot, then "Not connected") → buttons →
cable_link → cable_client (starts hello cadence) → `esp_ota_mark_app_valid_cancel_rollback()`
once all init succeeded.

RAM budget (400 KB total): LVGL buffers ≈45 KB + LVGL heap 32 KB, decoder 8.2 KB +
RX ring 32 KB, task stacks ≈40 KB, cJSON working set small, ≥80 KB headroom required.
No TLS, no Wi-Fi (`CONFIG_ESP_WIFI_ENABLED=n`), no PSRAM (does not exist on C3).

Partition table (`partitions.csv`, 4 MB, dual OTA):
```
nvs,      data, nvs,     0x9000,   0x6000
otadata,  data, ota,     0xf000,   0x2000
phy_init, data, phy,     0x11000,  0x1000
ota_0,    app,  ota_0,   0x20000,  0x1E0000
ota_1,    app,  ota_1,   0x200000, 0x1E0000
```
App must stay < 1.875 MB: LVGL trimmed (Montserrat 14/20/28 only), no Wi-Fi/TLS stacks.

`sdkconfig.defaults`: `CONFIG_IDF_TARGET="esp32c3"`, 4 MB QIO 80 MHz, CPU 160 MHz,
console = USB_SERIAL_JTAG, FreeRTOS 1000 Hz, task WDT panic 10 s, LVGL color 16 bpp,
dark theme, release optimization.

## 4. Cable protocol implementation rules

- `cable_frame.c/.h` are taken from upstream **verbatim where possible** (MIT);
  they compile with plain gcc for host tests. Keep upstream attribution header + LICENSE note.
- Message layer implements the vocabulary per PROTOCOL.md:
  - **Device→daemon (MUST):** `hello` (product `"harness"`, proto 3, fw version from
    `esp_app_get_description()->version` suffixed `-c3`, device id from MAC),
    `agents.list` after welcome, `answer` (echo `request_id` byte-for-byte, answers object
    built by UI verbatim), `focus`, `agent.open` (reason NULL or `"question"`).
  - **Daemon→device (MUST):** `welcome`, `agents.begin`/`agents.add`/`agents.end`
    (`.total`, `.tab`), `notif.replace`, `question`, `question.close`,
    `turn.started`/`turn.done`/`turn.error`, `fw.offer` (see §9).
  - Unknown `t` or unreadable JSON → increment counters, never drop link, never reboot.
  - Lenient parsing everywhere: missing fields fall back to safe defaults.
- Agent store: array of 8 (screen shows one at a time anyway), fields per PROTOCOL.md
  (id, name, engine, state, summary, machine). `state` values per PROTOCOL.md state machine.
- Session: no `welcome` within hello timeout → stay "Not connected", keep retrying
  (cadence per PROTOCOL.md); link drop (host_present false) → back to "Not connected".

## 5. USB transport rules

- `driver/usb_serial_jtag` install with 32 KB RX ring (matches upstream credit-window math),
  TX with timeout; write failure = "host not draining" = not-connected, never tight-loop.
- ESP_LOG rerouted to LOG frames **only while a session is live**; otherwise plain console
  so `idf.py monitor` still works. ROM/bootloader chatter resync is the decoder's job.

## 6. UI (LVGL v9, 240×240 round, dark, low-saturation)

Screens (adapted from upstream UI_FLOWS.md to 240×240 + 2 buttons):
1. **Boot**: λ glyph + "Harness C3" + fw version.
2. **Not connected**: cable icon + "Brancher sur un PC avec Harness" / "Plug into a Harness daemon".
3. **Home (agent carousel)**: one agent per page — status ring (color by state:
   grey idle / blue running / amber waiting / green done / red error), agent name (Montserrat 20),
   engine + machine (14), last summary line (14, ellipsized). Header: machine name;
   corner badge: fleet total from `agents.end.total`. BTN_A short = next agent
   (sends `focus`), long = `agent.open`; BTN_B = back to home.
4. **Question**: question text (auto-scroll or BTN_A short scrolls), per-option buttons
   rendered as list; BTN_A short = move selection, long = confirm choice → `answer`;
   BTN_B = dismiss without answering (no message). `question.close` clears the screen.
5. **Notification overlay**: `notif.replace` with question=true pulses the status ring amber.
6. **FW update**: not reachable in v1 (offers ignored, §9) — screen exists for future use.

Round-panel rule: keep all critical text inside the inscribed square (~170×170 centered).

## 7. Host tests (`firmware/test/host/`)

- `test_cable_frame.c` (gcc, -std=c11, Wall Werror): CRC known answer
  ("123456789" → 0x29B1), encode→decode roundtrip, resync over garbage prefix,
  truncated frame, oversize payload rejection, **every vector in
  `test/vectors/cable_frame.txt`** (file copied verbatim from upstream; the generator
  script `scripts/gen_cable_vectors.py` must regenerate it byte-identical).
- `test_messages.c`: golden JSON tests — for each device→daemon message our code can
  emit, assert exact `t` string + required field names against PROTOCOL.md table;
  for each daemon→device message, feed a golden JSON (from upstream spec tests)
  into the parser and assert the resulting struct fields.
- `run_tests.sh`: builds both with gcc + vendored cJSON, runs, exits non-zero on failure.
- CI runs this on every push.

## 8. CI / CD (`.github/workflows/build.yml`)

- **job `host-tests`**: ubuntu-latest, gcc + python3 → `firmware/test/host/run_tests.sh`.
- **job `firmware`**: container `espressif/idf:v5.5` → `cd firmware && idf.py set-target esp32c3 && idf.py build` → upload `build/*.bin` artifacts.
- **job `webflasher`** (push to main only, `contents: write`, needs firmware):
  copy `bootloader.bin` (0x0), `partition-table.bin` (0x8000), `ota_data_initial.bin` (0xf000),
  `harness-c3.bin` (0x20000) into `docs/webflasher/bin/`, regenerate `manifest.json`
  (esp-web-tools format, `chipFamily: "ESP32-C3"`), commit back to main.
- **job `release`** (tags `v*`): create GitHub Release with the 4 .bin + `harness-c3-merged.bin`
  (esptool merge-bin for esptool users) + source zip.

## 9. Firmware-update policy (anti-brick)

v1: the device **never answers `fw.offer`** (upstream offers ESP32-S3 images — accepting
would flash wrong-architecture firmware). Log one LOG frame per offer
("fw offer ignored: upstream images target esp32s3"). Dual-OTA partitions + rollback
plumbing are in place so a future C3-aware updater can use them. The bootloader's
chip-id check is the last line of defence.

## 10. Web flasher (`docs/webflasher/`)

- `index.html`: [ESP Web Tools](https://esphome.github.io/esp-web-tools/) button
  (`<esp-web-install-button manifest="manifest.json">`), dark styling, requirements note
  (Chrome/Edge/Opera — WebSerial, HTTPS or localhost), pin-map table, manual-flash
  instructions (esptool.py / idf.py), link to Releases.
- `manifest.json`: generated by CI with real version + file sizes; checked in with
  placeholder version `0.0.0` so local testing works after a local build.
- Activation GitHub Pages : Settings → Pages → Deploy from branch → `main` / `docs`
  (one-time manual click, documented in README).

## 11. Repo layout

```
harness-c3/
├── README.md                 # EN + résumé FR
├── LICENSE                   # MIT + attribution upstream
├── SPEC.md                   # this file
├── PROTOCOL.md               # extracted cable protocol spec
├── firmware/                 # ESP-IDF project
├── docs/webflasher/          # ESP Web Tools page
└── .github/workflows/build.yml
```

## 12. Acceptance criteria

1. Host tests pass (`run_tests.sh` exit 0) in the sandbox.
2. `idf.py build` succeeds for esp32c3 (sandbox ESP-IDF v5.5, else CI is the gate).
3. Verifier subagent confirms protocol fidelity against PROTOCOL.md (field-by-field).
4. Repo `Silexemple/harness-c3` public on GitHub, CI green, webflasher page committed.
