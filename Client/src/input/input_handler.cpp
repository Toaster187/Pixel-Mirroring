#include "input_handler.h"
#include <algorithm>

namespace pm::input {

InputHandler::InputHandler(pm::stream::ScrcpyClient* client) : client_(client) {}

void InputHandler::set_device_size(int width, int height) {
    if (width > 0 && height > 0) {
        device_width_ = width;
        device_height_ = height;
    }
}

void InputHandler::window_to_device(int wx, int wy, int ww, int wh, int* dx, int* dy) {
    if (ww <= 0 || wh <= 0) {
        *dx = 0;
        *dy = 0;
        return;
    }

    *dx = (wx * device_width_) / ww;
    *dy = (wy * device_height_) / wh;
    *dx = (std::max)(0, (std::min)(*dx, device_width_ - 1));
    *dy = (std::max)(0, (std::min)(*dy, device_height_ - 1));
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

    client_->inject_touch(scrcpy_action, dx, dy, device_width_, device_height_);
}

void InputHandler::handle_mouse_down(int x, int y, int ww, int wh) {
    int dx, dy;
    window_to_device(x, y, ww, wh, &dx, &dy);
    client_->inject_touch(0, dx, dy, device_width_, device_height_); // ACTION_DOWN
}

void InputHandler::handle_mouse_up(int x, int y, int ww, int wh) {
    int dx, dy;
    window_to_device(x, y, ww, wh, &dx, &dy);
    client_->inject_touch(1, dx, dy, device_width_, device_height_); // ACTION_UP
}

void InputHandler::handle_mouse_move(int x, int y, int ww, int wh) {
    int dx, dy;
    window_to_device(x, y, ww, wh, &dx, &dy);
    client_->inject_touch(2, dx, dy, device_width_, device_height_); // ACTION_MOVE
}

void InputHandler::handle_key_down(int keycode) {
    client_->inject_keycode(0, keycode);
}

void InputHandler::handle_key_up(int keycode) {
    client_->inject_keycode(1, keycode);
}

} // namespace pm::input
