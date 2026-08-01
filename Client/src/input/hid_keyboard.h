#pragma once

#include <cstddef>
#include <cstdint>

namespace pm::input {

/**
 * A plain USB keyboard, built out of thin air and hung on the phone.
 *
 * Ugg! inject_text tells the phone WHICH LETTER cave man wants. That works in a
 * chat box and nowhere else — terminals, games and dead keys never see a key at
 * all, and a German umlaut is a coin toss. A real keyboard says something else:
 * it says WHICH HOLE was hit. The phone then lays its OWN German layout on top,
 * so ä ö ü, ^ ` ´ and every Strg/Alt combo come out exactly like on a keyboard
 * plugged into the phone with a cable.
 *
 * This carves the little stone the phone expects — the 8-byte boot keyboard
 * report from the USB HID spec:
 *
 *     byte 0: modifiers (Strg/Shift/Alt/Win, left and right)
 *     byte 1: always empty
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

    // Same id scrcpy itself uses for its keyboard. The phone only needs it to
    // tell several fake devices apart, so any number works — this one keeps us
    // honest with upstream.
    static constexpr uint16_t DEVICE_ID = 1;

    // The drawing that tells the phone what kind of device is being plugged in.
    // Handed to the phone once, in the UHID_CREATE message.
    static const uint8_t* report_descriptor(size_t* out_size);

    // Feeds one press or release. Fills report and returns true when the phone
    // needs to hear about it; false for keys a boot keyboard simply does not
    // have (volume, media, ...), which the caller then sends the old way.
    bool process_key(bool pressed, uint32_t scancode, bool extended, uint8_t* report);

    // Ugg! Cave man walks to other window while still holding Shift. Phone never
    // hears the release and screams in capitals forever. This lets everything go.
    // Returns false when nothing was held anyway.
    bool release_all(uint8_t* report);

private:
    void build_report(uint8_t* report) const;

    // 0x00..0x65 — a HID usage per key, exactly the range the report descriptor
    // below announces (an AT-101 keyboard ends at 0x65 "Application").
    static constexpr size_t KEY_COUNT = 0x66;

    bool m_keys[KEY_COUNT] = {};
    uint8_t m_modifiers{0};
};

} // namespace pm::input
