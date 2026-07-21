#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <functional>
#include <mutex>
#include "window_interface.h"

struct SDL_Renderer;
struct SDL_Window;

namespace pm::window {

class Win32Window : public IWindow {
public:
    Win32Window(int width, int height, const std::string& title);
    ~Win32Window() override;

    bool create() override;
    void show() override;
    void hide() override;
    bool is_visible() const override;
    void process_messages() override;
    
    void set_aspect_ratio(double ratio) override;
    void set_orientation(bool landscape) override;
    
    void* get_native_handle() override { return hwnd_; }
    void set_render_callback(std::function<void(SDL_Renderer*, int, int, int, int)> cb) override { m_render_cb_ = std::move(cb); }
    void set_video_viewport_callback(std::function<void(int, int, int, int)> cb) override;
    void set_pointer_callback(std::function<void(PointerAction, int, int, int, int)> cb) override { m_pointer_cb_ = std::move(cb); }
    void set_key_callback(std::function<void(int, int)> cb) override { m_key_cb_ = std::move(cb); }
    void set_text_callback(std::function<void(const std::string&)> cb) override { m_text_cb_ = std::move(cb); }
    void set_scroll_callback(std::function<void(int, int, int, int, float, float)> cb) override { m_scroll_cb_ = std::move(cb); }
    void set_app_state(AppState state) override;
    void set_status_text(const std::string& text) override;
    void set_start_callback(std::function<void()> cb) override { start_cb_ = std::move(cb); }
    void post_task(std::function<void()> task) override;
    
    void set_menu_callback(std::function<void(MenuAction)> cb) override { menu_cb_ = std::move(cb); }
    void set_restore_callback(std::function<void()> cb) override { m_restore_cb_ = std::move(cb); }
    void set_fps_limited(bool limited) override { fps_limited_ = limited; }
    void set_resolution_limited(bool limited) override { resolution_limited_ = limited; }
    void set_compatibility_mode(bool enabled) override { compatibility_mode_ = enabled; }
    void set_lowest_brightness(bool enabled) override { lowest_brightness_ = enabled; }
    void set_capture_send_to_phone(bool enabled) override { capture_send_to_phone_ = enabled; }
    void set_recording(bool recording) override { recording_ = recording; if (hwnd_) InvalidateRect(hwnd_, &rect_bubble_, FALSE); }
    void set_os_clipboard_update_callback(std::function<void(const std::string&)> cb) override { m_os_clipboard_cb_ = std::move(cb); }
    void set_pc_clipboard(const std::string& text) override;

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT msg, WPARAM wparam, LPARAM lparam);
    
    LRESULT handle_nchittest(POINT pt);
    void handle_sizing(WPARAM wparam, LPARAM lparam);
    void handle_paint();
    
    // GDI+ drawing helpers
    void draw_setup_screen(Gdiplus::Graphics& g);
    void draw_scanning_screen(Gdiplus::Graphics& g);
    void draw_connected_screen(Gdiplus::Graphics& g);
    void draw_streaming_screen(Gdiplus::Graphics& g);
    
    void toggle_max_height();
    void recalc_layout();
    void update_region();
    void notify_video_viewport();
    void send_pointer_event(PointerAction action, int x, int y);
    int hit_test_button(POINT client_pt);
    bool is_start_button_hit(POINT client_pt);
    void show_context_menu(POINT pt);
    void show_capture_menu(POINT pt);

    HWND hwnd_{nullptr};
    HWND hwnd_child_{nullptr};
    SDL_Window* m_sdl_window{nullptr};
    SDL_Renderer* m_sdl_renderer{nullptr};
    HFONT icon_font_{nullptr};
    int width_;
    int height_;
    std::string title_;
    
    double aspect_ratio_{0.0};
    bool is_landscape_{false};
    bool is_max_height_{false};
    RECT restore_bounds_{0};
    int hovered_button_{-1}; // 0=drag, 1=min, 2=max, 3=close, 4=capture
    bool start_button_hovered_{false};

    // Layout rectangles
    RECT rect_phone_{0};
    RECT rect_bubble_{0};
    RECT rect_capture_{0}, rect_drag_{0}, rect_min_{0}, rect_max_{0}, rect_close_{0};
    RECT rect_start_btn_{0};
    
    // State
    AppState app_state_{AppState::SETUP};
    bool visible_{true};
    std::string status_text_;
    int scan_animation_frame_{0};
    
    // Callbacks
    std::function<void(SDL_Renderer*, int, int, int, int)> m_render_cb_;
    std::function<void(int, int, int, int)> m_viewport_cb_;
    std::function<void(PointerAction, int, int, int, int)> m_pointer_cb_;
    std::function<void(int, int)> m_key_cb_;
    std::function<void(const std::string&)> m_text_cb_;
    std::function<void(int, int, int, int, float, float)> m_scroll_cb_;
    std::function<void()> start_cb_;
    std::function<void(MenuAction)> menu_cb_;
    std::function<void()> m_restore_cb_;
    bool fps_limited_{false};
    bool resolution_limited_{false};
    bool compatibility_mode_{false};
    bool lowest_brightness_{true};
    bool capture_send_to_phone_{false};
    bool recording_{false};
    std::function<void(const std::string&)> m_os_clipboard_cb_;
    std::string last_clipboard_text_;
    std::mutex clipboard_mutex_;
};

} // namespace pm::window
#endif
