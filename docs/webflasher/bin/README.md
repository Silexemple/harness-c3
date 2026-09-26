# Web flasher binaries

This folder holds the four flash images served by the ESP Web Tools page
(`docs/webflasher/index.html` + `manifest.json`):

| File | Flash offset |
|---|---|
| `bootloader.bin` | `0x0` |
| `partition-table.bin` | `0x8000` |
| `ota_data_initial.bin` | `0xf000` |
| `harness-c3.bin` | `0x20000` |

They are published automatically by the `webflasher` CI job on every push to
`main`. You can also publish them manually: build with `idf.py build`
(ESP-IDF v5.5, target `esp32c3`) and copy the four files from
`firmware/build/` here (bootloader and partition-table are under
`firmware/build/bootloader/` and `firmware/build/partition_table/`).
