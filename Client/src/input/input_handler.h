#pragma once
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
    
private:
    void window_to_device(int wx, int wy, int ww, int wh, int* dx, int* dy);
    
    pm::stream::ScrcpyClient* client_;
    int device_width_{1080};
    int device_height_{1920};
};

} // namespace pm::input
