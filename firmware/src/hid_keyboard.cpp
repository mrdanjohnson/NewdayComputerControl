#include "hid_keyboard.h"
#include <Arduino.h>
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
#include <USBHIDKeyboard.h>
#include <tusb.h>

// USB HID keyboard Power usage (spec-defined chord target; not named by the
// Arduino core).
static constexpr uint8_t kKeyPower = 0x66;

USBHIDKeyboard Keyboard;
#endif

void HidKeyboard::begin() {
#if MC_HAS_USB_HID
    Keyboard.begin();
#endif
}

bool HidKeyboard::mounted() const {
#if MC_HAS_USB_HID
    return tud_mounted();
#else
    return false; // no USB device controller on this chip
#endif
}

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

bool HidKeyboard::send_once(mcco::CommandType type) {
#if MC_HAS_USB_HID
    uint8_t mods[3] = {0, 0, 0};
    uint8_t n_mods = 0;
    uint8_t key = 0;
    switch (type) {
        case mcco::CommandType::Wake:
            // Modifier tap: wakes a sleeping Mac without injecting a character.
            key = KEY_LEFT_SHIFT;
            break;
        case mcco::CommandType::Lock:
            mods[0] = KEY_LEFT_CTRL;
            mods[1] = KEY_LEFT_GUI;
            n_mods = 2;
            key = 'q';
            break;
        case mcco::CommandType::Sleep:
            mods[0] = KEY_LEFT_GUI;
            mods[1] = KEY_LEFT_ALT;
            n_mods = 2;
            key = kKeyPower;
            break;
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
    if (!wait_mounted()) return false;
    if (send_once(type)) return true;
    // Retry dispatch once (spec 15.1): allow a 1 s re-enumeration window.
    delay(1000);
    if (!wait_mounted()) return false;
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

bool HidKeyboard::keyDown(uint8_t code) {
#if MC_HAS_USB_HID
    if (!tud_mounted()) return false;
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
