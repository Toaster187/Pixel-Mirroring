// Ugg! The scancode table in hid_keyboard.cpp is exactly the kind of stone wall that
// breaks quietly: one wrong number and a single key types the wrong letter on the
// phone, on a keyboard layout nobody here owns. Nothing on the way there complains.
//
// So this little cave presses keys at HidKeyboard and looks at the stones it carves.
// No test tribe, no new build system — one main(), one check(), and a count at the
// end. Build it with the client (target pm_hid_keyboard_test) and run it, or let
// ctest do it.

#include "../src/input/hid_keyboard.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

// NOT assert(): the client ships as Release, and Release carves NDEBUG, which makes
// every assert vanish. A test that silently passes is worse than no test.
void check(bool condition, const std::string& what) {
    ++g_checks;
    if (condition) return;
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
}

std::string bytes_to_text(const uint8_t* report) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%02X %02X %02X %02X %02X %02X %02X %02X",
                  report[0], report[1], report[2], report[3],
                  report[4], report[5], report[6], report[7]);
    return std::string(buffer);
}

void check_report(const uint8_t* report, uint8_t modifiers,
                  const std::initializer_list<uint8_t>& keys, const std::string& what) {
    uint8_t expected[pm::input::HidKeyboard::REPORT_SIZE] = {};
    expected[0] = modifiers;
    size_t slot = 0;
    for (uint8_t key : keys) {
        expected[2 + slot] = key;
        ++slot;
    }
    const bool same = std::memcmp(report, expected,
                                  pm::input::HidKeyboard::REPORT_SIZE) == 0;
    check(same, what + " (expected " + bytes_to_text(expected) +
                ", got " + bytes_to_text(report) + ")");
}

// Modifier bits, spelled out again so a wrong number in hid_keyboard.cpp cannot
// quietly agree with itself.
constexpr uint8_t L_CTRL  = 0x01;
constexpr uint8_t L_SHIFT = 0x02;
constexpr uint8_t L_ALT   = 0x04;
constexpr uint8_t L_GUI   = 0x08;
constexpr uint8_t R_CTRL  = 0x10;
constexpr uint8_t R_SHIFT = 0x20;
constexpr uint8_t R_ALT   = 0x40;
constexpr uint8_t R_GUI   = 0x80;

constexpr bool PLAIN = false;
constexpr bool EXTRA = true;   // the keyboard shouted E0 first

void test_report_descriptor() {
    std::printf("report descriptor\n");
    size_t size = 0;
    const uint8_t* desc = pm::input::HidKeyboard::report_descriptor(&size);
    check(desc != nullptr, "descriptor exists");
    check(size > 0, "descriptor has a size");
    // The phone builds its keyboard out of these bytes. Head and tail must be a
    // proper Generic-Desktop/Keyboard collection or Android sees a stranger.
    check(desc[0] == 0x05 && desc[1] == 0x01, "descriptor starts with Usage Page (Generic Desktop)");
    check(desc[2] == 0x09 && desc[3] == 0x06, "descriptor says Usage (Keyboard)");
    check(desc[size - 1] == 0xC0, "descriptor ends with End Collection");
    // Asking without a place to write the size must not fall over.
    check(pm::input::HidKeyboard::report_descriptor(nullptr) == desc,
          "descriptor pointer is the same without an out-size");
}

void test_plain_keys() {
    std::printf("plain keys\n");
    pm::input::HidKeyboard kb;
    uint8_t report[pm::input::HidKeyboard::REPORT_SIZE] = {};

    check(kb.process_key(true, 0x1E, PLAIN, report), "A goes down");
    check_report(report, 0, {0x04}, "A down is usage 0x04");

    check(kb.process_key(false, 0x1E, PLAIN, report), "A comes up");
    check_report(report, 0, {}, "A up is an empty report");

    // Two keys at once keep their places, sorted by usage — B (0x05) after A (0x04)
    // no matter which hand hit first.
    check(kb.process_key(true, 0x30, PLAIN, report), "B goes down");
    check(kb.process_key(true, 0x1E, PLAIN, report), "A goes down after B");
    check_report(report, 0, {0x04, 0x05}, "A and B ride in usage order");

    // The same key pressed twice is still one key.
    check(kb.process_key(true, 0x1E, PLAIN, report), "A pressed again");
    check_report(report, 0, {0x04, 0x05}, "a second A press changes nothing");
}

void test_modifiers() {
    std::printf("modifiers\n");
    pm::input::HidKeyboard kb;
    uint8_t report[pm::input::HidKeyboard::REPORT_SIZE] = {};

    check(kb.process_key(true, 0x2A, PLAIN, report), "left Shift travels");
    check_report(report, L_SHIFT, {}, "left Shift is bit 1 and takes no key slot");

    check(kb.process_key(true, 0x36, PLAIN, report), "right Shift travels");
    check_report(report, L_SHIFT | R_SHIFT, {}, "both Shifts at once");

    kb.process_key(false, 0x2A, PLAIN, report);
    kb.process_key(false, 0x36, PLAIN, report);
    check_report(report, 0, {}, "both Shifts let go");

    // Same scancode, different key — the E0 stone is what tells them apart.
    kb.process_key(true, 0x1D, PLAIN, report);
    check_report(report, L_CTRL, {}, "plain 0x1D is left Strg");
    kb.process_key(false, 0x1D, PLAIN, report);
    kb.process_key(true, 0x1D, EXTRA, report);
    check_report(report, R_CTRL, {}, "extended 0x1D is right Strg");
    kb.process_key(false, 0x1D, EXTRA, report);

    kb.process_key(true, 0x38, PLAIN, report);
    check_report(report, L_ALT, {}, "plain 0x38 is left Alt");
    kb.process_key(false, 0x38, PLAIN, report);

    kb.process_key(true, 0x5B, EXTRA, report);
    check_report(report, L_GUI, {}, "extended 0x5B is the left window key");
    kb.process_key(false, 0x5B, EXTRA, report);
    kb.process_key(true, 0x5C, EXTRA, report);
    check_report(report, R_GUI, {}, "extended 0x5C is the right window key");
    kb.process_key(false, 0x5C, EXTRA, report);

    // Letting go of something that was never held still has to be told: cave man
    // may have walked in while the key was already down.
    check(kb.process_key(false, 0x2A, PLAIN, report),
          "releasing an unheld modifier still sends a report");
}

void test_altgr_phantom_ctrl() {
    std::printf("AltGr phantom Strg\n");
    pm::input::HidKeyboard kb;
    uint8_t report[pm::input::HidKeyboard::REPORT_SIZE] = {};

    // This is exactly what Windows sends for one press of AltGr: a left Strg nobody
    // touched, then right Alt. Handing both to the phone turns AltGr+Q into
    // Strg+AltGr+Q and the "@" never appears.
    kb.process_key(true, 0x1D, PLAIN, report);
    check_report(report, L_CTRL, {}, "the phantom Strg alone still looks like Strg");

    kb.process_key(true, 0x38, EXTRA, report);
    check_report(report, R_ALT, {}, "while AltGr is held the phantom Strg is masked out");

    kb.process_key(true, 0x10, PLAIN, report);
    check_report(report, R_ALT, {0x14}, "AltGr+Q carries no Strg");

    kb.process_key(false, 0x10, PLAIN, report);
    kb.process_key(false, 0x38, EXTRA, report);
    // The mask hides the bit, it does not delete it. A REAL Strg that is still down
    // has to come back the moment AltGr goes up.
    check_report(report, L_CTRL, {}, "the Strg bit is only hidden, not thrown away");
}

void test_extended_versus_keypad() {
    std::printf("extended keys versus the number block\n");
    pm::input::HidKeyboard kb;
    uint8_t report[pm::input::HidKeyboard::REPORT_SIZE] = {};

    struct Pair {
        uint32_t scancode;
        uint8_t plain_usage;
        uint8_t extended_usage;
        const char* name;
    };
    // Every one of these numbers is the same on both sides of the E0 stone, and every
    // one of them means a different key. Swapping a pair sends arrow-up as a "8".
    const Pair pairs[] = {
        {0x1C, 0x28, 0x58, "Enter vs keypad Enter"},
        {0x35, 0x38, 0x54, "the -/ key vs keypad /"},
        {0x37, 0x55, 0x46, "keypad * vs PrintScreen"},
        {0x47, 0x5F, 0x4A, "keypad 7 vs Pos1"},
        {0x48, 0x60, 0x52, "keypad 8 vs arrow up"},
        {0x4B, 0x5C, 0x50, "keypad 4 vs arrow left"},
        {0x4D, 0x5E, 0x4F, "keypad 6 vs arrow right"},
        {0x50, 0x5A, 0x51, "keypad 2 vs arrow down"},
        {0x52, 0x62, 0x49, "keypad 0 vs Insert"},
        {0x53, 0x63, 0x4C, "keypad . vs Delete"},
    };

    for (const Pair& pair : pairs) {
        kb.process_key(true, pair.scancode, PLAIN, report);
        check_report(report, 0, {pair.plain_usage}, std::string("plain: ") + pair.name);
        kb.process_key(false, pair.scancode, PLAIN, report);

        kb.process_key(true, pair.scancode, EXTRA, report);
        check_report(report, 0, {pair.extended_usage}, std::string("extended: ") + pair.name);
        kb.process_key(false, pair.scancode, EXTRA, report);
    }
}

void test_keys_the_board_does_not_have() {
    std::printf("keys a boot keyboard does not have\n");
    pm::input::HidKeyboard kb;
    uint8_t report[pm::input::HidKeyboard::REPORT_SIZE] = {};
    std::memset(report, 0xEE, sizeof(report));

    // A "no" is the whole point: the window then walks the Android keycode road
    // instead, and the key does not disappear.
    check(!kb.process_key(true, 0x00, PLAIN, report), "scancode 0 is not a key");
    check(!kb.process_key(true, 0x54, PLAIN, report), "SysRq is not on the board");
    check(!kb.process_key(true, 0xFF, PLAIN, report), "a scancode past the table is not a key");
    check(!kb.process_key(true, 0x02, EXTRA, report), "an extended key nobody knows is not a key");
    check(report[0] == 0xEE, "a refused key leaves the report untouched");
    check(!kb.process_key(true, 0x1E, PLAIN, nullptr), "no report stone, no work");
}

void test_rollover() {
    std::printf("six fingers and one too many\n");
    pm::input::HidKeyboard kb;
    uint8_t report[pm::input::HidKeyboard::REPORT_SIZE] = {};

    // A, S, D, F, G, H — six holes, six slots, still a proper report.
    const uint32_t six[] = {0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23};
    for (uint32_t scancode : six) {
        kb.process_key(true, scancode, PLAIN, report);
    }
    check_report(report, 0, {0x04, 0x07, 0x09, 0x0A, 0x0B, 0x16},
                 "six keys fill all six slots in usage order");

    // The seventh does not push anybody out — the spec says shout rollover instead.
    kb.process_key(true, 0x24, PLAIN, report);
    check_report(report, 0, {0x01, 0x01, 0x01, 0x01, 0x01, 0x01},
                 "the seventh key turns the whole report into rollover");

    // Let one go and the report is a normal one again.
    kb.process_key(false, 0x1E, PLAIN, report);
    check_report(report, 0, {0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x16},
                 "back to six keys, back to a normal report");
}

void test_release_all() {
    std::printf("cave man walks away\n");
    pm::input::HidKeyboard kb;
    uint8_t report[pm::input::HidKeyboard::REPORT_SIZE] = {};

    check(!kb.release_all(report), "nothing held, nothing to say");

    kb.process_key(true, 0x2A, PLAIN, report);   // Shift
    kb.process_key(true, 0x1E, PLAIN, report);   // A
    check(kb.release_all(report), "something was held, so it has to be said");
    check_report(report, 0, {}, "letting go empties modifiers AND key slots");
    check(!kb.release_all(report), "letting go twice says nothing the second time");

    // Only a modifier down is still "something held" — otherwise the phone SHOUTS
    // IN CAPITALS forever after cave man walks to another window.
    kb.process_key(true, 0x36, PLAIN, report);
    check(kb.release_all(report), "a lone Shift also has to be let go");

    // Only a key down, no modifier.
    kb.process_key(true, 0x39, PLAIN, report);   // Space
    check(kb.release_all(report), "a lone key also has to be let go");
    check(!kb.release_all(report), "and then there is nothing left");
    check(!kb.release_all(nullptr), "no report stone, no work");
}

} // namespace

int main() {
    std::printf("HidKeyboard report tests\n\n");

    test_report_descriptor();
    test_plain_keys();
    test_modifiers();
    test_altgr_phantom_ctrl();
    test_extended_versus_keypad();
    test_keys_the_board_does_not_have();
    test_rollover();
    test_release_all();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
