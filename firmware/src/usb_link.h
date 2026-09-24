#pragma once
#include <cstdint>
#include "mc_clock.h"
#include "mc_log.h"

// USB device-link tracker (src/-only glue). Reports the state of the ESP32's
// own USB device link to the host Mac, independent of the MCA agent channel:
// the endpoint survives Mac outages and can distinguish a host restart (USB
// down + new boot_id) from an agent/network fault (USB up + silence). Raw
// TinyUSB lifecycle events only — this layer never classifies sleep vs
// power-off (host power-off may surface as bus suspend, not unmount; VBUS
// sense on the devkitc is unverified).
//
// All storage is static; callbacks (TinyUSB/Arduino USB event task context)
// only write volatile words. Classic ESP32 (no USB device controller) compiles
// to a permanent no_usb state.

constexpr uint8_t kUsbLinkDetached = 0;   // host gone (unmount / bus reset)
constexpr uint8_t kUsbLinkMounted = 1;    // enumerated and active
constexpr uint8_t kUsbLinkSuspended = 2;  // host suspended the bus
constexpr uint8_t kUsbLinkNoUsb = 3;      // no USB device controller (classic)

struct UsbLinkState {
    uint8_t state = kUsbLinkNoUsb;
    uint64_t changed_at = 0;       // IClock::millis() of last transition; 0 = never attached
    uint64_t changed_epoch = 0;    // IClock::epoch_seconds() at last transition (for display)
    uint32_t attach_count = 0;
    uint32_t detach_count = 0;
    uint32_t suspend_count = 0;
    uint32_t resume_count = 0;
};

// One-time init on USB-capable targets: registers the USB.onEvent handler
// (must precede or accompany USB.begin() so the initial enumeration is
// captured). No-op on classic ESP32.
void usb_link_begin(mcco::IClock* clock);

// Volatile snapshot; callable from any task. changed_at is monotonic millis.
UsbLinkState usb_link_state();

// Drain pending link events into the log ring. Call from a 1 s housekeeping
// context with log/clock access (AgentLink::tick). The boot-time initial
// enumeration is logged once as informational "usb_link", never as
// usb_attached; afterwards each transition logs one entry:
//   usb_attached / usb_suspended / usb_resumed / usb_detached
// with detail {"state":"<new state>"}. No-op on classic ESP32.
void usb_link_service(mcco::ILog& log, mcco::IClock& clock);

const char* usb_link_state_string(uint8_t state);
