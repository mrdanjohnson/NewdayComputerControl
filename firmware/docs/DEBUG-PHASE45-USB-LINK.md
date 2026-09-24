# MacControl — DEBUG: Phase 4.5 USB host-link detection (invisible disconnects)

2026-09-24. Adding Mac power-off detection via the ESP32-S3's own USB device
link turned into a multi-round bench investigation. Outcome: detection works
(`usb_link.*`, a 1 Hz PHY backstop); the forensics below are recorded so the
dead ends are never re-walked.

## Goal

The endpoint is a USB HID device *of the Mac*. With independent power, a Mac
sleep/shutdown/power-off stops host USB signaling while the ESP32 keeps
running — a detection channel independent of the agent's WS/heartbeat.

## What failed (and why)

**1. TinyUSB events and flags are blind to host-side port loss.**
The Arduino core re-posts `tud_mount/umount/suspend/resume` as
`ARDUINO_USB_*` events (`cores/esp32/USB.cpp:98-124`; the `tud_*` weak
symbols are strongly defined by the core, so `USB.onEvent` is the only
hook). On the S3-DevKitC-1 the device task never fires umount/suspend for a
mid-session host disconnect/re-enumeration, and `tud_mounted()` stays stuck
true — verified: host re-enumerated at a new USB address (Location ID
suffix 90 → 101 → 117) while the firmware saw zero events across several
confirmed replugs. `tud_mounted()` only reads correctly when the device
boots with no host (never-mounted case).

**2. GOTGCTL.BSESVLD is useless on this board.** The devkitc straps the
OTG port's VBUS sense to the board rail, so the bit reads "valid" whenever
the board has power. It cannot see the host at all.

**3. (Bench red herring) The board is powered by whichever USB port is
connected.** Both ports feed the 5V rail (diode-OR). Pulling the cable that
currently feeds power = instant board reboot (`reset: poweron`, fresh log
ring, no events possible) — indistinguishable from "detection failed"
without reading the boot entry. Always check `boot.reset` and the heap-line
continuity before believing an "invisible event". Cable labels: **"USB" =
native OTG (HID keyboard + the monitored link)**; **"com" = CH343 UART
(console/flashing)**. Pulling the wrong one cost three investigation
rounds.

**4. The first backstop read the same blind flags.** Reconciling against
`tud_mounted()`/`tud_suspended()` cannot fix a stack whose flags don't move.

## What works: DSTS.SUSPSTS

Serial forensics (temporary 1 Hz `USBDBG` dump of raw registers; since
removed) established the two signatures:

- **Host present**: `mnt=1`, DSTS low bits `...6` (SUSPSTS=0), SOF frame
  number in DSTS bits [16:3] ticking at ~1000/s.
- **Host absent**: `mnt=0`, DSTS low bits `...7` (SUSPSTS=1, set by the
  DWC core ~3 ms after host signaling stops), frame number frozen.

ESP32-S3 TRM: USB OTG registers base `0x60080000`, DSTS at `+0x808`,
SUSPSTS = bit 0. A host that sleeps, powers off, or unplugs all stop
sending SOFs, so this single bit is the reliable "host link down" signal.
It cannot distinguish sleep from power-off from unplug — that stays with
the agent's declared goodbye + boot_id (documented classification).

## The shipped design (`src/usb_link.*`)

- `USB.onEvent` handler sets volatile state + pending bits only (TinyUSB
  task context: never log/allocate there).
- 1 Hz `usb_link_service()` (drained from `AgentLink::tick`, agent-scoped
  or not): reads DSTS.SUSPSTS + `tud_mounted()`, derives the observed
  state, **debounces 2 consecutive ticks** (re-enumeration flickers the
  signals one tick apart — un-debounced it logged
  attached/detached/attached within 2 s), then adopts + logs:
  `usb_attached` / `usb_suspended` / `usb_resumed` / `usb_detached`
  (category `system`, detail `{"state":...}`).
- Report: `/api/v1/agent/status` gains `"usb": {"link": "up|down",
  "state": "mounted|suspended|detached|no_usb", "changed_at": <iso|null>}`.
  `link: "up"` **only** when mounted; suspended = host stopped signaling
  (link down even if still enumerated). `changed_at` is captured as
  wall-clock at transition (stable); pre-SNTP values (< 2026-01-01 floor)
  re-derive from monotonic age at render (mixing integer `epoch_seconds`
  with a floored `age_ms/1000` oscillates ±1 s around sub-second phases).
- Classic ESP32: honest `no_usb` stub.

## Verified on the bench (2026-09-24)

- Labeled-USB cable unplug ~10 s: `16:54:13 usb_detached` →
  `16:55:04 usb_attached`; report `mounted/up → detached/down →
  mounted/up`; single debounced events; board powered via the CH343 cable
  throughout. (This is also the power-off simulation: same PHY signature.)
- Real Mac sleep (~40 s): agent's IOKit declaration landed 24 s before
  link loss (`16:56:19` goodbye vs `16:56:43 usb_detached`); wake at
  `16:56:56 usb_attached`; same `boot_id`; expected-offline classification
  (not fault).
- Board reboots (flash/serial-open) correctly log link down/up as the host
  sees the device vanish/reappear.

## Known residuals

- `usb_suspended` vs `usb_detached` distinction is partially theoretical:
  with the host gone mid-session the state usually reads detached; during
  sleep it may read suspended (`tud_mounted` stuck + SUSPSTS set). Both
  mean "link down" — classification is documented, not auto-labeled.
- Agent-side wake deltas can double-report on Power Nap dark wakes
  (documented in `docs/PHASE4.5.md`; Phase 5 polish).
