#pragma once

#include <cstddef>
#include <cstdint>

namespace pm::input {

/**
 * A virtual USB keyboard, synthesised on the PC and attached to the phone.
 *
 * inject_text tells the phone WHICH CHARACTER to insert. That works in a text field
 * and nowhere else — terminals, games and dead keys never see a key event at all, and
 * German umlauts are unreliable. A real keyboard reports something different: WHICH
 * PHYSICAL KEY was pressed. The phone then applies its OWN layout on top, so ä ö ü,
 * ^ ` ´ and every Ctrl/Alt combination behave exactly like on a keyboard plugged into
 * the phone by cable.
 *
 * This builds what the phone expects — the 8-byte boot keyboard report from the USB
 * HID specification:
 *
 *     byte 0: modifiers (Ctrl/Shift/Alt/Win, left and right)
 *     byte 1: reserved, always zero
 *     byte 2..7: up to six keys held down at the same time
 *
 * Keys arrive as PC set-1 scancodes (the hardware numbers Windows hands over in
 * the top bits of lParam), NOT as virtual keys — a virtual key already carries
 * the PC's layout, which is the very thing the phone must not inherit.
 */
class HidKeyboard {
public:
    // Modifiers + reserved + six key slots.
    static constexpr size_t REPORT_SIZE = 8;

    // The same id scrcpy itself uses for its keyboard. The device only needs it to
    // tell multiple virtual devices apart, so any value works — this one stays
    // consistent with upstream.
    static constexpr uint16_t DEVICE_ID = 1;

    // The HID report descriptor: it tells the device what kind of input device is
    // being attached. Sent once, in the UHID_CREATE message.
    static const uint8_t* report_descriptor(size_t* out_size);

    // Processes one press or release. Fills report and returns true when the device
    // needs to be told; false for keys a boot keyboard does not have (volume, media,
    // ...), which the caller then sends via the Android keycode path.
    bool process_key(bool pressed, uint32_t scancode, bool extended, uint8_t* report);

    // Releases everything. Without it, switching windows while holding Shift means the
    // device never sees the release and keeps typing in capitals. Returns false when
    // nothing was held in the first place.
    bool release_all(uint8_t* report);

private:
    void build_report(uint8_t* report) const;

    // 0x00..0x65 — one HID usage per key, exactly the range the report descriptor
    // declares (an AT-101 keyboard ends at 0x65, "Application").
    static constexpr size_t KEY_COUNT = 0x66;

    bool m_keys[KEY_COUNT] = {};
    uint8_t m_modifiers{0};
};

} // namespace pm::input
