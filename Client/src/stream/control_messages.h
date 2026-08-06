#pragma once
// Byte layouts of the scrcpy wire protocol, deliberately free of sockets, FFmpeg and
// everything else in ScrcpyClient. They live here so pm_control_messages_test can pin
// them without linking half the client.
//
// This is not tidiness for its own sake. These layouts moved between server versions in
// ways that fail silently: a wrong UHID_CREATE makes the server read the name length out
// of the middle of the report description and tears down its ENTIRE control thread —
// keys, mouse, touch and clipboard die while video keeps running, with nothing in any
// log. The bundled server is 4.1; verify against that jar before touching a number here.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace pm::stream::wire {

// Control message types beyond the old handful.
inline constexpr uint8_t MSG_SET_DISPLAY_POWER = 10;
inline constexpr uint8_t MSG_UHID_CREATE = 12;
inline constexpr uint8_t MSG_UHID_INPUT = 13;
inline constexpr uint8_t MSG_UHID_DESTROY = 14;
inline constexpr uint8_t MSG_OPEN_HARD_KEYBOARD_SETTINGS = 15;
inline constexpr uint8_t MSG_START_APP = 16;

// Both fields are length-prefixed with a single byte, so neither can be longer. Separate
// constants on purpose: the two share a limit, not a meaning.
inline constexpr size_t UHID_MAX_NAME_LENGTH = 127;
inline constexpr size_t START_APP_MAX_NAME_LENGTH = 127;

// Video packet flags. Server 4.0 took the top bit for session packets and pushed both
// older flags down one. Reading the old positions makes every frame look like a config
// packet, which is what a version mismatch looks like from here.
inline constexpr uint64_t PACKET_FLAG_SESSION = 1ULL << 63;
inline constexpr uint64_t PACKET_FLAG_CONFIG = 1ULL << 62;
inline constexpr uint64_t PACKET_FLAG_KEY_FRAME = 1ULL << 61;

// Since server 3.0 the device multiplies every scroll value by 16 (the old decoder read
// the range as [-1,1] when it really was [-16,16]). Sending a sixteenth keeps one wheel
// notch worth one wheel notch.
inline constexpr float SCROLL_FIXED_POINT_SCALE = 8192.0f / 16.0f;

inline void write16be(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xff);
    out[1] = static_cast<uint8_t>(value & 0xff);
}

inline uint32_t read32be(const uint8_t* in) {
    return (static_cast<uint32_t>(in[0]) << 24) | (static_cast<uint32_t>(in[1]) << 16)
         | (static_cast<uint32_t>(in[2]) << 8) | static_cast<uint32_t>(in[3]);
}

// type(1) id(2 BE) vendor(2 BE) product(2 BE) name_len(1) name rd_size(2 BE) rd_data
//
// Vendor and product are 0 for keyboards and mice; only gamepads carry real ids. Server
// 2.7 had neither field, and the name field does not exist before 2.7 at all.
//
// A name longer than one length byte can express is dropped entirely rather than cut:
// truncating UTF-8 mid-sequence corrupts umlauts, and the device falls back to plain
// "scrcpy" for an empty name. Returns an empty vector when there is nothing to send.
inline std::vector<uint8_t> build_uhid_create(uint16_t id, const std::string& name,
                                              const uint8_t* report_desc, size_t desc_size) {
    if (!report_desc || desc_size == 0 || desc_size > 0xFFFF) {
        return {};
    }

    constexpr uint16_t UHID_VENDOR_ID = 0;
    constexpr uint16_t UHID_PRODUCT_ID = 0;
    const size_t name_length = name.size() <= UHID_MAX_NAME_LENGTH ? name.size() : 0;

    std::vector<uint8_t> buf(8 + name_length + 2 + desc_size);
    buf[0] = MSG_UHID_CREATE;
    write16be(buf.data() + 1, id);
    write16be(buf.data() + 3, UHID_VENDOR_ID);
    write16be(buf.data() + 5, UHID_PRODUCT_ID);
    buf[7] = static_cast<uint8_t>(name_length);
    if (name_length > 0) {
        std::memcpy(buf.data() + 8, name.data(), name_length);
    }
    write16be(buf.data() + 8 + name_length, static_cast<uint16_t>(desc_size));
    std::memcpy(buf.data() + 10 + name_length, report_desc, desc_size);
    return buf;
}

// type(1) len(1) package_name. Returns an empty vector for a name the wire cannot carry.
inline std::vector<uint8_t> build_start_app(const std::string& package_name) {
    if (package_name.empty() || package_name.size() > START_APP_MAX_NAME_LENGTH) {
        return {};
    }

    std::vector<uint8_t> buf(2 + package_name.size());
    buf[0] = MSG_START_APP;
    buf[1] = static_cast<uint8_t>(package_name.size());
    std::memcpy(buf.data() + 2, package_name.data(), package_name.size());
    return buf;
}

// Since server 3.0 the payload is a plain boolean, not the old power mode number. The
// old 2 would still read as "true" over there, but only by accident.
inline std::vector<uint8_t> build_set_display_power(bool on) {
    return {MSG_SET_DISPLAY_POWER, static_cast<uint8_t>(on ? 1 : 0)};
}

// Fixed-point scroll value, clamped to what an int16 can carry.
inline int16_t scroll_to_fixed_point(float value) {
    const float scaled = value * SCROLL_FIXED_POINT_SCALE;
    if (scaled <= -32768.0f) return -32768;
    if (scaled >= 32767.0f) return 32767;
    return static_cast<int16_t>(scaled);
}

} // namespace pm::stream::wire
