#include "usb_link.h"
#include <cstdio>

// Same gating as hid_keyboard.cpp: the classic ESP32 has no native USB device
// controller, so this compiles to a permanent no_usb stub there.
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || \
    defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
#define MC_HAS_USB_HID 1
#else
#define MC_HAS_USB_HID 0
#endif

#if MC_HAS_USB_HID
#include <Arduino.h>
#include <USB.h>
#include <esp_event.h>
#include <tusb.h>

// Event mechanism: the Arduino core strongly defines tud_mount_cb/
// tud_umount_cb/tud_suspend_cb/tud_resume_cb (cores/esp32/USB.cpp) and re-posts
// them as ARDUINO_USB_* events on its own event loop, so USB.onEvent is the
// only hook available — defining the weak tud_* callbacks ourselves would
// collide at link time. The handler runs on the Arduino USB event task
// (TinyUSB context): volatile state words only, never log or allocate here.
namespace {
volatile uint8_t g_state = kUsbLinkDetached;
volatile uint64_t g_changed_at = 0;
volatile uint64_t g_changed_epoch = 0;
volatile uint32_t g_attach_count = 0;
volatile uint32_t g_detach_count = 0;
volatile uint32_t g_suspend_count = 0;
volatile uint32_t g_resume_count = 0;
volatile uint8_t g_pending = 0;
mcco::IClock* g_clock = nullptr;

constexpr uint8_t kPendingAttach = 0x01;
constexpr uint8_t kPendingDetach = 0x02;
constexpr uint8_t kPendingSuspend = 0x04;
constexpr uint8_t kPendingResume = 0x08;

void on_usb_event(void*, esp_event_base_t, int32_t id, void*) {
    uint8_t next;
    uint8_t bit;
    switch (id) {
        case ARDUINO_USB_STARTED_EVENT:
            next = kUsbLinkMounted;
            bit = kPendingAttach;
            g_attach_count++;
            break;
        case ARDUINO_USB_SUSPEND_EVENT:
            next = kUsbLinkSuspended;
            bit = kPendingSuspend;
            g_suspend_count++;
            break;
        case ARDUINO_USB_RESUME_EVENT:
            next = kUsbLinkMounted;
            bit = kPendingResume;
            g_resume_count++;
            break;
        case ARDUINO_USB_STOPPED_EVENT:
            next = kUsbLinkDetached;
            bit = kPendingDetach;
            g_detach_count++;
            break;
        default:
            return;
    }
    // Raw events reported honestly: host power-off may arrive as suspend
    // rather than unmount (VBUS sense unverified) — no sleep/off claim here.
    g_state = next;
    g_pending |= bit;
    g_changed_at = g_clock ? g_clock->millis() : 0;
    g_changed_epoch = g_clock ? g_clock->epoch_seconds() : 0;
}
} // namespace

void usb_link_begin(mcco::IClock* clock) {
    g_clock = clock;
    USB.onEvent(on_usb_event);
}

UsbLinkState usb_link_state() {
    UsbLinkState s;
    s.state = g_state;
    s.changed_at = g_changed_at;
    s.changed_epoch = g_changed_epoch;
    s.attach_count = g_attach_count;
    s.detach_count = g_detach_count;
    s.suspend_count = g_suspend_count;
    s.resume_count = g_resume_count;
    return s;
}

void usb_link_service(mcco::ILog& log, mcco::IClock& clock) {
    static bool s_settled = false;
    // Host-liveness backstop: the TinyUSB device task on the S3 devkitc is
    // blind to host-side port loss — neither the core's events nor the
    // tud_*() flags move when the host unplugs/re-enumerates, and GOTGCTL.
    // BSESVLD is useless too (the devkitc straps OTG VBUS to the board
    // rail, so it reads valid whenever the board has power — verified on
    // the bench). What the PHY does report is DSTS.SUSPSTS: the DWC core
    // sets it ~3 ms after host signaling (SOF packets) stops. A host that
    // sleeps, powers off, or unplugs all stop signaling, so this is the
    // reliable "host link down" signal — it cannot distinguish sleep from
    // power-off from unplug (classification stays with the declared
    // goodbye + boot_id, per the docs). ESP32-S3 TRM: USB OTG base
    // 0x60080000, DSTS at +0x808, SUSPSTS = bit 0.
    constexpr uint32_t kDwotgBase = 0x60080000UL;
    constexpr uint32_t kDstsSuspsts = (1UL << 0);
    const bool host_signaling =
        (*reinterpret_cast<volatile uint32_t*>(kDwotgBase + 0x808) & kDstsSuspsts) == 0;
    const uint8_t tracked = g_state;
    uint8_t actual;
    if (tud_mounted() && !host_signaling) {
        actual = kUsbLinkSuspended;  // host stopped signaling: sleep/off/unplug
    } else if (tud_mounted() || host_signaling) {
        actual = kUsbLinkMounted;
    } else {
        actual = kUsbLinkDetached;  // never enumerated (boot window)
    }
    // Debounce: re-enumeration flickers (tud mount and SUSPSTS clear one
    // tick apart) would log attached/detached pairs a second apart. Require
    // the observed state to hold for 2 consecutive 1 s ticks before
    // adopting it; events then follow within ~2 s of the physical change.
    static uint8_t s_candidate = 0xFF;  // 0xFF = none
    static uint8_t s_candidate_ticks = 0;
    if (actual == g_state) {
        s_candidate = 0xFF;
        s_candidate_ticks = 0;
    } else if (actual == s_candidate) {
        s_candidate_ticks++;
    } else {
        s_candidate = actual;
        s_candidate_ticks = 1;
    }
    if (s_candidate != 0xFF && s_candidate_ticks >= 2 && s_candidate != g_state) {
        const uint8_t prev = g_state;
        g_state = s_candidate;
        g_changed_at = clock.millis();
        g_changed_epoch = clock.epoch_seconds();
        if (s_candidate == kUsbLinkMounted) {
            g_pending |= (prev == kUsbLinkSuspended) ? kPendingResume : kPendingAttach;
            if (prev == kUsbLinkSuspended) g_resume_count++; else g_attach_count++;
        } else if (s_candidate == kUsbLinkSuspended) {
            g_pending |= kPendingSuspend;
            g_suspend_count++;
        } else {
            g_pending |= kPendingDetach;
            g_detach_count++;
        }
    }
    const uint8_t pending = g_pending;
    g_pending = 0;
    if (!s_settled) {
        // Boot-time enumeration is the baseline, not a transition: log one
        // informational entry with the settled state and swallow the rest.
        s_settled = true;
        if (g_state != kUsbLinkDetached) {
            char detail[32];
            snprintf(detail, sizeof(detail), "{\"state\":\"%s\"}",
                     usb_link_state_string(g_state));
            log.write(mcco::LogCategory::System, mcco::LogLevel::Info, "usb_link", nullptr,
                      nullptr, nullptr, detail);
        }
        return;
    }
    struct Ev {
        uint8_t bit;
        const char* event;
        const char* state;
        mcco::LogLevel level;
    };
    static const Ev kEvents[] = {
        {kPendingAttach, "usb_attached", "mounted", mcco::LogLevel::Info},
        {kPendingSuspend, "usb_suspended", "suspended", mcco::LogLevel::Info},
        {kPendingResume, "usb_resumed", "mounted", mcco::LogLevel::Info},
        {kPendingDetach, "usb_detached", "detached", mcco::LogLevel::Warn},
    };
    for (const Ev& e : kEvents) {
        if (!(pending & e.bit)) continue;
        char detail[32];
        snprintf(detail, sizeof(detail), "{\"state\":\"%s\"}", e.state);
        log.write(mcco::LogCategory::System, e.level, e.event, nullptr, nullptr, nullptr,
                  detail);
    }
}

#else // !MC_HAS_USB_HID — classic ESP32: honest no_usb stub

void usb_link_begin(mcco::IClock*) {}

UsbLinkState usb_link_state() { return UsbLinkState{}; }

void usb_link_service(mcco::ILog&, mcco::IClock&) {}

#endif

const char* usb_link_state_string(uint8_t state) {
    switch (state) {
        case kUsbLinkMounted: return "mounted";
        case kUsbLinkSuspended: return "suspended";
        case kUsbLinkDetached: return "detached";
        default: return "no_usb";
    }
}
