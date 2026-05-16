#pragma once
#include <string>
#include <memory>
#include <functional>

namespace pm::window {

// Cave man state machine — each state = different cave painting on phone screen
enum class AppState {
    SETUP,      // Show first-time setup instructions
    SCANNING,   // Scanning network for Android device
    CONNECTED,  // Found device, starting stream
    STREAMING   // Video stream active
};

enum class PointerAction {
    DOWN,
    MOVE,
    UP
};

class IWindow {
public:
    virtual ~IWindow() = default;
    
    // Creates and initializes the window. Returns true on success.
    virtual bool create() = 0;
    
    // Shows the window on screen.
    virtual void show() = 0;
    
    // Enters the main message loop. Blocks until the window is closed.
    virtual void process_messages() = 0;
    
    // Sets the aspect ratio to lock to (e.g. 9.0/16.0 for portrait).
    virtual void set_aspect_ratio(double ratio) = 0;
    
    // Signals the window that the orientation has changed.
    virtual void set_orientation(bool landscape) = 0;
    
    // Returns the underlying OS window handle (HWND or NSWindow*)
    virtual void* get_native_handle() = 0;

    // Sets a callback to paint the latest video frame into the phone area.
    virtual void set_render_callback(std::function<void(void*, int, int, int, int)> cb) = 0;

    // Sets a callback that receives the current video viewport inside the window.
    virtual void set_video_viewport_callback(std::function<void(int, int, int, int)> cb) = 0;

    // Sets a callback for pointer input inside the video viewport.
    virtual void set_pointer_callback(std::function<void(PointerAction, int, int, int, int)> cb) = 0;
    
    // Sets the current app state (changes what is drawn in the phone area)
    virtual void set_app_state(AppState state) = 0;
    
    // Sets the status text shown in the phone area
    virtual void set_status_text(const std::string& text) = 0;
    
    // Sets a callback for when the user clicks the "Start" button in SETUP state
    virtual void set_start_callback(std::function<void()> cb) = 0;
};

// Factory function to create the appropriate window implementation for the current OS.
std::unique_ptr<IWindow> create_window(int width, int height, const std::string& title);

} // namespace pm::window
