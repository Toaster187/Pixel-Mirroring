#pragma once
#include <atomic>
#include "hid_keyboard.h"
#include "../stream/scrcpy_client.h"
#include "../window/window_interface.h"

namespace pm::input {

class InputHandler {
public:
    InputHandler(pm::stream::ScrcpyClient* client);

    void set_device_size(int width, int height);
    void handle_pointer(pm::window::PointerAction action, int x, int y, int viewport_w, int viewport_h);

    void handle_mouse_down(int x, int y, int window_w, int window_h);
    void handle_mouse_up(int x, int y, int window_w, int window_h);
    void handle_mouse_move(int x, int y, int window_w, int window_h);
    void handle_key_down(int keycode);
    void handle_key_up(int keycode);
    void handle_scroll(int x, int y, int window_w, int window_h, float hscroll, float vscroll);
    void handle_text(const std::string& text);

    // One physical key on the PC, named by its hardware position (PC set-1
    // scancode), not by the letter the PC layout painted on it. Only travels
    // while the virtual USB keyboard is alive; otherwise it is dropped and the
    // old inject_text/inject_keycode path does the work.
    void handle_raw_key(int action, uint32_t scancode, bool extended);

    // Ugg! Window loses focus with Shift still down. Let go of everything, or the
    // phone keeps shouting in capitals long after cave man walked away.
    void release_all_keys();

    // Hangs a real USB keyboard on the phone (asks first whether the phone even
    // allows it — see ScrcpyClient::device_supports_uhid). Returns false when the
    // phone said no, and then nothing changes: text input keeps working.
    bool enable_uhid_keyboard();

    // Unplugs it again and goes back to inject_text.
    void disable_uhid_keyboard();

    // Forgets the keyboard without talking to the phone — for a new session,
    // where the old control socket is long gone.
    void reset_uhid_keyboard();

private:
    void window_to_device(int wx, int wy, int ww, int wh, int* dx, int* dy);
    void send_hid_report(const uint8_t* report);

    pm::stream::ScrcpyClient* client_;
    int m_device_width{1080};
    int m_device_height{1920};

    // Written by the connection thread when a session starts, read by the UI
    // thread on every keystroke — hence atomic. The HID state below is only ever
    // touched by the UI thread, and only while this flag is true.
    std::atomic<bool> m_uhid_active{false};
    HidKeyboard m_hid_keyboard;
};

} // namespace pm::input
