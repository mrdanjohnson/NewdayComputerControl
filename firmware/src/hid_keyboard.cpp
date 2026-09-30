#include "hid_keyboard.h"
#include <Arduino.h>
#include <esp_task_wdt.h>
#include <string.h>

// Classic ESP32 (ESP32/ESP32-D0WD, e.g. ESP-WROOM-32) has no native USB
// device controller — its USB port is a UART bridge, so TinyUSB HID cannot
// exist. This stub keeps the firmware buildable and honest on those chips:
// every dispatch fails as `failed`/`dispatch_error` at the engine layer,
// which is the correct Mode A verdict for "no HID link".
// Native USB device controllers exist only on the newer Espressif chips.
// Gate on the IDF target macros (always provided by the toolchain), not the
// ARDUINO_USB_* flags — PlatformIO defines those even for USB-less chips.
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2) || \
    defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
#define MC_HAS_USB_HID 1
#else
#define MC_HAS_USB_HID 0
#endif

#if MC_HAS_USB_HID
#include <USB.h>
#include <USBHID.h>
#include <USBHIDKeyboard.h>
#include <tusb.h>

// USB HID keyboard Power usage (spec-defined chord target; not named by the
// Arduino core).
static constexpr uint8_t kKeyPower = 0x66;

USBHIDKeyboard Keyboard;

// NOTE 2026-09-28: a second HID device (hand-written System Control
// descriptor for a HID Sleep report) lived here from 2026-09-27 15:40.
// It enumerated correctly on macOS (both collections parsed, keyboard
// driver matched — verified via ioreg), but from the v2-protocol flash
// onward NO keyboard report ever reached the host: typed macros and the
// Ctrl+Cmd+Q lock chord did nothing on two Macs, while the firmware
// believed every dispatch succeeded. The composite descriptor (report-ID'd
// keyboard + no-report-ID system control in one interface) is the only
// suspect. The System Control path is gone:
//  - Mode B sleep/restart/shutdown are agent-executed (protocol v2), HID
//    is not involved;
//  - Mode A HID sleep was already proven useless on modern macOS
//    2026-09-27 (report enumerates, macOS parses, silently ignores) and
//    the Cmd+Alt+Power chord only display-sleeps.
// Mode A sleep on S3 now has no HID mechanism and fails honestly
// (failed/dispatch_error) instead of silently breaking the keyboard.
#endif

void HidKeyboard::begin() {
#if MC_HAS_USB_HID
    Keyboard.begin();
    // Declare USB remote wakeup in the config descriptor (default core
    // attributes = self-powered, no wakeup): without it a sleeping host
    // keeps the port suspended and every HID report — including the wake
    // key-tap — is dropped on the suspended bus (proven live 2026-09-28:
    // AT-09 cleanup wake never woke the target). Must be set before start.
    USB.usbAttributes(TUSB_DESC_CONFIG_ATT_SELF_POWERED |
                      TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP);
    // TinyUSB does not start on its own: without this the device never
    // enumerates and every dispatch dies as dispatch_error.
    USB.begin();
#endif
}

#if MC_HAS_USB_HID
// A suspended host (Mac asleep) cannot receive reports: signal remote
// wakeup first and give the bus a bounded window to resume. No-op when
// the host is already awake. Requires the descriptor attribute set in
// begin(); harmless if the host never enabled remote wakeup (the resume
// signaling is a no-op then and the report attempt fails honestly).
// Pulses the wakeup up to 10x, 100 ms apart (macOS resume latency after
// a remote-wakeup signal is ~tens of ms but not bounded by the spec).
static bool hostResumeIfSuspendedDbg(bool& was_suspended, bool& wakeup_ok,
                                     unsigned& resume_ms) {
    was_suspended = tud_suspended();
    wakeup_ok = true;
    resume_ms = 0;
    if (!was_suspended) return true;
    const uint32_t t0 = millis();
    for (int i = 0; i < 10 && tud_suspended(); i++) {
        const bool ok = tud_remote_wakeup();
        if (i == 0) wakeup_ok = ok;
        for (int j = 0; j < 100 && tud_suspended(); j++) delay(1);
    }
    resume_ms = (unsigned)(millis() - t0);
    return !tud_suspended();
}

static void hostResumeIfSuspended() {
    bool ws, wo;
    unsigned ms;
    hostResumeIfSuspendedDbg(ws, wo, ms);
}
#endif

bool HidKeyboard::mounted() const {
#if MC_HAS_USB_HID
    return tud_mounted();
#else
    return false; // no USB device controller on this chip
#endif
}

HidKeyboard::WakeDebug HidKeyboard::s_wake_debug{};

bool HidKeyboard::wait_mounted() {
#if MC_HAS_USB_HID
    // Re-enumeration poll every 1 s, max 5 s (spec 15.1).
    for (int i = 0; i < 5; i++) {
        if (tud_mounted()) return true;
        delay(1000);
    }
    return tud_mounted();
#else
    return false;
#endif
}

bool HidKeyboard::reconnectForUserWake(uint32_t timeout_ms) {
#if MC_HAS_USB_HID
    // See the header: a sleeping host cuts port power, so the device is
    // truly detached — emulate the physical replug that provably
    // re-enumerates (pull-up toggle), then poll for the host to enumerate us.
    tud_disconnect();
    delay(200);
    tud_connect();
    const uint32_t start = millis();
    while (!tud_mounted() && millis() - start < timeout_ms) {
        esp_task_wdt_reset();
        delay(100);
    }
    return tud_mounted();
#else
    (void)timeout_ms;
    return false;
#endif
}

bool HidKeyboard::send_once(mcco::CommandType type) {
#if MC_HAS_USB_HID
    uint8_t mods[3] = {0, 0, 0};
    uint8_t n_mods = 0;
    uint8_t key = 0;
    switch (type) {
        case mcco::CommandType::Wake: {
            // Space tap. The original modifier-only tap (shift, press+release
            // in the same instant, no character) was never detected by this
            // host as a wake event — the user always fell back to a macro,
            // and space-containing macros woke it reliably (2026-09-30).
            // A printing key also gives the press a real duration (100 ms),
            // closer to a physical keypress than write()'s zero-gap tap.
            // Cost: one space lands in whatever app is focused after wake.
            Keyboard.releaseAll();
            Keyboard.press(' ');
            delay(100);
            Keyboard.release(' ');
            return true;
        }
        case mcco::CommandType::Lock:
            mods[0] = KEY_LEFT_CTRL;
            mods[1] = KEY_LEFT_GUI;
            n_mods = 2;
            key = 'q';
            break;
        case mcco::CommandType::Sleep:
            // No HID sleep mechanism: the System Control device that carried
            // it broke keyboard report delivery (see note above) and macOS
            // ignored the report anyway. Mode B sleep is agent-executed; in
            // Mode A this fails honestly at the dispatcher.
            return false;
        case mcco::CommandType::Restart:
            mods[0] = KEY_LEFT_CTRL;
            mods[1] = KEY_LEFT_GUI;
            n_mods = 2;
            key = kKeyPower;
            break;
        case mcco::CommandType::Shutdown:
            mods[0] = KEY_LEFT_CTRL;
            mods[1] = KEY_LEFT_ALT;
            mods[2] = KEY_LEFT_GUI;
            n_mods = 3;
            key = kKeyPower;
            break;
        default:
            return false; // no HID semantics for the remaining types in Phase 1
    }
    Keyboard.releaseAll();
    for (uint8_t i = 0; i < n_mods; i++) Keyboard.press(mods[i]);
    size_t sent = Keyboard.write(key);
    Keyboard.releaseAll();
    return sent == 1;
#else
    (void)type;
    return false;
#endif
}

bool HidKeyboard::send_chord(mcco::CommandType type) {
#if MC_HAS_USB_HID
    if (!wait_mounted()) {
        // Polling alone cannot recover a device the sleeping host powered
        // down (usb_detached ~8 s into sleep). An explicit WAKE dispatch
        // emulates the user's physical replug instead (proven to enumerate
        // on a sleeping host; the chord then wakes it like any keyboard).
        if (type == mcco::CommandType::Wake) {
            if (reconnectForUserWake(10000)) delay(1000); // let the host's HID driver attach before the tap
        }
        if (!wait_mounted()) return false;
    }
    if (type == mcco::CommandType::Wake) {
        // Wake gets the remote-wakeup diagnostics (read by the dispatcher
        // into the device log); other chords resume silently.
        s_wake_debug = WakeDebug{};
        s_wake_debug.was_suspended = tud_suspended();
        bool wo;
        hostResumeIfSuspendedDbg(s_wake_debug.was_suspended, wo,
                                 s_wake_debug.resume_ms);
        s_wake_debug.wakeup_ok = wo;
        s_wake_debug.sent_ok = send_once(type);
        if (!s_wake_debug.sent_ok) {
            delay(1000);
            if (wait_mounted()) s_wake_debug.sent_ok = send_once(type);
        }
        return s_wake_debug.sent_ok;
    }
    hostResumeIfSuspended();
    if (send_once(type)) return true;
    // Retry dispatch once (spec 15.1): allow a 1 s re-enumeration window.
    delay(1000);
    if (!wait_mounted()) return false;
    hostResumeIfSuspended();
    return send_once(type);
#else
    (void)type;
    return false;
#endif
}

uint8_t HidKeyboard::modifierUsage(const char* name) {
    if (!name) return 0;
    if (strcmp(name, "ctrl") == 0) return 0xE0;
    if (strcmp(name, "shift") == 0) return 0xE1;
    if (strcmp(name, "alt") == 0) return 0xE2;
    if (strcmp(name, "cmd") == 0) return 0xE3;
    return 0;
}

bool HidKeyboard::serviceHostReconnect(bool agent_session_active,
                                       bool host_declared_offline) {
#if MC_HAS_USB_HID
    // Host awake = live agent session (Wi-Fi heartbeat) and no open declared
    // expected-offline window. Only then is a missing mount a re-enumeration
    // failure; while the host sleeps the port is off by design and reconnect
    // churn would just flap the bus — and on this host a reconnect dark-wakes
    // the sleeping Mac, so the churn actively defeats sleep.
    static uint8_t unmounted_s = 0;
    if (!agent_session_active || host_declared_offline) {
        unmounted_s = 0;
        return false;
    }
    if (tud_mounted()) {
        unmounted_s = 0;
        return false;
    }
    if (++unmounted_s < 10) return false;
    unmounted_s = 0;
    tud_disconnect();
    delay(200);
    tud_connect();
    return true;
#else
    (void)agent_session_active;
    return false;
#endif
}

bool HidKeyboard::keyDown(uint8_t code) {
#if MC_HAS_USB_HID
    if (!tud_mounted()) return false;
    hostResumeIfSuspended();
    Keyboard.pressRaw(code);
    return true;
#else
    (void)code;
    return false;
#endif
}

bool HidKeyboard::keyUp(uint8_t code) {
#if MC_HAS_USB_HID
    if (!tud_mounted()) return false;
    Keyboard.releaseRaw(code);
    return true;
#else
    (void)code;
    return false;
#endif
}

bool HidKeyboard::allKeysUp() {
#if MC_HAS_USB_HID
    if (!tud_mounted()) return false;
    Keyboard.releaseAll();
    return true;
#else
    return false;
#endif
}

bool HidKeyboard::typeText(const char* s, size_t len, uint32_t inter_key_ms) {
#if MC_HAS_USB_HID
    if (!s || !tud_mounted()) return false;
    hostResumeIfSuspended();
    for (size_t i = 0; i < len; i++) {
        if (!tud_mounted()) return false; // USB dropped mid-text
        uint8_t c = (uint8_t)s[i];
        if (c < 0x20 && c != '\n' && c != '\t') continue; // non-printable: skip
        Keyboard.write(c); // press+release; ASCII path adds shift when needed
        if (inter_key_ms) delay(inter_key_ms);
    }
    return true;
#else
    (void)s;
    (void)len;
    (void)inter_key_ms;
    return false;
#endif
}
