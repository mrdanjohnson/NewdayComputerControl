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
    // injecting a character. lock = Ctrl+Cmd+Q. sleep/restart/shutdown use the
    // USB keyboard Power key (usage 0x66) with the noted modifiers.
    //
    // Returns false if USB is not mounted: polls re-enumeration every 1 s for
    // up to 5 s (spec 15.1), retries the dispatch once, and gives up on the
    // second failure (the engine then maps the command to failed/dispatch_error).
    bool send_chord(mcco::CommandType type);

private:
    static bool wait_mounted();
    static bool send_once(mcco::CommandType type);
};
