#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <functional>
#include <mutex>
#include <chrono>
#include <vector>
#include <unordered_set>
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
    void minimize() override;
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
    void set_raw_key_callback(std::function<bool(int, uint32_t, bool)> cb) override { m_raw_key_cb_ = std::move(cb); }
    void set_focus_lost_callback(std::function<void()> cb) override { m_focus_lost_cb_ = std::move(cb); }
    void set_scroll_callback(std::function<void(int, int, int, int, float, float)> cb) override { m_scroll_cb_ = std::move(cb); }
    void set_app_state(AppState state) override;
    void set_status_text(const std::string& text) override;
    void set_start_callback(std::function<void()> cb) override { start_cb_ = std::move(cb); }
    void post_task(std::function<void()> task) override;
    
    void set_menu_callback(std::function<void(MenuAction)> cb) override { menu_cb_ = std::move(cb); }
    void set_restore_callback(std::function<void()> cb) override { m_restore_cb_ = std::move(cb); }
    void set_minimize_callback(std::function<void(bool)> cb) override { m_minimize_cb_ = std::move(cb); }
    void set_quality_preset(int preset) override { quality_preset_ = preset; }
    void set_compatibility_mode(bool enabled) override { compatibility_mode_ = enabled; }
    void set_lowest_brightness(bool enabled) override { lowest_brightness_ = enabled; }
    void set_screen_off(bool enabled) override { screen_off_ = enabled; }
    void set_capture_send_to_phone(bool enabled) override { capture_send_to_phone_ = enabled; }
    void set_audio_enabled(bool enabled) override { audio_enabled_ = enabled; }
    void set_auto_rotate_enabled(bool enabled) override { auto_rotate_enabled_ = enabled; }
    void set_uhid_keyboard(bool enabled) override { uhid_keyboard_ = enabled; hid_held_keys_.clear(); }
    void set_auto_pause_minimized(bool enabled) override { auto_pause_minimized_ = enabled; }
    void set_virtual_display(bool enabled) override { virtual_display_ = enabled; }
    void set_recording(bool recording) override;
    void trigger_screenshot_flash() override;
    void set_file_drop_callback(std::function<void(const std::vector<std::string>&)> cb) override { m_file_drop_cb_ = std::move(cb); }
    void set_transfer_status(TransferState state, float progress, const std::string& label) override;
    void set_os_clipboard_update_callback(std::function<void(const std::string&)> cb) override { m_os_clipboard_cb_ = std::move(cb); }
    void set_pc_clipboard(const std::string& text) override;
    void set_focus_callback(std::function<void()> cb) override { m_focus_cb_ = std::move(cb); }

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
    void update_animation_timer();
    void ensure_fonts();
    void release_fonts();
    void notify_video_viewport();
    void send_pointer_event(PointerAction action, int x, int y);
    int hit_test_button(POINT client_pt);
    bool is_start_button_hit(POINT client_pt);
    void show_context_menu(POINT pt);
    void build_settings_menu_items();
    void show_capture_menu(POINT pt);
    void handle_dropped_files(WPARAM wparam);
    void draw_transfer_bubble(Gdiplus::Graphics& g);

    // Hands the physical key position to the raw-key callback. Returns true when
    // the key was dealt with and must not walk down the Android-keycode path too.
    bool send_raw_key(bool pressed, LPARAM lparam);

    // Holes the fake keyboard actually took, as scancode plus the extended bit.
    // Only needed for auto-repeat: a repeat carves no new stone, so nobody asks
    // the fake keyboard again — and without this shelf we would not know whether
    // to swallow the repeat or let it walk the Android-keycode path.
    std::unordered_set<uint32_t> hid_held_keys_;

    // Virtual keys whose PRESS was eaten by a cave shortcut (Strg+U, Strg+L).
    // Their release has to be eaten as well, or the fake keyboard tells the phone
    // to let go of a key the phone never saw being pressed.
    std::unordered_set<UINT> swallowed_shortcuts_;

    HWND hwnd_{nullptr};
    HWND hwnd_child_{nullptr};
    SDL_Window* m_sdl_window{nullptr};
    SDL_Renderer* m_sdl_renderer{nullptr};
    HFONT icon_font_{nullptr};
    int width_;
    int height_;
    std::string title_;

    // Ugg! Cave man used to carve fresh letter-stones on every single paint, sixty
    // times a heartbeat. Now he carves them once and keeps them on the shelf.
    // Raw pointers, because they MUST die before GdiplusShutdown (release_fonts).
    Gdiplus::FontFamily* font_family_ui_{nullptr};
    Gdiplus::FontFamily* font_family_icons_{nullptr};
    Gdiplus::Font* font_icon_{nullptr};
    Gdiplus::Font* font_title_{nullptr};
    Gdiplus::Font* font_body_{nullptr};
    Gdiplus::Font* font_button_{nullptr};
    Gdiplus::Font* font_status_{nullptr};
    Gdiplus::Font* font_live_{nullptr};
    Gdiplus::Font* font_timer_{nullptr};
    bool animation_timer_active_{false};
    
    double aspect_ratio_{0.0};
    bool is_landscape_{false};
    bool is_max_height_{false};
    RECT restore_bounds_{0};
    int hovered_button_{-1}; // 0=drag, 1=min, 2=max, 3=close, 4=capture
    bool start_button_hovered_{false};

    // Layout rectangles
    RECT rect_phone_{0};
    RECT rect_bubble_{0};
    RECT rect_recording_bubble_{0};
    RECT rect_recording_stop_{0};
    RECT rect_screenshot_bubble_{0};
    RECT rect_transfer_bubble_{0};
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
    std::function<bool(int, uint32_t, bool)> m_raw_key_cb_;
    std::function<void()> m_focus_lost_cb_;
    std::function<void(int, int, int, int, float, float)> m_scroll_cb_;
    std::function<void()> start_cb_;
    std::function<void(MenuAction)> menu_cb_;
    std::function<void()> m_restore_cb_;
    std::function<void(bool)> m_minimize_cb_;

    // Ugg! WM_SIZE shouts SIZE_RESTORED for every drag of the window edge too. Only
    // a real fold-down or fold-up is worth telling anybody about.
    bool minimized_{false};
    int quality_preset_{1};        // 0 = Akku, 1 = Ausgewogen, 2 = Maximal
    bool quality_expanded_{false}; // Cave man folded the quality stones open?
    bool compatibility_mode_{false};
    bool lowest_brightness_{true};
    bool screen_off_{false};
    bool capture_send_to_phone_{false};
    bool audio_enabled_{true};
    bool auto_rotate_enabled_{true};
    bool uhid_keyboard_{false};
    bool auto_pause_minimized_{true};
    bool virtual_display_{false};
    bool recording_{false};
    std::chrono::steady_clock::time_point recording_start_time_;
    bool screenshot_flash_{false};
    std::chrono::steady_clock::time_point screenshot_flash_start_;
    int screenshot_flash_frames_{0};
    std::function<void(const std::vector<std::string>&)> m_file_drop_cb_;
    TransferState transfer_state_{TransferState::IDLE};
    float transfer_progress_{0.0f};
    std::string transfer_label_;
    std::chrono::steady_clock::time_point transfer_settled_at_;
    std::function<void()> m_focus_cb_;
    std::function<void(const std::string&)> m_os_clipboard_cb_;
    std::string last_clipboard_text_;
    std::mutex clipboard_mutex_;
};

} // namespace pm::window
#endif
