# DEBUG: task-watchdog panic during serial provisioning (`key create`)

**Date:** 2026-10-07 (NDC client install, ProTools Mac) — firmware 1.6.0-phase6
(dist build 2026-10-03, git 49a2e07)
**Status:** provisioner hardened (detects the reboot and retries — agent-side);
**firmware root cause NOT yet identified — needs a live repro.**

## What happened

First `key create` on a factory-fresh device, seconds after `wifi set` +
association succeeded (`connected ip=10.10.40.146`), crashed with a
task-watchdog panic and rebooted. The serial buffer replayed the command
after boot, so the key WAS created post-reboot ("created key-01") — the
crash is timing-sensitive, not deterministic.

Console capture (tail):

```
 on core 0

Backtrace: 0x40377a66:0x3fc95b90 0x4037c905:0x3fc95bb0 0x40382bd5:0x3fc95bd0
0x42046f24:0x3fc95c50 0x40379119:0x3fc95c70 0x420eb7a7:0x3fcf5da0
0x42047795:0x3fcf5dc0 0x4037ddc0:0x3fcf5de0
...
Rebooting...
reset reason: task_wdt
```

Decoded against `firmware.elf` (addr2line, toolchain-xtensa-esp32s3):

```
0x40377a66 panic_abort          panic.c:408
0x4037c905 esp_system_abort     esp_system.c:137
0x40382bd5 abort                abort.c:46
0x42046f24 task_wdt_isr         task_wdt.c:176   <- TWDT ISR (panic)
0x40379119 _xt_lowint1          xtensa_vectors.S:1118
0x420eb7a7 esp_pm_impl_waiti    pm_impl.c:853    (via cpu_ll_waiti)
0x42047795 esp_vApplicationIdleHook  freertos_hooks.c:63
0x4037ddc0 prvIdleTask          tasks.c:4099
```

The frames below `task_wdt_isr` are the core-0 **Idle** task the ISR
preempted — they do NOT identify the starved task. The TWDT message line
naming the starved task was truncated in the paste (" on core 0"); **the
task name is the key missing fact** (see "What to capture").

Boot log on the recovery boot is otherwise NORMAL for factory state:
`kstore.*` / `mstore.*` / `tstore.*` "does not exist, no permits for
creation" = files absent on an empty LittleFS (expected); the various
`nvs_get_str ... NOT_FOUND` = factory NVS. "init complete in 95 ms".

## Facts

- TWDT: 10 s timeout, panic on missed feed (`main.cpp:305-306`);
  loopTask subscribes there, server/dispatcher/wifi/ws/agent_link subscribe
  at task creation.
- RNG on the key-create path is `esp_fill_random` (instant — not the hog).
- `persistKeys` → `writeSlotFile` (`nvs_config.cpp:48-78`): one small file
  write + byte-by-byte readback + one NVS `putUChar`. Milliseconds even with
  flash erase — NOT a 10 s block on its own.
- The same key-create code succeeded on 2026-10-03 (key-09/10/11,
  label "bench") — so it is not simply "first LittleFS write crashes".
- Crash timing correlates with the post-association burst: SNTP sync
  (`beginSntp` at boot fires on connect), mDNS announce/TXT, DHCP settling,
  agent_link WS connect attempt, dispatcher tick work.

## Hypotheses (unconfirmed)

1. **Mutex convoy**: CLI (loopTask) holds `engine_mutex` through
   `KeyStore::add` + `persistKeys`; a subscribed task on the other core
   blocks on `engine_mutex`/LittleFS and misses its feed. Candidates:
   dispatcher, agent_link (its own comment at `agent_link.cpp:767`
   documents exactly this hazard class), http task.
2. **A subscribed task with a feedless long block** triggered by the
   connect-time event burst (e.g. agent_link reconnect loop, WS upgrade)
   — i.e. the crash is coincidental with `key create`, not caused by it.

## What to capture on the next repro (in priority order)

1. The FULL TWDT line — the starved task name(s):
   "Task watchdog got triggered. The following tasks did not reset the
   watchdog in time: **- `<task name>` (CPU n)**". Terminal width ate it;
   widen the window or pipe to a file.
2. `GET /api/v1/logs?category=system` right after (the RAM ring holds the
   boot event sequence).
3. Whether it reproduces when WiFi is ALREADY settled (wait for
   `connected ip=` via `wifi status` before any `key create`), and on a
   quiet network (no paired agent).
4. Whether it reproduces with `admin set` or a second `key create` when
   the device is idle — isolates "key create" vs "connect-time burst".

## Agent-side mitigation shipped (2026-10-07)

`agent/mc_provision.py` now detects the boot banner appearing mid-command
(`DeviceRebooted`) and retries the whole serial phase (max 3 attempts; the
crashed command may be replayed by the device, so a duplicate key slot is
possible — harmless within the 8-key limit). Provisioning self-heals; this
doc stays open until the firmware side is root-caused.
