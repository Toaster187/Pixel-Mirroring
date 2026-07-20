#pragma once
#include <string>
#include <memory>
#include <functional>
struct SDL_Renderer;
struct SDL_Window;

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

enum class MenuAction {
    FACTORY_RESET,
    TOGGLE_FPS_LIMIT,
    TOGGLE_RESOLUTION_LIMIT,
    SET_PIN,
    UNLOCK_DEVICE,
    LOCK_DEVICE,
    TOGGLE_COMPATIBILITY_MODE,
    TOGGLE_LOWEST_BRIGHTNESS
};

class IWindow {
public:
    virtual ~IWindow() = default;
    
    // Creates and initializes the window. Returns true on success.
    virtual bool create() = 0;
    
    // Shows the window on screen.
    virtual void show() = 0;
    
    // Hides the window on screen.
    virtual void hide() = 0;
    
    // Checks if the window is currently visible.
    virtual bool is_visible() const = 0;
    
    // Enters the main message loop. Blocks until the window is closed.
    virtual void process_messages() = 0;
    
    // Sets the aspect ratio to lock to (e.g. 9.0/16.0 for portrait).
    virtual void set_aspect_ratio(double ratio) = 0;
    
    // Signals the window that the orientation has changed.
    virtual void set_orientation(bool landscape) = 0;
    
    // Returns the underlying OS window handle (HWND or NSWindow*)
    virtual void* get_native_handle() = 0;

    // Set render callback. Paint latest frame in phone area.
    virtual void set_render_callback(std::function<void(SDL_Renderer*, int, int, int, int)> cb) = 0;

    // Set video viewport callback. Receive current phone area size.
    virtual void set_video_viewport_callback(std::function<void(int, int, int, int)> cb) = 0;

    // Set pointer callback. Handle click and drag in video.
    virtual void set_pointer_callback(std::function<void(PointerAction, int, int, int, int)> cb) = 0;
    
    // Set key callback. Cave man tap physical keys. Params: action (0 = down, 1 = up), keycode.
    virtual void set_key_callback(std::function<void(int, int)> cb) = 0;

    // Set text callback. Cave man write words. Params: text.
    virtual void set_text_callback(std::function<void(const std::string&)> cb) = 0;

    // Set scroll callback. Cave man scroll screen. Params: x, y, w, h, hscroll, vscroll.
    virtual void set_scroll_callback(std::function<void(int, int, int, int, float, float)> cb) = 0;
    
    
    // Sets the current app state (changes what is drawn in the phone area)
    virtual void set_app_state(AppState state) = 0;
    
    // Sets the status text shown in the phone area
    virtual void set_status_text(const std::string& text) = 0;
    
    // Sets a callback for when the user clicks the "Start" button in SETUP state
    virtual void set_start_callback(std::function<void()> cb) = 0;
    virtual void post_task(std::function<void()> task) = 0;

    // Set a callback for context menu actions
    virtual void set_menu_callback(std::function<void(MenuAction)> cb) = 0;

    // Set a callback for when the window is restored from another instance
    virtual void set_restore_callback(std::function<void()> cb) = 0;

    // Set checkbox states on the menu options from outside
    virtual void set_fps_limited(bool limited) = 0;
    virtual void set_resolution_limited(bool limited) = 0;
    virtual void set_compatibility_mode(bool enabled) = 0;
    virtual void set_lowest_brightness(bool enabled) = 0;

    // Clipboard callbacks & sync
    virtual void set_os_clipboard_update_callback(std::function<void(const std::string&)> cb) = 0;
    virtual void set_pc_clipboard(const std::string& text) = 0;
};

// Factory function to create the appropriate window implementation for the current OS.
std::unique_ptr<IWindow> create_window(int width, int height, const std::string& title);

} // namespace pm::window
