#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <functional>
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
    void process_messages() override;
    
    void set_aspect_ratio(double ratio) override;
    void set_orientation(bool landscape) override;
    
    void* get_native_handle() override { return hwnd_; }
    void set_render_callback(std::function<void(SDL_Renderer*, int, int, int, int)> cb) override { m_render_cb_ = std::move(cb); }
    void set_video_viewport_callback(std::function<void(int, int, int, int)> cb) override;
    void set_pointer_callback(std::function<void(PointerAction, int, int, int, int)> cb) override { m_pointer_cb_ = std::move(cb); }
    void set_app_state(AppState state) override;
    void set_status_text(const std::string& text) override;
    void set_start_callback(std::function<void()> cb) override { start_cb_ = std::move(cb); }
    void post_task(std::function<void()> task) override;
    
    void set_menu_callback(std::function<void(MenuAction)> cb) override { menu_cb_ = std::move(cb); }
    void set_fps_limited(bool limited) override { fps_limited_ = limited; }
    void set_resolution_limited(bool limited) override { resolution_limited_ = limited; }

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
    int hovered_button_{-1}; // 0=drag, 1=min, 2=max, 3=close
    bool start_button_hovered_{false};

    // Layout rectangles
    RECT rect_phone_{0};
    RECT rect_bubble_{0};
    RECT rect_drag_{0}, rect_min_{0}, rect_max_{0}, rect_close_{0};
    RECT rect_start_btn_{0};
    
    // State
    AppState app_state_{AppState::SETUP};
    std::string status_text_;
    int scan_animation_frame_{0};
    
    // Callbacks
    std::function<void(SDL_Renderer*, int, int, int, int)> m_render_cb_;
    std::function<void(int, int, int, int)> m_viewport_cb_;
    std::function<void(PointerAction, int, int, int, int)> m_pointer_cb_;
    std::function<void()> start_cb_;
    std::function<void(MenuAction)> menu_cb_;
    bool fps_limited_{false};
    bool resolution_limited_{false};
};

} // namespace pm::window
#endif
