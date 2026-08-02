#include "hid_keyboard.h"

#include <algorithm>
#include <iterator>

namespace pm::input {

namespace {

// Modifier bits of byte 0, straight from the USB HID spec.
constexpr uint8_t MOD_LEFT_CTRL   = 1 << 0;
constexpr uint8_t MOD_LEFT_SHIFT  = 1 << 1;
constexpr uint8_t MOD_LEFT_ALT    = 1 << 2;
constexpr uint8_t MOD_LEFT_GUI    = 1 << 3;
constexpr uint8_t MOD_RIGHT_CTRL  = 1 << 4;
constexpr uint8_t MOD_RIGHT_SHIFT = 1 << 5;
constexpr uint8_t MOD_RIGHT_ALT   = 1 << 6;
constexpr uint8_t MOD_RIGHT_GUI   = 1 << 7;

// More keys held than the report has slots: the spec says to fill every key slot
// with the rollover code rather than picking six arbitrarily.
constexpr uint8_t ERROR_ROLL_OVER = 0x01;
constexpr size_t MAX_KEYS = 6;

// The same report descriptor scrcpy uses. Do NOT get creative here — the device
// builds its keyboard from exactly these bytes, and anything else is a device Android
// has never seen.
const uint8_t KEYBOARD_REPORT_DESC[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x06,       // Usage (Keyboard)
    0xA1, 0x01,       // Collection (Application)

    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0xE0,       //   Usage Minimum (224 = left Ctrl)
    0x29, 0xE7,       //   Usage Maximum (231 = right GUI)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1 bit)
    0x95, 0x08,       //   Report Count (8)
    0x81, 0x02,       //   Input (Data, Variable, Absolute) — byte 0, the modifiers

    0x75, 0x08,       //   Report Size (8 bits)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x01,       //   Input (Constant) — byte 1, reserved

    0x05, 0x08,       //   Usage Page (LEDs)
    0x19, 0x01,       //   Usage Minimum (1 = NumLock)
    0x29, 0x05,       //   Usage Maximum (5 = Kana)
    0x75, 0x01,       //   Report Size (1 bit)
    0x95, 0x05,       //   Report Count (5)
    0x91, 0x02,       //   Output (Data, Variable, Absolute) — LEDs, reported back by the device
    0x75, 0x03,       //   Report Size (3 bits)
    0x95, 0x01,       //   Report Count (1)
    0x91, 0x01,       //   Output (Constant) — padding to a full byte

    0x05, 0x07,       //   Usage Page (Key Codes)
    0x19, 0x00,       //   Usage Minimum (0)
    0x29, 0x65,       //   Usage Maximum (101 = Application)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x65,       //   Logical Maximum (101)
    0x75, 0x08,       //   Report Size (8 bits)
    0x95, 0x06,       //   Report Count (6)
    0x81, 0x00,       //   Input (Data, Array) — bytes 2..7, the pressed keys

    0xC0              // End Collection
};

// PC set-1 scancode -> HID usage, for keys that arrive WITHOUT the extended flag.
// 0 means either "no such key on a boot keyboard" or "this is a modifier" — modifiers
// go through scancode_to_modifier instead.
const uint8_t SCANCODE_TO_USAGE[] = {
    0x00,       // 0x00 unused
    0x29,       // 0x01 Esc
    0x1E,       // 0x02 1
    0x1F,       // 0x03 2
    0x20,       // 0x04 3
    0x21,       // 0x05 4
    0x22,       // 0x06 5
    0x23,       // 0x07 6
    0x24,       // 0x08 7
    0x25,       // 0x09 8
    0x26,       // 0x0A 9
    0x27,       // 0x0B 0
    0x2D,       // 0x0C key right of 0 (ß on German layouts)
    0x2E,       // 0x0D key right of that (´)
    0x2A,       // 0x0E Backspace
    0x2B,       // 0x0F Tab
    0x14,       // 0x10 Q
    0x1A,       // 0x11 W
    0x08,       // 0x12 E
    0x15,       // 0x13 R
    0x17,       // 0x14 T
    0x1C,       // 0x15 Y (Z on German layouts — the device's layout decides, not us)
    0x18,       // 0x16 U
    0x0C,       // 0x17 I
    0x12,       // 0x18 O
    0x13,       // 0x19 P
    0x2F,       // 0x1A [ (Ü)
    0x30,       // 0x1B ] (+)
    0x28,       // 0x1C Enter
    0x00,       // 0x1D left Ctrl (modifier)
    0x04,       // 0x1E A
    0x16,       // 0x1F S
    0x07,       // 0x20 D
    0x09,       // 0x21 F
    0x0A,       // 0x22 G
    0x0B,       // 0x23 H
    0x0D,       // 0x24 J
    0x0E,       // 0x25 K
    0x0F,       // 0x26 L
    0x33,       // 0x27 ; (Ö)
    0x34,       // 0x28 ' (Ä)
    0x35,       // 0x29 ` (^)
    0x00,       // 0x2A left Shift (modifier)
    0x31,       // 0x2B backslash (#)
    0x1D,       // 0x2C Z (Y on German layouts)
    0x1B,       // 0x2D X
    0x06,       // 0x2E C
    0x19,       // 0x2F V
    0x05,       // 0x30 B
    0x11,       // 0x31 N
    0x10,       // 0x32 M
    0x36,       // 0x33 ,
    0x37,       // 0x34 .
    0x38,       // 0x35 / (-)
    0x00,       // 0x36 right Shift (modifier)
    0x55,       // 0x37 Keypad *
    0x00,       // 0x38 left Alt (modifier)
    0x2C,       // 0x39 Space
    0x39,       // 0x3A CapsLock
    0x3A,       // 0x3B F1
    0x3B,       // 0x3C F2
    0x3C,       // 0x3D F3
    0x3D,       // 0x3E F4
    0x3E,       // 0x3F F5
    0x3F,       // 0x40 F6
    0x40,       // 0x41 F7
    0x41,       // 0x42 F8
    0x42,       // 0x43 F9
    0x43,       // 0x44 F10
    0x53,       // 0x45 NumLock
    0x47,       // 0x46 ScrollLock
    0x5F,       // 0x47 Keypad 7
    0x60,       // 0x48 Keypad 8
    0x61,       // 0x49 Keypad 9
    0x56,       // 0x4A Keypad -
    0x5C,       // 0x4B Keypad 4
    0x5D,       // 0x4C Keypad 5
    0x5E,       // 0x4D Keypad 6
    0x57,       // 0x4E Keypad +
    0x59,       // 0x4F Keypad 1
    0x5A,       // 0x50 Keypad 2
    0x5B,       // 0x51 Keypad 3
    0x62,       // 0x52 Keypad 0
    0x63,       // 0x53 Keypad .
    0x00,       // 0x54 SysRq
    0x00,       // 0x55 unused
    0x64,       // 0x56 the extra key next to left Shift (< > | on German layouts)
    0x44,       // 0x57 F11
    0x45        // 0x58 F12
};

uint8_t scancode_to_modifier(uint32_t scancode, bool extended) {
    if (extended) {
        switch (scancode) {
        case 0x1D: return MOD_RIGHT_CTRL;
        case 0x38: return MOD_RIGHT_ALT;   // AltGr
        case 0x5B: return MOD_LEFT_GUI;
        case 0x5C: return MOD_RIGHT_GUI;
        default:   return 0;
        }
    }
    switch (scancode) {
    case 0x1D: return MOD_LEFT_CTRL;
    case 0x2A: return MOD_LEFT_SHIFT;
    case 0x36: return MOD_RIGHT_SHIFT;
    case 0x38: return MOD_LEFT_ALT;
    default:   return 0;
    }
}

uint8_t scancode_to_usage(uint32_t scancode, bool extended) {
    if (extended) {
        // Same scancode numbers as above, but prefixed with E0 by the keyboard. These
        // are the keys the original AT keyboard did not have.
        switch (scancode) {
        case 0x1C: return 0x58; // Keypad Enter
        case 0x35: return 0x54; // Keypad /
        case 0x37: return 0x46; // PrintScreen
        case 0x46: return 0x48; // Pause / Break
        case 0x47: return 0x4A; // Pos1
        case 0x48: return 0x52; // Arrow up
        case 0x49: return 0x4B; // Page up
        case 0x4B: return 0x50; // Arrow left
        case 0x4D: return 0x4F; // Arrow right
        case 0x4F: return 0x4D; // End
        case 0x50: return 0x51; // Arrow down
        case 0x51: return 0x4E; // Page down
        case 0x52: return 0x49; // Insert
        case 0x53: return 0x4C; // Delete
        case 0x5D: return 0x65; // Context menu key
        default:   return 0;
        }
    }

    if (scancode < sizeof(SCANCODE_TO_USAGE)) {
        return SCANCODE_TO_USAGE[scancode];
    }
    return 0;
}

} // namespace

const uint8_t* HidKeyboard::report_descriptor(size_t* out_size) {
    if (out_size) {
        *out_size = sizeof(KEYBOARD_REPORT_DESC);
    }
    return KEYBOARD_REPORT_DESC;
}

void HidKeyboard::build_report(uint8_t* report) const {
    uint8_t modifiers = m_modifiers;

    // Windows synthesises a left-Ctrl press every time AltGr goes down, so AltGr+Q
    // would reach the device as Ctrl+AltGr+Q and the "@" would never appear. Mask that
    // phantom Ctrl out while right Alt is held. Nothing is lost: Windows cannot tell a
    // real Ctrl+AltGr from a plain AltGr either.
    if (modifiers & MOD_RIGHT_ALT) {
        modifiers &= static_cast<uint8_t>(~MOD_LEFT_CTRL);
    }

    report[0] = modifiers;
    report[1] = 0;

    size_t slot = 0;
    for (size_t usage = 0; usage < KEY_COUNT; ++usage) {
        if (!m_keys[usage]) continue;
        if (slot >= MAX_KEYS) {
            std::fill(report + 2, report + REPORT_SIZE, ERROR_ROLL_OVER);
            return;
        }
        report[2 + slot] = static_cast<uint8_t>(usage);
        ++slot;
    }
    std::fill(report + 2 + slot, report + REPORT_SIZE, static_cast<uint8_t>(0));
}

bool HidKeyboard::process_key(bool pressed, uint32_t scancode, bool extended, uint8_t* report) {
    if (!report) return false;

    // A modifier never occupies a key slot, it only sets a bit in byte 0 — and it must
    // always be sent: press 'a', then press Shift, and swallowing the Shift would leave
    // the device unaware that the modifier state changed.
    const uint8_t modifier = scancode_to_modifier(scancode, extended);
    if (modifier != 0) {
        if (pressed) {
            m_modifiers |= modifier;
        } else {
            m_modifiers &= static_cast<uint8_t>(~modifier);
        }
        build_report(report);
        return true;
    }

    const uint8_t usage = scancode_to_usage(scancode, extended);
    if (usage == 0 || usage >= KEY_COUNT) {
        return false;
    }

    m_keys[usage] = pressed;
    build_report(report);
    return true;
}

bool HidKeyboard::release_all(uint8_t* report) {
    if (!report) return false;

    bool anything_held = m_modifiers != 0;
    if (!anything_held) {
        anything_held = std::any_of(std::begin(m_keys), std::end(m_keys),
                                    [](bool down) { return down; });
    }
    if (!anything_held) {
        return false;
    }

    m_modifiers = 0;
    std::fill(std::begin(m_keys), std::end(m_keys), false);
    build_report(report);
    return true;
}

} // namespace pm::input
