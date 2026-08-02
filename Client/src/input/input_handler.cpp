#include "input_handler.h"
#include <algorithm>

namespace pm::input {

namespace {
// The name the phone shows under Einstellungen -> System -> Sprachen & Eingabe ->
// Physische Tastatur, where the German layout gets assigned. scrcpy puts its own
// "scrcpy: " in front, so the human sees "scrcpy: Pixel Mirroring".
const char* const UHID_KEYBOARD_NAME = "Pixel Mirroring";
}

InputHandler::InputHandler(pm::stream::ScrcpyClient* client) : client_(client) {}

void InputHandler::set_device_size(int width, int height) {
    if (width > 0 && height > 0) {
        m_device_width = width;
        m_device_height = height;
    }
}

void InputHandler::window_to_device(int wx, int wy, int ww, int wh, int* dx, int* dy) {
    if (ww <= 0 || wh <= 0) {
        *dx = 0;
        *dy = 0;
        return;
    }

    *dx = (wx * m_device_width) / ww;
    *dy = (wy * m_device_height) / wh;
    *dx = (std::max)(0, (std::min)(*dx, m_device_width - 1));
    *dy = (std::max)(0, (std::min)(*dy, m_device_height - 1));
}

void InputHandler::handle_pointer(pm::window::PointerAction action, int x, int y, int viewport_w, int viewport_h) {
    int dx, dy;
    window_to_device(x, y, viewport_w, viewport_h, &dx, &dy);

    int scrcpy_action = 2;
    if (action == pm::window::PointerAction::DOWN) {
        scrcpy_action = 0;
    } else if (action == pm::window::PointerAction::UP) {
        scrcpy_action = 1;
    }

    client_->inject_touch(scrcpy_action, dx, dy, m_device_width, m_device_height);
}

void InputHandler::handle_mouse_down(int x, int y, int ww, int wh) {
    int dx, dy;
    window_to_device(x, y, ww, wh, &dx, &dy);
    client_->inject_touch(0, dx, dy, m_device_width, m_device_height); // ACTION_DOWN
}

void InputHandler::handle_mouse_up(int x, int y, int ww, int wh) {
    int dx, dy;
    window_to_device(x, y, ww, wh, &dx, &dy);
    client_->inject_touch(1, dx, dy, m_device_width, m_device_height); // ACTION_UP
}

void InputHandler::handle_mouse_move(int x, int y, int ww, int wh) {
    int dx, dy;
    window_to_device(x, y, ww, wh, &dx, &dy);
    client_->inject_touch(2, dx, dy, m_device_width, m_device_height); // ACTION_MOVE
}

void InputHandler::handle_key_down(int keycode) {
    client_->inject_keycode(0, keycode);
}

void InputHandler::handle_key_up(int keycode) {
    client_->inject_keycode(1, keycode);
}

void InputHandler::handle_scroll(int x, int y, int ww, int wh, float hscroll, float vscroll) {
    // scroll convert window to device. cave man spin wheel.
    int dx, dy;
    window_to_device(x, y, ww, wh, &dx, &dy);
    client_->inject_scroll(static_cast<float>(dx), static_cast<float>(dy), m_device_width, m_device_height, hscroll, vscroll);
}

void InputHandler::handle_text(const std::string& text) {
    // Ugg! With the USB keyboard hanging on the phone the key itself already flew
    // over. Writing the letter a second time would type everything twice.
    if (m_uhid_active.load()) return;

    // text inject. cave man send string.
    client_->inject_text(text);
}

void InputHandler::send_hid_report(const uint8_t* report) {
    client_->uhid_input(HidKeyboard::DEVICE_ID, report, HidKeyboard::REPORT_SIZE);
}

bool InputHandler::handle_raw_key(int action, uint32_t scancode, bool extended) {
    if (!m_uhid_active.load()) return false;

    uint8_t report[HidKeyboard::REPORT_SIZE];
    if (!m_hid_keyboard.process_key(action == 0, scancode, extended, report)) {
        return false;
    }
    send_hid_report(report);
    return true;
}

void InputHandler::release_all_keys() {
    if (!m_uhid_active.load()) return;

    uint8_t report[HidKeyboard::REPORT_SIZE];
    if (!m_hid_keyboard.release_all(report)) {
        return;
    }
    send_hid_report(report);
}

bool InputHandler::enable_uhid_keyboard() {
    if (m_uhid_active.load()) return true;
    if (!client_->device_supports_uhid()) return false;

    size_t desc_size = 0;
    const uint8_t* desc = HidKeyboard::report_descriptor(&desc_size);
    if (!client_->uhid_create(HidKeyboard::DEVICE_ID, UHID_KEYBOARD_NAME, desc, desc_size)) {
        return false;
    }

    // Fresh board, no key held. Set the flag LAST — until it is true nobody else
    // looks at the state above.
    m_hid_keyboard = HidKeyboard();
    m_uhid_active.store(true);
    return true;
}

void InputHandler::disable_uhid_keyboard() {
    if (!m_uhid_active.load()) return;

    release_all_keys();
    m_uhid_active.store(false);
    client_->uhid_destroy(HidKeyboard::DEVICE_ID);
}

void InputHandler::reset_uhid_keyboard() {
    m_uhid_active.store(false);
    m_hid_keyboard = HidKeyboard();
}

} // namespace pm::input
