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
    // keyboard Power key (usage 0x66) with the noted modifiers. sleep is a HID
    // System Control report (Generic Desktop page 0x01, Sleep usage 0x82) sent
    // on a second device alongside the keyboard — the Cmd+Alt+Power chord it
    // replaced only slept the displays, not the system.
    //
    // Returns false if USB is not mounted: polls re-enumeration every 1 s for
    // up to 5 s (spec 15.1), retries the dispatch once, and gives up on the
    // second failure (the engine then maps the command to failed/dispatch_error).
    bool send_chord(mcco::CommandType type);

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

private:
    static bool wait_mounted();
    static bool send_once(mcco::CommandType type);
};
