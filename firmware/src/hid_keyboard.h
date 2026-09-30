#pragma once
#include "mc_types.h"

// Arduino TinyUSB keyboard-only HID (USB CDC on boot is disabled by the build
// flag, so the device enumerates as a keyboard only).
class HidKeyboard {
public:
    void begin();
    // True while the HID device is mounted by a host (TinyUSB enumeration
    // complete). Cheap volatile read; safe from any task.
    bool mounted() const;
    // Sends the chord for a power/lock command type.
    //
    // IMPLEMENTATION DECISION (the spec fixes no chords): the wake chord is a
    // left-shift *tap* — a modifier press+release wakes a sleeping Mac without
    // injecting a character. lock = Ctrl+Cmd+Q. restart/shutdown use the USB
    // keyboard Power key (usage 0x66) with the noted modifiers. sleep has no
    // HID mechanism in Mode A (see hid_keyboard.cpp — Mode B sleep is
    // agent-executed); the wake chord signals USB remote wakeup first when
    // the host has suspended the port.
    //
    // Returns false if USB is not mounted: polls re-enumeration every 1 s for
    // up to 5 s (spec 15.1), retries the dispatch once, and gives up on the
    // second failure (the engine then maps the command to failed/dispatch_error).
    bool send_chord(mcco::CommandType type);

    // Last Wake-attempt diagnostics, populated by send_chord(Wake) and read
    // by the dispatcher into the device log — remote-wakeup failures are
    // otherwise invisible from the network.
    struct WakeDebug {
        bool was_suspended;  // TinyUSB saw the host suspend the port
        bool wakeup_ok;      // tud_remote_wakeup() accepted (host enabled it)
        unsigned resume_ms;  // time until the bus left the suspended state
        bool sent_ok;        // the chord report reached the wire
    };
    static const WakeDebug& wakeDebug() { return s_wake_debug; }

    // ---- Macro interpreter primitives (spec 10.1.1/10.3.1) ------------------
    // Named USB HID usages come from the core table (mcco::hid_key_code).
    // All return false immediately when USB is not mounted; no retry polling
    // (the macro runner handles usb_disconnected aborts itself).
    bool keyDown(uint8_t code);  // press one usage (key or modifier 0xE0..0xE7)
    bool keyUp(uint8_t code);
    bool allKeysUp();            // release every held key + modifier (spec 10.3.1)
    // Emits each byte as press+release at inter_key_ms; ASCII subset only
    // (letters/digits/space/enter and common punctuation), other bytes skipped.
    bool typeText(const char* s, size_t len, uint32_t inter_key_ms);
    // Maps a core modifier name {ctrl,shift,alt,cmd} to its left-side HID
    // modifier usage (0xE0..0xE3); 0 if unknown.
    static uint8_t modifierUsage(const char* name);

    // USB re-enumeration supervisor, call ~1/s with whether the paired
    // agent session is live (i.e. the host is awake). Hosts that power-cycle
    // the USB port in sleep sometimes leave the device un-enumerated after
    // wake (the S3 PHY misses the re-attach; macros then report
    // usb_disconnected on an awake host — observed 2026-09-28). When the
    // host is demonstrably awake but tud_mounted() has been false for 10 s,
    // force a disconnect/connect so the host re-enumerates us. Returns true
    // when a re-attach was forced (caller logs it). `host_declared_offline`
    // (an open agent_goodbye sleep/restart/shutdown window) suppresses the
    // supervisor: re-enumerating a sleeping host dark-wakes it and the
    // reconnect churn prevents sustained sleep (observed 2026-09-29: the
    // session stays live across darkwake heartbeats, so the session gate
    // alone never closes).
    bool serviceHostReconnect(bool agent_session_active, bool host_declared_offline);

private:
    static bool wait_mounted();
    static bool send_once(mcco::CommandType type);
    static WakeDebug s_wake_debug;
};
