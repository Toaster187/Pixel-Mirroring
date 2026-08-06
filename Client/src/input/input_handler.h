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

    // One physical key on the PC, identified by its hardware position (PC set-1
    // scancode) rather than by the character the PC layout assigns to it. Only sent
    // while the virtual USB keyboard is attached; otherwise it is dropped and the
    // inject_text/inject_keycode path does the work.
    //
    // Returns false when nothing was sent — no keyboard attached, or a key a boot
    // keyboard simply does not have. The caller MUST then fall back to the Android
    // keycode path, or the key disappears without a trace.
    bool handle_raw_key(int action, uint32_t scancode, bool extended);

    // Called when the window loses focus with modifiers still held. Releases
    // everything, or the phone keeps typing in capitals long after focus is gone.
    void release_all_keys();

    // Attaches a virtual USB keyboard to the device (probing first whether the device
    // allows it at all — see ScrcpyClient::device_supports_uhid). Returns false if the
    // device refused, in which case nothing changes and text input keeps working.
    bool enable_uhid_keyboard();

    // Unplugs it again and goes back to inject_text.
    void disable_uhid_keyboard();

    // Drops the keyboard state without sending anything — for a new session, where the
    // old control socket is already gone.
    void reset_uhid_keyboard();

private:
    void window_to_device(int wx, int wy, int ww, int wh, int* dx, int* dy);
    void send_hid_report(const uint8_t* report);

    pm::stream::ScrcpyClient* client_;
    // Atomic for the same reason as m_uhid_active below: since server 4.x the video
    // thread rewrites the size on every rotation, while the UI thread reads it for
    // every pointer event. They are still two separate stores, so an event landing
    // between them is mapped with a new width and an old height — one misplaced poke
    // at the instant of a rotation, which is the cheapest of the available outcomes.
    std::atomic<int> m_device_width{1080};
    std::atomic<int> m_device_height{1920};

    // Written by the connection thread when a session starts, read by the UI
    // thread on every keystroke — hence atomic. The HID state below is only ever
    // touched by the UI thread, and only while this flag is true.
    std::atomic<bool> m_uhid_active{false};
    HidKeyboard m_hid_keyboard;
};

} // namespace pm::input
