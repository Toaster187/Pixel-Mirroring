// Pins the scrcpy wire layouts in src/stream/control_messages.h against the bundled
// server 4.1.
//
// This exists because every one of these fields moved between server versions, and each
// of them fails silently rather than loudly. A UHID_CREATE with the 2.7 layout makes the
// server read the name length out of the middle of the report description and take down
// its whole control thread — keys, mouse, touch and clipboard stop while video keeps
// running, with nothing in any log to say why. The same for the packet flags: read them
// one bit too high and every frame looks like a config packet.
//
// Same shape as hid_keyboard_test.cpp: a plain main() with a hand-rolled check(),
// deliberately NOT assert — the shipped build defines NDEBUG and would compile every
// assertion away.

#include "../src/stream/control_messages.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace wire = pm::stream::wire;

static int g_failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

static void check_bytes(const std::vector<uint8_t>& actual,
                        const std::vector<uint8_t>& expected,
                        const char* what) {
    if (actual == expected) {
        return;
    }
    std::printf("FAIL: %s\n  expected:", what);
    for (uint8_t b : expected) std::printf(" %02x", b);
    std::printf("\n  actual:  ");
    for (uint8_t b : actual) std::printf(" %02x", b);
    std::printf("\n");
    ++g_failures;
}

// --- Message type numbers -------------------------------------------------------

static void test_message_types() {
    check(wire::MSG_SET_DISPLAY_POWER == 10, "SET_DISPLAY_POWER is type 10");
    check(wire::MSG_UHID_CREATE == 12, "UHID_CREATE is type 12");
    check(wire::MSG_UHID_INPUT == 13, "UHID_INPUT is type 13");
    check(wire::MSG_UHID_DESTROY == 14, "UHID_DESTROY is type 14");
    check(wire::MSG_OPEN_HARD_KEYBOARD_SETTINGS == 15, "OPEN_HARD_KEYBOARD_SETTINGS is type 15");
    check(wire::MSG_START_APP == 16, "START_APP is type 16");
}

// --- UHID_CREATE ----------------------------------------------------------------
// type(1) id(2 BE) vendor(2 BE) product(2 BE) name_len(1) name rd_size(2 BE) rd_data

static void test_uhid_create_layout() {
    const uint8_t desc[3] = {0xAA, 0xBB, 0xCC};
    const std::vector<uint8_t> msg = wire::build_uhid_create(0x0102, "kb", desc, sizeof(desc));

    check_bytes(msg, {
        12,          // type
        0x01, 0x02,  // id, big endian
        0x00, 0x00,  // vendor  — zero for keyboards and mice, only gamepads carry real ids
        0x00, 0x00,  // product
        0x02,        // name length
        'k', 'b',
        0x00, 0x03,  // report descriptor size, big endian
        0xAA, 0xBB, 0xCC
    }, "UHID_CREATE full layout");

    // The 2.7 shape was four bytes shorter. If this ever matches again, the vendor and
    // product fields have been dropped and the server will misread every name length.
    check(msg.size() == 8 + 2 + 2 + sizeof(desc), "UHID_CREATE has the 4-byte vendor/product block");
    check(msg[3] == 0 && msg[4] == 0 && msg[5] == 0 && msg[6] == 0,
          "UHID_CREATE vendor and product are zero");
}

static void test_uhid_create_name_rules() {
    const uint8_t desc[1] = {0x01};

    // A name of exactly the limit still fits in the single length byte.
    const std::string max_name(wire::UHID_MAX_NAME_LENGTH, 'x');
    const std::vector<uint8_t> at_limit = wire::build_uhid_create(1, max_name, desc, sizeof(desc));
    check(at_limit[7] == wire::UHID_MAX_NAME_LENGTH, "name of exactly 127 bytes is kept");

    // One byte more is dropped WHOLE, never truncated: cutting UTF-8 mid-sequence
    // corrupts umlauts, and the device falls back to plain "scrcpy" for an empty name.
    const std::string too_long(wire::UHID_MAX_NAME_LENGTH + 1, 'x');
    const std::vector<uint8_t> over = wire::build_uhid_create(1, too_long, desc, sizeof(desc));
    check(over[7] == 0, "an over-long name is dropped, not truncated");
    check(over.size() == 8 + 2 + sizeof(desc), "a dropped name leaves no bytes behind");

    // Nothing sendable produces nothing.
    check(wire::build_uhid_create(1, "kb", nullptr, 4).empty(), "no descriptor, no message");
    check(wire::build_uhid_create(1, "kb", desc, 0).empty(), "empty descriptor, no message");
    check(wire::build_uhid_create(1, "kb", desc, 0x10000).empty(),
          "descriptor beyond a 16-bit size, no message");
}

// --- START_APP ------------------------------------------------------------------
// type(1) len(1) package_name

static void test_start_app_layout() {
    const std::vector<uint8_t> msg = wire::build_start_app("org.fossify.home");
    check_bytes(msg, {
        16, 16,
        'o','r','g','.','f','o','s','s','i','f','y','.','h','o','m','e'
    }, "START_APP full layout");

    check(wire::build_start_app("").empty(), "an empty package name sends nothing");
    const std::string too_long(wire::START_APP_MAX_NAME_LENGTH + 1, 'a');
    check(wire::build_start_app(too_long).empty(), "an unsendable package name sends nothing");
}

// --- SET_DISPLAY_POWER ----------------------------------------------------------

static void test_set_display_power() {
    // Server 3.0 turned the old power-mode number into a plain boolean. The old "2" for
    // NORMAL would still read as true over there, but only by accident.
    check_bytes(wire::build_set_display_power(true), {10, 1}, "SET_DISPLAY_POWER on");
    check_bytes(wire::build_set_display_power(false), {10, 0}, "SET_DISPLAY_POWER off");
}

// --- UHID_INPUT -----------------------------------------------------------------
// type(1) id(2 BE) size(2 BE) report
//
// The message every single key press and release goes through. A wrong size prefix here
// kills the server's control thread exactly like a wrong UHID_CREATE does.

static void test_uhid_input_layout() {
    const uint8_t report[8] = {0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00}; // Shift+A
    uint8_t out[wire::UHID_INPUT_HEADER_SIZE + wire::UHID_MAX_REPORT_SIZE];

    const size_t length = wire::build_uhid_input(out, sizeof(out), 0x0102, report, sizeof(report));
    check(length == wire::UHID_INPUT_HEADER_SIZE + sizeof(report), "UHID_INPUT is header plus report");
    check_bytes(std::vector<uint8_t>(out, out + length), {
        13,          // type
        0x01, 0x02,  // id, big endian
        0x00, 0x08,  // report size, big endian
        0x02, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
    }, "UHID_INPUT full layout");
}

static void test_uhid_input_refusals() {
    const uint8_t report[8] = {0};
    uint8_t out[wire::UHID_INPUT_HEADER_SIZE + wire::UHID_MAX_REPORT_SIZE];

    check(wire::build_uhid_input(out, sizeof(out), 1, nullptr, 8) == 0, "no report, no message");
    check(wire::build_uhid_input(out, sizeof(out), 1, report, 0) == 0, "empty report, no message");

    // Over the cap the buffer was sized against. Sending it anyway would write past the
    // stack array in the hot path.
    const uint8_t oversized[wire::UHID_MAX_REPORT_SIZE + 1] = {0};
    check(wire::build_uhid_input(out, sizeof(out), 1, oversized, sizeof(oversized)) == 0,
          "a report beyond the cap sends nothing");

    // A buffer too small is refused rather than filled halfway.
    uint8_t tiny[4];
    check(wire::build_uhid_input(tiny, sizeof(tiny), 1, report, sizeof(report)) == 0,
          "a buffer that cannot hold the message sends nothing");
}

// --- UHID_DESTROY and OPEN_HARD_KEYBOARD_SETTINGS -------------------------------

static void test_uhid_destroy_layout() {
    check_bytes(wire::build_uhid_destroy(0x0102), {14, 0x01, 0x02}, "UHID_DESTROY full layout");
}

static void test_open_hard_keyboard_settings_layout() {
    // A bare type byte: any payload at all would shift every following message.
    check_bytes(wire::build_open_hard_keyboard_settings(), {15},
                "OPEN_HARD_KEYBOARD_SETTINGS carries no payload");
}

// --- INJECT_SCROLL --------------------------------------------------------------
// type(1) x(4 BE) y(4 BE) w(2 BE) h(2 BE) hscroll(2 BE) vscroll(2 BE) buttons(4 BE)

static void test_inject_scroll_layout() {
    // Coordinates stay well under 2^24 so the float parameters carry them exactly — the
    // test pins the byte order, not the rounding of a number no screen is that wide for.
    const auto msg = wire::build_inject_scroll(0x00010203, 0x00040506, 1080, 2424, 0.0f, 1.0f);
    check(msg.size() == wire::INJECT_SCROLL_SIZE, "INJECT_SCROLL is 21 bytes");
    check_bytes(std::vector<uint8_t>(msg.begin(), msg.end()), {
        3,                       // type
        0x00, 0x01, 0x02, 0x03,  // x
        0x00, 0x04, 0x05, 0x06,  // y
        0x04, 0x38,              // width 1080
        0x09, 0x78,              // height 2424
        0x00, 0x00,              // hscroll
        0x02, 0x00,              // vscroll: one notch = 512
        0x00, 0x00, 0x00, 0x00   // buttons: none held
    }, "INJECT_SCROLL full layout");
}

static void test_inject_scroll_clamps_coordinates() {
    // A pointer dragged past the left or top edge must not wrap through the unsigned
    // cast — that would put the event at roughly four billion on the phone.
    const auto msg = wire::build_inject_scroll(-5.0f, -5.0f, 100, 100, 0.0f, -1.0f);
    check(msg[1] == 0 && msg[2] == 0 && msg[3] == 0 && msg[4] == 0, "a negative x clamps to zero");
    check(msg[5] == 0 && msg[6] == 0 && msg[7] == 0 && msg[8] == 0, "a negative y clamps to zero");

    // -512 as int16 reinterpreted unsigned.
    check(msg[15] == 0xFE && msg[16] == 0x00, "a backwards notch is -512");
}

// --- Video packet flags ---------------------------------------------------------

static void test_packet_flags() {
    // Server 4.0 took bit 63 for session packets and pushed the older two down one.
    check(wire::PACKET_FLAG_SESSION == (1ULL << 63), "SESSION is bit 63");
    check(wire::PACKET_FLAG_CONFIG == (1ULL << 62), "CONFIG is bit 62 (was 63 before 4.0)");
    check(wire::PACKET_FLAG_KEY_FRAME == (1ULL << 61), "KEY_FRAME is bit 61 (was 62 before 4.0)");

    // The three must not overlap, or one packet kind reads as another.
    check((wire::PACKET_FLAG_SESSION & wire::PACKET_FLAG_CONFIG) == 0, "SESSION and CONFIG differ");
    check((wire::PACKET_FLAG_CONFIG & wire::PACKET_FLAG_KEY_FRAME) == 0, "CONFIG and KEY_FRAME differ");
    check((wire::PACKET_FLAG_SESSION & wire::PACKET_FLAG_KEY_FRAME) == 0, "SESSION and KEY_FRAME differ");
}

// --- Session packet parsing -----------------------------------------------------

static void test_session_packet_read() {
    // The twelve bytes a session packet occupies: flags, then width and height as two
    // big-endian 32-bit values at offsets 4 and 8.
    const uint8_t packet[12] = {
        0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x04, 0x38,  // 1080
        0x00, 0x00, 0x09, 0x78   // 2424
    };
    check((packet[0] & 0x80) != 0, "top bit marks a session packet");
    check(wire::read32be(packet + 4) == 1080, "width reads out of offset 4");
    check(wire::read32be(packet + 8) == 2424, "height reads out of offset 8");
}

// --- Scroll scaling -------------------------------------------------------------

static void test_scroll_scaling() {
    // Since server 3.0 the device multiplies scroll by 16, so the client sends a
    // sixteenth of the old value. Without this every wheel notch scrolls 16-fold.
    check(wire::scroll_to_fixed_point(1.0f) == 512, "one notch is 8192/16");
    check(wire::scroll_to_fixed_point(-1.0f) == -512, "one notch back is -8192/16");
    check(wire::scroll_to_fixed_point(0.0f) == 0, "no scroll is zero");

    // Clamped, not wrapped: a wrap turns a hard scroll up into a hard scroll down.
    check(wire::scroll_to_fixed_point(1000.0f) == 32767, "a huge scroll clamps to int16 max");
    check(wire::scroll_to_fixed_point(-1000.0f) == -32768, "a huge back-scroll clamps to int16 min");
}

int main() {
    test_message_types();
    test_uhid_create_layout();
    test_uhid_create_name_rules();
    test_start_app_layout();
    test_set_display_power();
    test_uhid_input_layout();
    test_uhid_input_refusals();
    test_uhid_destroy_layout();
    test_open_hard_keyboard_settings_layout();
    test_inject_scroll_layout();
    test_inject_scroll_clamps_coordinates();
    test_packet_flags();
    test_session_packet_read();
    test_scroll_scaling();

    if (g_failures > 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("control_messages: all checks passed\n");
    return EXIT_SUCCESS;
}
