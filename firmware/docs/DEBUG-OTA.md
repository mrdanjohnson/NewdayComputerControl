# DEBUG-OTA — signed dual-partition OTA (spec 15.3)

Notes for the next person debugging OTA on the bench. Feature design lives in
`docs/PHASE6.md`; the endpoint contract in `lib/maccontrol_core/mc_openapi.cpp`
(amendment note); this file is only the non-obvious operational findings.

## How rollback is enabled (and how to re-verify)

`framework = arduino` builds link the IDF and the bootloader **precompiled**
from `framework-arduinoespressif32`; `board_build.sdkconfig` is only consumed
by the espidf builder, so `firmware/sdkconfig.defaults` is a project-level
record, not an input to the arduino-only build. That is safe because the
Arduino core 2.0.17 sdkconfig that built the shipped bootloader already has
rollback on. To re-verify after any framework/platform upgrade:

```bash
grep ROLLBACK ~/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/sdkconfig
# expect: CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y  +  CONFIG_APP_ROLLBACK_ENABLE=y
strings ~/.platformio/packages/framework-arduinoespressif32/tools/sdk/esp32s3/bin/bootloader_qio_80m.elf | grep ESP_OTA_IMG_PENDING_VERIFY
```

If either check fails after an upgrade, the bootloader no longer reverts
pending slots and a mixed `arduino,espidf` build (with
`board_build.sdkconfig`) becomes mandatory — do not ship OTA until rollback is
proven again (flash a bad image via a deliberately unsigned upload + apply and
confirm the device comes back on the previous version).

## Update.begin erase vs the task watchdog

`Update.begin(size, U_FLASH)` erases the target slot inside the call
(64 KB-block erase, seconds for a 1.4 MB image) and cannot feed the TWDT
while doing so. The 10 s task watchdog has headroom in practice, but a slow or
worn flash could in theory trip it — the failure mode is a WDT reboot mid-
upload, which leaves the slot `ABORTED` (never bootable) and the client gets a
dropped connection, not a 400. Retry the upload. If this ever fires in the
field, the fix is erasing in bounded chunks (raw `esp_ota_begin/write` with a
pre-erase loop that feeds the WDT), not a longer TWDT.

## Signature is verified AFTER the write (deviation, RAM-driven)

Spec 15.3 draws verification before the flash write. A 1.4 MB image cannot be
buffered on 320 KB RAM (AGENTS.md), so the shipped flow streams to the
inactive slot while hashing incrementally, then verifies the ECDSA signature
over the digest BEFORE the 200 and calls `Update.abort()` on failure so the
slot is not bootable. Same guarantee (a bad image never runs; the running
image is never erased), different order. The host-side gate is
`scripts/ota_sign.py verify`; the device gate is `verifySignature()` in
`src/ota.cpp`.

## Partition-table switch orphans LittleFS

`partitions_maccontrol.csv` moves the LittleFS/spiffs region from 0x670000
(stock `default_8MB.csv`) to 0x5E0000. Flashing the new table via a full
`esptool write_flash` (what `pio run -t upload` does NOT do for the partition
table — PlatformIO flashes only app+bootloader; the table goes in only when
the whole flash image is rewritten, e.g. `pio run -t upload` with
`board_build.partitions` change DOES rewrite... see below) — **any full
re-flash with the new table invalidates the existing LittleFS superblock**,
losing API keys, pairing record (NVS survives: same offset 0x9000), macros,
triggers and the ledger. Re-provision keys/pairing after the first flash of
the new table, or back up (`scripts/serial_cli.py` has no FS dump — use
`esptool read_flash 0x670000 0x180000` BEFORE migrating and restore to the new
offset).

## Confirmation semantics (AT-relevant)

The post-apply self-check confirms on the **first successful
`GET /api/v1/status` serve** (spec 15.3 allows "automatic on first successful
status serve" as the explicit confirmation write; `esp_ota_mark_app_valid_cancel_rollback()`
runs in `ota::tick` right after). A controller that applies an image and then
never polls status will see the device roll back after 60 s. AT-12 polls
status as part of its OTA sequence — that poll IS the confirmation.

## 409 ota_in_progress cases (both share the code)

1. An upload/apply arriving while another OTA op holds `g_busy` (the HTTP
   server is single-threaded, so this mainly guards apply-vs-apply and the
   apply-during-upload window from a second connection's perspective).
2. `apply` without `force:true` while any command is accepted/dispatched/
   confirming (`CommandEngine::count_non_terminal`).
`at05.py` must now find `ota_in_progress` PRESENT in the closed error table
(the Phase 6 flip).
