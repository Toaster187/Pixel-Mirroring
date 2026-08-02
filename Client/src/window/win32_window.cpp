#ifdef _WIN32
#include "win32_window.h"
#include "../resource.h"
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <SDL2/SDL.h>
#include <dwmapi.h>

#pragma comment(lib, "gdiplus.lib")

namespace pm::window {

namespace {
    const int PHONE_CORNER_RADIUS = 24;
    const int BUBBLE_CORNER_RADIUS = 18;
    const int BUBBLE_W = 193;
    const int BUBBLE_H = 36;
    const int BUBBLE_GAP = 6;
    const int MIN_PHONE_W = 140;
    const int MIN_PHONE_H = 200;
    // Transfer bubble shows the file name next to the ring; when the window is too
    // narrow for that it shrinks down to just the ring.
    const int TRANSFER_BUBBLE_W = 132;
    const int TRANSFER_RING_ONLY_W = 40;
    const UINT WM_VIDEO_RENDER = WM_APP + 2;
    // Cave man does not open talking-stone while the drag hand still holds the rock.
    // Drop is handed back to ourselves first, so Explorer is free again.
    const UINT WM_FILES_DROPPED = WM_APP + 4;
    const wchar_t* ICON_DRAG = L"\uE700";
    const wchar_t* ICON_MINIMIZE = L"\uE921";
    const wchar_t* ICON_MAXIMIZE = L"\uE922";
    const wchar_t* ICON_RESTORE = L"\uE923";
    const wchar_t* ICON_CLOSE = L"\uE8BB";
    const wchar_t* ICON_CAPTURE = L"\uE722";
    const wchar_t* ICON_CHECK = L"\uE73E";

    void AddRoundedRect(Gdiplus::GraphicsPath& path, Gdiplus::RectF r, float rad) {
        float d = rad * 2;
        if (d > r.Width) d = r.Width;
        if (d > r.Height) d = r.Height;
        path.AddArc(r.X, r.Y, d, d, 180, 90);
        path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
        path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
        path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
        path.CloseFigure();
    }

    std::string wchar_to_utf8(wchar_t wch) {
        // convert character. cave man talk utf8.
        wchar_t wstr[2] = { wch, 0 };
        char buf[8] = { 0 };
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr, 1, buf, sizeof(buf) - 1, nullptr, nullptr);
        if (len > 0) {
            return std::string(buf, len);
        }
        return "";
    }

    std::string wstring_to_utf8(const std::wstring& wstr) {
        if (wstr.empty()) return "";
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        if (len <= 0) return "";
        std::string str(len, 0);
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &str[0], len, nullptr, nullptr);
        return str;
    }

    std::wstring utf8_to_wstring(const std::string& str) {
        if (str.empty()) return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
        if (len <= 0) return L"";
        std::wstring wstr(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &wstr[0], len);
        return wstr;
    }

    // Ugg! Phone has three big cave buttons at the bottom. PC keyboard has none, so
    // Alt + letter stands in for them. Alt (not Ctrl) because Ctrl+C/Ctrl+V already
    // carry the clipboard back and forth, and Ctrl+U/Ctrl+L unlock and lock.
    int alt_shortcut_to_android_keycode(WPARAM wparam) {
        switch (wparam) {
        case 'B': return 4;   // AKEYCODE_BACK
        case 'H': return 3;   // AKEYCODE_HOME
        case 'S': return 187; // AKEYCODE_APP_SWITCH (Übersicht / Task-Manager)
        default:  return 0;
        }
    }

    int vk_to_android_keycode(WPARAM wparam) {
        // map VK to android key. cave man press buttons.
        switch (wparam) {
        case VK_RETURN:   return 66;  // AKEYCODE_ENTER
        case VK_BACK:     return 67;  // AKEYCODE_DEL
        case VK_ESCAPE:   return 4;   // AKEYCODE_BACK
        case VK_LEFT:     return 21;  // AKEYCODE_DPAD_LEFT
        case VK_RIGHT:    return 22;  // AKEYCODE_DPAD_RIGHT
        case VK_UP:       return 19;  // AKEYCODE_DPAD_UP
        case VK_DOWN:     return 20;  // AKEYCODE_DPAD_DOWN
        case VK_TAB:      return 61;  // AKEYCODE_TAB
        case VK_DELETE:   return 112; // AKEYCODE_FORWARD_DEL
        case VK_HOME:     return 122; // AKEYCODE_MOVE_HOME
        case VK_END:      return 123; // AKEYCODE_MOVE_END
        case VK_PRIOR:    return 92;  // AKEYCODE_PAGE_UP
        case VK_NEXT:     return 93;  // AKEYCODE_PAGE_DOWN
        case VK_VOLUME_UP:   return 24;  // AKEYCODE_VOLUME_UP
        case VK_VOLUME_DOWN: return 25;  // AKEYCODE_VOLUME_DOWN
        case VK_VOLUME_MUTE: return 164; // AKEYCODE_VOLUME_MUTE
        default:          return 0;   // Printable or unmapped
        }
    }
}

std::unique_ptr<IWindow> create_window(int w, int h, const std::string& t) {
    return std::make_unique<Win32Window>(w, h, t);
}

Win32Window::Win32Window(int w, int h, const std::string& t) : width_(w), height_(h), title_(t) {}

Win32Window::~Win32Window() {
    release_fonts();
    if (m_sdl_renderer) SDL_DestroyRenderer(m_sdl_renderer);
    if (m_sdl_window) SDL_DestroyWindow(m_sdl_window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (icon_font_) DeleteObject(icon_font_);
    // Cave man destroy child window before parent
    if (hwnd_child_) DestroyWindow(hwnd_child_);
    if (hwnd_) {
        RemoveClipboardFormatListener(hwnd_);
        DestroyWindow(hwnd_);
    }
}

void Win32Window::ensure_fonts() {
    if (font_family_ui_) return;

    font_family_ui_ = new Gdiplus::FontFamily(L"Segoe UI");
    font_family_icons_ = new Gdiplus::FontFamily(L"Segoe MDL2 Assets");

    font_icon_ = new Gdiplus::Font(font_family_icons_, 10, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    font_title_ = new Gdiplus::Font(font_family_ui_, 16, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    font_body_ = new Gdiplus::Font(font_family_ui_, 9, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    font_button_ = new Gdiplus::Font(font_family_ui_, 11, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    font_status_ = new Gdiplus::Font(font_family_ui_, 12, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    font_live_ = new Gdiplus::Font(font_family_ui_, 7, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    font_timer_ = new Gdiplus::Font(font_family_ui_, 9, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
}

void Win32Window::release_fonts() {
    delete font_icon_;   font_icon_ = nullptr;
    delete font_title_;  font_title_ = nullptr;
    delete font_body_;   font_body_ = nullptr;
    delete font_button_; font_button_ = nullptr;
    delete font_status_; font_status_ = nullptr;
    delete font_live_;   font_live_ = nullptr;
    delete font_timer_;  font_timer_ = nullptr;
    delete font_family_ui_;    font_family_ui_ = nullptr;
    delete font_family_icons_; font_family_icons_ = nullptr;
}

// Ugg! Spinner only turns while cave man waits. When the phone picture is live, or
// the window sleeps in the tray, a 60-times-a-heartbeat repaint burns fire for nothing.
void Win32Window::update_animation_timer() {
    if (!hwnd_) return;

    // Timers belong to the thread that owns the window. Some callers live on the
    // connection thread, so hand the job over instead of poking from outside.
    if (GetCurrentThreadId() != GetWindowThreadProcessId(hwnd_, nullptr)) {
        post_task([this]() { update_animation_timer(); });
        return;
    }

    const bool needs_animation = visible_ && app_state_ != AppState::STREAMING;
    if (needs_animation == animation_timer_active_) return;

    if (needs_animation) {
        SetTimer(hwnd_, 1, 16, nullptr);
    } else {
        KillTimer(hwnd_, 1);
    }
    animation_timer_active_ = needs_animation;
}

void Win32Window::set_app_state(AppState s) {
    app_state_ = s;
    // Cave man toggle child window visibility based on streaming state
    if (hwnd_child_) {
        ShowWindow(hwnd_child_, s == AppState::STREAMING ? SW_SHOW : SW_HIDE);
    }
    update_animation_timer();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}
void Win32Window::set_status_text(const std::string& t) { status_text_ = t; if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

bool Win32Window::create() {
    HINSTANCE hi = GetModuleHandle(nullptr);
    // Ugg! Class must be Unicode. An ANSI class hands us cp1252 bytes in WM_CHAR,
    // so anything past the old Latin alphabet (€, emoji) arrives as broken rocks.
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
    wc.lpfnWndProc = Win32Window::window_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    // Ugg! Our own rock painting for taskbar + alt-tab, big and small size.
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    wc.lpszClassName = L"PixelMirroringWindowClass";
    RegisterClassExW(&wc);

    DWORD style = WS_POPUP | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    int th = height_ + BUBBLE_H + BUBBLE_GAP;
    const std::wstring wide_title = utf8_to_wstring(title_);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, wide_title.c_str(), style,
        CW_USEDEFAULT, CW_USEDEFAULT, width_, th, nullptr, nullptr, hi, this);
    if (!hwnd_) return false;

    AddClipboardFormatListener(hwnd_);

    // Cave man opens window for rocks thrown in from the file cave. The SDL child is
    // WS_DISABLED, so WindowFromPoint walks past it and every drop lands right here.
    DragAcceptFiles(hwnd_, TRUE);
    // Ugg! If our fire burns higher than Explorer's, Windows eats the drop messages
    // in silence. Let them through explicitly. 0x0049 = WM_COPYGLOBALDATA.
    ChangeWindowMessageFilterEx(hwnd_, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd_, 0x0049, MSGFLT_ALLOW, nullptr);

    // MEOW. REMOVE WINDOW 11 BORDER AND CORNER ARTIFACTS.
    COLORREF border_color = 0xFFFFFFFE; // DWM_COLOR_DONT_DRAW
    DwmSetWindowAttribute(hwnd_, 34, &border_color, sizeof(border_color)); // 34 = DWMWA_BORDER_COLOR
    DWORD corner_preference = 1; // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(hwnd_, 33, &corner_preference, sizeof(corner_preference)); // 33 = DWMWA_WINDOW_CORNER_PREFERENCE

    // Cave man register child window class
    WNDCLASSEXW child_wc = {sizeof(child_wc)};
    child_wc.style = CS_HREDRAW | CS_VREDRAW;
    child_wc.lpfnWndProc = DefWindowProcW;
    child_wc.hInstance = hi;
    child_wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    child_wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    child_wc.lpszClassName = L"PixelMirroringChildWindowClass";
    RegisterClassExW(&child_wc);

    // Cave man create child window for SDL video, ignore input so it fall through to parent
    hwnd_child_ = CreateWindowExW(0, child_wc.lpszClassName, nullptr,
        WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS,
        0, 0, 0, 0,
        hwnd_, nullptr, hi, nullptr);

    icon_font_ = CreateFontW(14, 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

    SDL_Init(SDL_INIT_VIDEO);
    // Cave man bind SDL to child window, no draw over main window buttons!
    m_sdl_window = SDL_CreateWindowFrom(hwnd_child_);
    if (m_sdl_window) {
        m_sdl_renderer = SDL_CreateRenderer(m_sdl_window, -1, SDL_RENDERER_ACCELERATED);
    }

    recalc_layout();
    update_region();
    update_animation_timer();
    return true;
}

void Win32Window::show() {
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    visible_ = true;
    update_animation_timer();
    // Cave man force redraw of SDL frame when window shown
    PostMessage(hwnd_, WM_VIDEO_RENDER, 0, 0);
}
void Win32Window::hide() {
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
    update_animation_timer();
}
bool Win32Window::is_visible() const { return visible_; }

void Win32Window::process_messages() {
    MSG msg;
    // GetMessage returns -1 on error. Treating that as "true" spins forever.
    //
    // Ugg! The W ending is not decoration. TranslateMessage builds WM_CHAR in the
    // flavour of whoever FETCHED the message, not of the window class. With the
    // plain (= ...A) pair every typed letter takes a detour through cp1252, and
    // everything cp1252 cannot hold comes out as "?". Umlauts happen to survive
    // that detour by luck — cp1252 maps them onto the same numbers as Unicode —
    // but nothing else does. Fetch wide, stay wide.
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void Win32Window::set_aspect_ratio(double r) {
    aspect_ratio_ = r;
    if (!hwnd_ || r <= 0.0) return;

    RECT cr; GetClientRect(hwnd_, &cr);
    RECT wr; GetWindowRect(hwnd_, &wr);
    int phone_h = cr.bottom - cr.top - BUBBLE_H - BUBBLE_GAP;
    if (phone_h <= 0) return;

    int target_client_w = (std::max)(MIN_PHONE_W, (int)(phone_h * r + 0.5));
    int frame_extra_w = (wr.right - wr.left) - (cr.right - cr.left);
    int target_window_w = target_client_w + frame_extra_w;
    int window_h = wr.bottom - wr.top;
    int center_x = (wr.left + wr.right) / 2;

    SetWindowPos(hwnd_, nullptr, center_x - target_window_w / 2, wr.top,
        target_window_w, window_h, SWP_NOZORDER | SWP_NOACTIVATE);
}
void Win32Window::set_orientation(bool l) { is_landscape_ = l; is_max_height_ = false; }

void Win32Window::set_video_viewport_callback(std::function<void(int, int, int, int)> cb) {
    m_viewport_cb_ = std::move(cb);
    notify_video_viewport();
}

void Win32Window::recalc_layout() {
    if (!hwnd_) return;
    RECT cr; GetClientRect(hwnd_, &cr);
    int w = cr.right, h = cr.bottom;
    if (w <= 0 || h <= 0) return;

    int pt = BUBBLE_H + BUBBLE_GAP;
    rect_phone_ = {0, pt, w, h};

    int bw = (std::min)(BUBBLE_W, w);
    int bx = (std::max)(0, w - bw);
    rect_bubble_ = {bx, 0, bx + bw, BUBBLE_H};

    int btnw = bw / 5, rem = bw - btnw * 5;
    rect_capture_ = {rect_bubble_.left, 0, rect_bubble_.left + btnw + rem, BUBBLE_H};
    rect_drag_  = {rect_capture_.right, 0, rect_capture_.right + btnw, BUBBLE_H};
    rect_min_   = {rect_drag_.right, 0, rect_drag_.right + btnw, BUBBLE_H};
    rect_max_   = {rect_min_.right, 0, rect_min_.right + btnw, BUBBLE_H};
    rect_close_ = {rect_max_.right, 0, rect_max_.right + btnw, BUBBLE_H};

    if (recording_) {
        int r_bw = 100;
        int r_bx = rect_bubble_.left - BUBBLE_GAP - r_bw;
        if (r_bx < 0) r_bx = 0;
        rect_recording_bubble_ = {r_bx, 0, r_bx + r_bw, BUBBLE_H};
        rect_recording_stop_ = {rect_recording_bubble_.right - BUBBLE_H, 0, rect_recording_bubble_.right, BUBBLE_H};
    } else {
        rect_recording_bubble_ = {0,0,0,0};
        rect_recording_stop_ = {0,0,0,0};
    }

    if (screenshot_flash_) {
        int s_bw = 40;
        int s_bx = (recording_ ? rect_recording_bubble_.left : rect_bubble_.left) - BUBBLE_GAP - s_bw;
        if (s_bx < 0) s_bx = 0;
        rect_screenshot_bubble_ = {s_bx, 0, s_bx + s_bw, BUBBLE_H};
    } else {
        rect_screenshot_bubble_ = {0,0,0,0};
    }

    // Transfer bubble always sits at the far left end of the row — second bubble on a
    // quiet day, third one while the phone is being filmed or photographed.
    if (transfer_state_ != TransferState::IDLE) {
        int t_right = rect_bubble_.left;
        if (screenshot_flash_)  t_right = rect_screenshot_bubble_.left;
        else if (recording_)    t_right = rect_recording_bubble_.left;

        int t_bw = (t_right - BUBBLE_GAP >= TRANSFER_BUBBLE_W) ? TRANSFER_BUBBLE_W : TRANSFER_RING_ONLY_W;
        int t_bx = t_right - BUBBLE_GAP - t_bw;
        if (t_bx < 0) t_bx = 0;
        rect_transfer_bubble_ = {t_bx, 0, t_bx + t_bw, BUBBLE_H};
    } else {
        rect_transfer_bubble_ = {0,0,0,0};
    }

    // Start button in phone area
    int sbw = 180, sbh = 40;
    int px = (rect_phone_.left + rect_phone_.right) / 2;
    int py = rect_phone_.top + (rect_phone_.bottom - rect_phone_.top) * 2 / 3;
    rect_start_btn_ = {px - sbw/2, py - sbh/2, px + sbw/2, py + sbh/2};

    // Cave man position child window to phone rect
    if (hwnd_child_) {
        int child_w = rect_phone_.right - rect_phone_.left;
        int child_h = rect_phone_.bottom - rect_phone_.top;
        SetWindowPos(hwnd_child_, nullptr, rect_phone_.left, rect_phone_.top,
            child_w, child_h,
            SWP_NOZORDER | SWP_NOACTIVATE);
        // Cave man tell SDL window has new size, so backbuffer resize too!
        if (m_sdl_window) {
            SDL_SetWindowSize(m_sdl_window, child_w, child_h);
        }
    }

    notify_video_viewport();
}

void Win32Window::update_region() {
    if (!hwnd_) return;
    RECT cr; GetClientRect(hwnd_, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;

    HRGN br = CreateRoundRectRgn(rect_bubble_.left, rect_bubble_.top,
        rect_bubble_.right+1, rect_bubble_.bottom+1, BUBBLE_CORNER_RADIUS*2, BUBBLE_CORNER_RADIUS*2);
    HRGN pr = CreateRoundRectRgn(rect_phone_.left, rect_phone_.top,
        rect_phone_.right+1, rect_phone_.bottom+1, PHONE_CORNER_RADIUS*2, PHONE_CORNER_RADIUS*2);
    // Ugg! There was a third region stone here that nobody used and nobody threw
    // away. Every resize left one behind until Windows ran out of stones.
    HRGN rgn = CreateRectRgn(0,0,0,0);
    CombineRgn(rgn, br, pr, RGN_OR);
    
    if (recording_) {
        HRGN br2 = CreateRoundRectRgn(rect_recording_bubble_.left, rect_recording_bubble_.top,
            rect_recording_bubble_.right+1, rect_recording_bubble_.bottom+1, BUBBLE_CORNER_RADIUS*2, BUBBLE_CORNER_RADIUS*2);
        CombineRgn(rgn, rgn, br2, RGN_OR);
        DeleteObject(br2);
    }
    if (screenshot_flash_) {
        HRGN br3 = CreateRoundRectRgn(rect_screenshot_bubble_.left, rect_screenshot_bubble_.top,
            rect_screenshot_bubble_.right+1, rect_screenshot_bubble_.bottom+1, BUBBLE_CORNER_RADIUS*2, BUBBLE_CORNER_RADIUS*2);
        CombineRgn(rgn, rgn, br3, RGN_OR);
        DeleteObject(br3);
    }
    if (transfer_state_ != TransferState::IDLE) {
        HRGN br4 = CreateRoundRectRgn(rect_transfer_bubble_.left, rect_transfer_bubble_.top,
            rect_transfer_bubble_.right+1, rect_transfer_bubble_.bottom+1, BUBBLE_CORNER_RADIUS*2, BUBBLE_CORNER_RADIUS*2);
        CombineRgn(rgn, rgn, br4, RGN_OR);
        DeleteObject(br4);
    }

    SetWindowRgn(hwnd_, rgn, TRUE);
    DeleteObject(br); DeleteObject(pr);
}

void Win32Window::notify_video_viewport() {
    if (!m_viewport_cb_) return;

    int w = rect_phone_.right - rect_phone_.left;
    int h = rect_phone_.bottom - rect_phone_.top;
    if (w <= 0 || h <= 0) return;

    m_viewport_cb_(rect_phone_.left, rect_phone_.top, w, h);
}

void Win32Window::send_pointer_event(PointerAction action, int x, int y) {
    if (!m_pointer_cb_ || app_state_ != AppState::STREAMING) return;

    int w = rect_phone_.right - rect_phone_.left;
    int h = rect_phone_.bottom - rect_phone_.top;
    if (w <= 0 || h <= 0) return;

    x = (std::max)(0, (std::min)(x, w - 1));
    y = (std::max)(0, (std::min)(y, h - 1));
    m_pointer_cb_(action, x, y, w, h);
}

bool Win32Window::send_raw_key(bool pressed, LPARAM lparam) {
    if (!m_raw_key_cb_) return false;

    // Ugg! The number Windows shouts first is the LETTER its own layout painted on
    // the key — exactly what the phone must not inherit. The hardware number of
    // the hole sits in the upper stones of lparam, with one extra bit for the keys
    // the small old keyboards never had (arrows, right Alt, keypad Enter, ...).
    const uint32_t scancode = static_cast<uint32_t>((lparam >> 16) & 0xFF);
    if (scancode == 0) return false;

    const bool extended = (lparam & (1 << 24)) != 0;
    const uint32_t hole = scancode | (extended ? 0x100u : 0u);

    // A held key repeats by itself on the phone, the same way a cabled keyboard
    // works. Cave man does not carve the same stone sixty times a heartbeat — but
    // he still swallows the message, or the key would be typed twice. Only for the
    // holes the fake keyboard really took: one it refused walks the old road on
    // every repeat too, exactly like it did on the first press.
    if (pressed && (lparam & (1 << 30)) != 0) {
        return hid_held_keys_.count(hole) != 0;
    }

    // Ugg! "No" means no. A boot keyboard has no volume and no media buttons, and
    // a key the fake keyboard hands back must NOT be eaten here — the Android
    // keycode path below is the whole reason it says no in the first place.
    const bool taken = m_raw_key_cb_(pressed ? 0 : 1, scancode, extended);
    if (pressed && taken) {
        hid_held_keys_.insert(hole);
    } else {
        hid_held_keys_.erase(hole);
    }
    return taken;
}

int Win32Window::hit_test_button(POINT pt) {
    if (recording_ && PtInRect(&rect_recording_stop_, pt)) return 5;
    if (PtInRect(&rect_capture_, pt)) return 4;
    if (PtInRect(&rect_close_, pt)) return 3;
    if (PtInRect(&rect_max_, pt))   return 2;
    if (PtInRect(&rect_min_, pt))   return 1;
    if (PtInRect(&rect_drag_, pt))  return 0;
    return -1;
}

bool Win32Window::is_start_button_hit(POINT pt) {
    return app_state_ == AppState::SETUP && PtInRect(&rect_start_btn_, pt);
}

void Win32Window::toggle_max_height() {
    if (is_max_height_) {
        is_max_height_ = false;
        SetWindowPos(hwnd_, nullptr, restore_bounds_.left, restore_bounds_.top,
            restore_bounds_.right - restore_bounds_.left,
            restore_bounds_.bottom - restore_bounds_.top, SWP_NOZORDER);
    } else {
        GetWindowRect(hwnd_, &restore_bounds_);
        is_max_height_ = true;
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(mi)}; GetMonitorInfo(mon, &mi);
        RECT wa = mi.rcWork;
        if (is_landscape_) {
            SetWindowPos(hwnd_, nullptr, wa.left, wa.top,
                wa.right-wa.left, wa.bottom-wa.top, SWP_NOZORDER);
        } else {
            int th = wa.bottom - wa.top;
            int ph = th - BUBBLE_H - BUBBLE_GAP;
            double r = aspect_ratio_ > 0 ? aspect_ratio_ : (9.0/16.0);
            int pw = (int)(ph * r);
            int x = wa.left + (wa.right - wa.left - pw) / 2;
            SetWindowPos(hwnd_, nullptr, x, wa.top, pw, th, SWP_NOZORDER);
        }
    }
}

// --- GDI+ Paint ---
void Win32Window::handle_paint() {
    ensure_fonts();

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd_, &ps);
    RECT cr; GetClientRect(hwnd_, &cr);
    int w = cr.right, h = cr.bottom;
    if (w <= 0 || h <= 0) { EndPaint(hwnd_, &ps); return; }

    // Double buffer
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    // Fill black background
    HBRUSH bg = CreateSolidBrush(RGB(0,0,0));
    FillRect(mem, &cr, bg); DeleteObject(bg);

    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        // Phone background
        Gdiplus::RectF pr((float)rect_phone_.left, (float)rect_phone_.top,
            (float)(rect_phone_.right - rect_phone_.left) - 1.0f,
            (float)(rect_phone_.bottom - rect_phone_.top) - 1.0f);
        {
            Gdiplus::GraphicsPath pp;
            AddRoundedRect(pp, pr, (float)PHONE_CORNER_RADIUS);
            Gdiplus::SolidBrush pb(Gdiplus::Color(255, 22, 22, 22));
            g.FillPath(&pb, &pp);
            Gdiplus::Pen pen(Gdiplus::Color(255, 65, 65, 65), 1.5f);
            g.DrawPath(&pen, &pp);
        }

        // Bubble background
        Gdiplus::RectF bbr((float)rect_bubble_.left, (float)rect_bubble_.top,
            (float)(rect_bubble_.right - rect_bubble_.left) - 1.0f,
            (float)(rect_bubble_.bottom - rect_bubble_.top) - 1.0f);
        {
            Gdiplus::GraphicsPath bp;
            AddRoundedRect(bp, bbr, (float)BUBBLE_CORNER_RADIUS);
            Gdiplus::SolidBrush bb(Gdiplus::Color(255, 50, 50, 50));
            g.FillPath(&bb, &bp);
            Gdiplus::Pen pen(Gdiplus::Color(255, 75, 75, 75), 1.0f);
            g.DrawPath(&pen, &bp);
        }

        // Button hovers
        if (hovered_button_ >= 0 && hovered_button_ <= 4) {
            RECT hrs[] = {rect_drag_, rect_min_, rect_max_, rect_close_, rect_capture_};
            RECT hr = hrs[hovered_button_];
            Gdiplus::Color hc = (hovered_button_ == 3)
                ? Gdiplus::Color(255, 196, 43, 28) : Gdiplus::Color(255, 75, 75, 75);
            Gdiplus::RectF hrf((float)hr.left, (float)hr.top,
                (float)(hr.right-hr.left) - 1.0f, (float)(hr.bottom-hr.top) - 1.0f);

            // Clip to bubble shape
            Gdiplus::GraphicsPath clip;
            AddRoundedRect(clip, bbr, (float)BUBBLE_CORNER_RADIUS);
            Gdiplus::Region clipRgn(&clip);
            g.SetClip(&clipRgn);
            Gdiplus::SolidBrush hb(hc);
            g.FillRectangle(&hb, hrf);
            g.ResetClip();
        }

        // Button icons — fonts come from the shelf, not freshly carved
        Gdiplus::StringFormat sfmt;
        sfmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        sfmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        auto drawIcon = [&](const wchar_t* icon, RECT& r, bool isClose) {
            Gdiplus::Color c = (isClose && hovered_button_ == 3)
                ? Gdiplus::Color(255,255,255,255) : Gdiplus::Color(255,220,220,220);
            Gdiplus::SolidBrush b(c);
            Gdiplus::RectF rf((float)r.left,(float)r.top,(float)(r.right-r.left),(float)(r.bottom-r.top));
            g.DrawString(icon, -1, font_icon_, rf, &sfmt, &b);
        };
        if (recording_) {
            Gdiplus::SolidBrush red(Gdiplus::Color(255, 244, 67, 54));
            Gdiplus::RectF liveRect((float)rect_capture_.left, (float)rect_capture_.top,
                (float)(rect_capture_.right - rect_capture_.left), (float)(rect_capture_.bottom - rect_capture_.top));
            g.DrawString(L"LIVE", -1, font_live_, liveRect, &sfmt, &red);
        } else {
            drawIcon(ICON_CAPTURE, rect_capture_, false);
        }
        drawIcon(ICON_DRAG, rect_drag_, false);
        drawIcon(ICON_MINIMIZE, rect_min_, false);
        drawIcon(is_max_height_ ? ICON_RESTORE : ICON_MAXIMIZE, rect_max_, false);
        drawIcon(ICON_CLOSE, rect_close_, true);

        // Recording bubble
        if (recording_) {
            Gdiplus::RectF rbbr((float)rect_recording_bubble_.left, (float)rect_recording_bubble_.top,
                (float)(rect_recording_bubble_.right - rect_recording_bubble_.left) - 1.0f,
                (float)(rect_recording_bubble_.bottom - rect_recording_bubble_.top) - 1.0f);
            
            Gdiplus::GraphicsPath rbp;
            AddRoundedRect(rbp, rbbr, (float)BUBBLE_CORNER_RADIUS);
            Gdiplus::SolidBrush rbb(Gdiplus::Color(255, 196, 43, 28)); // Red Bubble
            g.FillPath(&rbb, &rbp);
            Gdiplus::Pen rpen(Gdiplus::Color(255, 220, 60, 40), 1.0f);
            g.DrawPath(&rpen, &rbp);

            // Timer Text
            auto now = std::chrono::steady_clock::now();
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - recording_start_time_).count();
            int mins = diff / 60;
            int secs = diff % 60;
            wchar_t time_buf[16];
            swprintf_s(time_buf, 16, L"%02d:%02d", mins, secs);
            std::wstring timeStr(time_buf);

            Gdiplus::SolidBrush textB(Gdiplus::Color(255, 255, 255, 255));
            Gdiplus::RectF timeRect((float)rect_recording_bubble_.left, (float)rect_recording_bubble_.top,
                (float)(rect_recording_bubble_.right - rect_recording_bubble_.left - BUBBLE_H), (float)(rect_recording_bubble_.bottom - rect_recording_bubble_.top));
            
            // Stop Icon
            Gdiplus::RectF stopRect((float)rect_recording_stop_.left, (float)rect_recording_stop_.top,
                (float)(rect_recording_stop_.right - rect_recording_stop_.left), (float)(rect_recording_stop_.bottom - rect_recording_stop_.top));
            
            if (hovered_button_ == 5) {
                Gdiplus::GraphicsPath clip;
                AddRoundedRect(clip, rbbr, (float)BUBBLE_CORNER_RADIUS);
                Gdiplus::Region clipRgn(&clip);
                g.SetClip(&clipRgn);
                Gdiplus::SolidBrush hb(Gdiplus::Color(255, 230, 70, 50));
                g.FillRectangle(&hb, stopRect);
                g.ResetClip();
            }
            g.DrawString(timeStr.c_str(), -1, font_timer_, timeRect, &sfmt, &textB);
        }

        if (screenshot_flash_) {
            Gdiplus::SolidBrush sb(Gdiplus::Color(255, 30, 144, 255)); // Blue flash bubble
            Gdiplus::RectF sbbr((float)rect_screenshot_bubble_.left, (float)rect_screenshot_bubble_.top,
                (float)(rect_screenshot_bubble_.right - rect_screenshot_bubble_.left) - 1.0f,
                (float)(rect_screenshot_bubble_.bottom - rect_screenshot_bubble_.top) - 1.0f);
            
            Gdiplus::GraphicsPath spath;
            AddRoundedRect(spath, sbbr, (float)BUBBLE_CORNER_RADIUS);
            g.FillPath(&sb, &spath);

            // Draw Camera Icon in blue bubble
            Gdiplus::SolidBrush whiteB(Gdiplus::Color(255, 255, 255, 255));
            Gdiplus::RectF iconRect((float)rect_screenshot_bubble_.left, (float)rect_screenshot_bubble_.top,
                (float)(rect_screenshot_bubble_.right - rect_screenshot_bubble_.left), (float)(rect_screenshot_bubble_.bottom - rect_screenshot_bubble_.top));
            g.DrawString(L"\xE722", -1, font_icon_, iconRect, &sfmt, &whiteB);
        }

        draw_transfer_bubble(g);

        if (app_state_ != AppState::STREAMING) {
            switch (app_state_) {
            case AppState::SETUP:    draw_setup_screen(g); break;
            case AppState::SCANNING: draw_scanning_screen(g); break;
            case AppState::CONNECTED:draw_connected_screen(g); break;
            case AppState::STREAMING:draw_streaming_screen(g); break;
            }
        }
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
}

// Ugg! Rock is on its way to the phone. Ring fills up, name says which rock.
void Win32Window::draw_transfer_bubble(Gdiplus::Graphics& g) {
    if (transfer_state_ == TransferState::IDLE) return;

    Gdiplus::RectF box((float)rect_transfer_bubble_.left, (float)rect_transfer_bubble_.top,
        (float)(rect_transfer_bubble_.right - rect_transfer_bubble_.left) - 1.0f,
        (float)(rect_transfer_bubble_.bottom - rect_transfer_bubble_.top) - 1.0f);
    if (box.Width <= 0.0f || box.Height <= 0.0f) return;

    Gdiplus::Color accent(255, 90, 170, 255);
    if (transfer_state_ == TransferState::DONE)   accent = Gdiplus::Color(255, 60, 190, 110);
    if (transfer_state_ == TransferState::FAILED) accent = Gdiplus::Color(255, 244, 67, 54);

    {
        Gdiplus::GraphicsPath bp;
        AddRoundedRect(bp, box, (float)BUBBLE_CORNER_RADIUS);
        Gdiplus::SolidBrush bb(Gdiplus::Color(255, 50, 50, 50));
        g.FillPath(&bb, &bp);
        Gdiplus::Pen pen(Gdiplus::Color(255, 75, 75, 75), 1.0f);
        g.DrawPath(&pen, &bp);
    }

    Gdiplus::StringFormat centred;
    centred.SetAlignment(Gdiplus::StringAlignmentCenter);
    centred.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    // Wide bubble: ring hugs the right end, name sits left of it. Narrow bubble has
    // no room for a name, so the ring moves to the middle.
    const float ring_d = 20.0f;
    const bool has_room_for_name =
        (rect_transfer_bubble_.right - rect_transfer_bubble_.left) > TRANSFER_RING_ONLY_W;
    const float ring_x = has_room_for_name
        ? box.GetRight() - (BUBBLE_H - ring_d) * 0.5f - ring_d
        : box.X + (box.Width - ring_d) * 0.5f;
    Gdiplus::RectF ring(ring_x, box.Y + (box.Height - ring_d) * 0.5f, ring_d, ring_d);

    Gdiplus::Pen track(Gdiplus::Color(255, 92, 92, 92), 3.0f);
    g.DrawEllipse(&track, ring);

    if (transfer_state_ == TransferState::ACTIVE) {
        const float done = std::clamp(transfer_progress_, 0.0f, 1.0f);
        if (done > 0.002f) {
            Gdiplus::Pen arc(accent, 3.0f);
            arc.SetStartCap(Gdiplus::LineCapRound);
            arc.SetEndCap(Gdiplus::LineCapRound);
            g.DrawArc(&arc, ring, -90.0f, done * 360.0f);
        }
        wchar_t percent[8];
        swprintf_s(percent, 8, L"%d", (int)(done * 100.0f + 0.5f));
        Gdiplus::SolidBrush pb(Gdiplus::Color(255, 235, 235, 235));
        g.DrawString(percent, -1, font_live_, ring, &centred, &pb);
    } else {
        Gdiplus::Pen full(accent, 3.0f);
        g.DrawEllipse(&full, ring);
        Gdiplus::SolidBrush mark(accent);
        if (transfer_state_ == TransferState::DONE) {
            g.DrawString(ICON_CHECK, -1, font_icon_, ring, &centred, &mark);
        } else {
            g.DrawString(L"!", -1, font_timer_, ring, &centred, &mark);
        }
    }

    const float name_w = ring_x - box.X - 14.0f;
    if (has_room_for_name && name_w > 20.0f && !transfer_label_.empty()) {
        Gdiplus::StringFormat name_fmt;
        name_fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        name_fmt.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        name_fmt.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        Gdiplus::SolidBrush nb(Gdiplus::Color(255, 225, 225, 225));
        Gdiplus::RectF nr(box.X + 12.0f, box.Y, name_w, box.Height);
        g.DrawString(utf8_to_wstring(transfer_label_).c_str(), -1, font_timer_, nr, &name_fmt, &nb);
    }
}

void Win32Window::draw_setup_screen(Gdiplus::Graphics& g) {
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    float pw = (float)(rect_phone_.right - rect_phone_.left);
    float ph = (float)(rect_phone_.bottom - rect_phone_.top);
    float px = (float)rect_phone_.left;
    float py = (float)rect_phone_.top;

    // Title
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 230, 230, 230));
    Gdiplus::RectF titleR(px, py + ph * 0.15f, pw, 30);
    g.DrawString(L"Pixel Mirroring", -1, font_title_, titleR, &sf, &white);

    // Instructions
    Gdiplus::SolidBrush gray(Gdiplus::Color(255, 160, 160, 160));
    Gdiplus::RectF instrR(px + 20, py + ph * 0.30f, pw - 40, ph * 0.30f);
    g.DrawString(L"1. USB-Debugging am Handy\r\n    einmal aktivieren\r\n\r\n"
                 L"2. Handy per USB verbinden\r\n    (nur für die Einrichtung)\r\n\r\n"
                 L"3. Verbinden drücken - die\r\n    Android-App wird installiert",
                 -1, font_body_, instrR, nullptr, &gray);

    // Start button
    Gdiplus::RectF btnR((float)rect_start_btn_.left, (float)rect_start_btn_.top,
        (float)(rect_start_btn_.right - rect_start_btn_.left),
        (float)(rect_start_btn_.bottom - rect_start_btn_.top));
    {
        Gdiplus::GraphicsPath bp;
        AddRoundedRect(bp, btnR, 10);
        Gdiplus::Color btnColor = start_button_hovered_
            ? Gdiplus::Color(255, 80, 140, 255) : Gdiplus::Color(255, 55, 120, 250);
        Gdiplus::SolidBrush bb(btnColor);
        g.FillPath(&bb, &bp);
    }
    Gdiplus::SolidBrush btnText(Gdiplus::Color(255, 255, 255, 255));
    g.DrawString(L"Verbinden", -1, font_button_, btnR, &sf, &btnText);

    if (!status_text_.empty()) {
        Gdiplus::SolidBrush errColor(Gdiplus::Color(255, 255, 110, 110));
        std::wstring ws = utf8_to_wstring(status_text_);
        Gdiplus::RectF sr(px + 10.0f, btnR.Y + btnR.Height + 15.0f, pw - 20.0f, 60.0f);
        g.DrawString(ws.c_str(), -1, font_body_, sr, &sf, &errColor);
    }
}

void Win32Window::draw_scanning_screen(Gdiplus::Graphics& g) {
    draw_connected_screen(g);
}

void Win32Window::draw_connected_screen(Gdiplus::Graphics& g) {
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    float pw = (float)(rect_phone_.right - rect_phone_.left);
    float ph = (float)(rect_phone_.bottom - rect_phone_.top);
    float px = (float)rect_phone_.left;
    float py = (float)rect_phone_.top;
    float cy = py + ph * 0.45f;

    scan_animation_frame_++;
    
    // Modern spinner
    float radius = 24.0f;
    Gdiplus::RectF arcRect(px + pw / 2.0f - radius, cy - radius - 30.0f, radius * 2.0f, radius * 2.0f);
    
    Gdiplus::Pen bgPen(Gdiplus::Color(255, 40, 40, 40), 4.0f);
    g.DrawEllipse(&bgPen, arcRect);
    
    float angle = fmodf((float)scan_animation_frame_ * 5.0f, 360.0f);
    Gdiplus::Pen spinnerPen(Gdiplus::Color(255, 60, 150, 255), 4.0f);
    spinnerPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    g.DrawArc(&spinnerPen, arcRect, angle - 90.0f, 120.0f);

    Gdiplus::SolidBrush white(Gdiplus::Color(255, 230, 230, 230));
    Gdiplus::RectF r(px, cy + 10.0f, pw, 30);
    g.DrawString(L"Verbindung wird hergestellt...", -1, font_status_, r, &sf, &white);

    if (!status_text_.empty()) {
        Gdiplus::SolidBrush gray(Gdiplus::Color(255, 140, 140, 140));
        std::wstring ws = utf8_to_wstring(status_text_);
        Gdiplus::RectF sr(px + 10.0f, cy + 40.0f, pw - 20.0f, 60.0f);
        g.DrawString(ws.c_str(), -1, font_body_, sr, &sf, &gray);
    }
}

void Win32Window::draw_streaming_screen(Gdiplus::Graphics& g) {
    if (!m_render_cb_) {
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        float pw = (float)(rect_phone_.right - rect_phone_.left);
        float ph = (float)(rect_phone_.bottom - rect_phone_.top);
        float px = (float)rect_phone_.left;
        float py = (float)rect_phone_.top;
        float cy = py + ph * 0.45f;

        scan_animation_frame_++;
        float radius = 24.0f;
        Gdiplus::RectF arcRect(px + pw / 2.0f - radius, cy - radius - 30.0f, radius * 2.0f, radius * 2.0f);
        
        Gdiplus::Pen bgPen(Gdiplus::Color(255, 40, 40, 40), 4.0f);
        g.DrawEllipse(&bgPen, arcRect);
        
        float angle = fmodf((float)scan_animation_frame_ * 5.0f, 360.0f);
        Gdiplus::Pen spinnerPen(Gdiplus::Color(255, 60, 150, 255), 4.0f);
        spinnerPen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
        g.DrawArc(&spinnerPen, arcRect, angle - 90.0f, 120.0f);

        Gdiplus::SolidBrush white(Gdiplus::Color(255, 230, 230, 230));
        Gdiplus::RectF r(px, cy + 10.0f, pw, 30);
        g.DrawString(L"Bildschirm wird geladen...", -1, font_status_, r, &sf, &white);
    }
}

// --- Message handling ---
LRESULT CALLBACK Win32Window::window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Win32Window* w = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lp);
        w = reinterpret_cast<Win32Window*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
        w->hwnd_ = hwnd;
    } else {
        w = reinterpret_cast<Win32Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (w) return w->handle_message(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Win32Window::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    static UINT restore_msg = RegisterWindowMessageA("PixelMirroringRestoreMsg");
    if (msg == restore_msg) {
        if (m_restore_cb_) {
            m_restore_cb_();
        }
        return 0;
    }

    switch (msg) {
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        int min_w = MIN_PHONE_W; // base minimum
        int bubble_req = BUBBLE_W + BUBBLE_GAP * 2; // ~200
        if (recording_) bubble_req += 100 + BUBBLE_GAP;
        if (screenshot_flash_) bubble_req += 40 + BUBBLE_GAP;
        // Only the ring is non-negotiable — the file name gives way on narrow windows.
        if (transfer_state_ != TransferState::IDLE) bubble_req += TRANSFER_RING_ONLY_W + BUBBLE_GAP;

        if (bubble_req > min_w) {
            min_w = bubble_req;
        }
        
        mmi->ptMinTrackSize.x = min_w;
        mmi->ptMinTrackSize.y = MIN_PHONE_H;
        return 0;
    }
    case WM_CLIPBOARDUPDATE: {
        if (!m_os_clipboard_cb_ || app_state_ != AppState::STREAMING) {
            return 0;
        }
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            return 0;
        }
        std::string clipboard_text;
        bool success = false;
        for (int retry = 0; retry < 3; ++retry) {
            if (OpenClipboard(hwnd_)) {
                HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                if (hData) {
                    wchar_t* pText = static_cast<wchar_t*>(GlobalLock(hData));
                    if (pText) {
                        clipboard_text = wstring_to_utf8(pText);
                        GlobalUnlock(hData);
                        success = true;
                    }
                }
                CloseClipboard();
                if (success) break;
            }
            Sleep(10);
        }
        if (success && !clipboard_text.empty()) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(clipboard_mutex_);
                if (clipboard_text != last_clipboard_text_) {
                    last_clipboard_text_ = clipboard_text;
                    changed = true;
                }
            }
            if (changed && m_os_clipboard_cb_) {
                m_os_clipboard_cb_(clipboard_text);
            }
        }
        return 0;
    }
    case WM_NCCALCSIZE:
        // Cave man remove window frame — no border artifacts
        if (wp == TRUE) return 0;
        break;
    case WM_TIMER:
        if (wp == 3 && screenshot_flash_) {
            auto now = std::chrono::steady_clock::now();
            auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(now - screenshot_flash_start_).count();
            if (diff > 1500) {
                screenshot_flash_ = false;
                KillTimer(hwnd_, 3);
                recalc_layout();
                update_region();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        if (wp == 2 && recording_) {
            InvalidateRect(hwnd_, &rect_recording_bubble_, FALSE);
            return 0;
        }
        if (wp == 4) {
            // Only runs while the bubble rests on a tick or a bang — the travelling
            // ring is repainted by the progress shouts themselves.
            if (transfer_state_ == TransferState::DONE || transfer_state_ == TransferState::FAILED) {
                const auto rested = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - transfer_settled_at_).count();
                if (rested > 2500) {
                    set_transfer_status(TransferState::IDLE, 0.0f, "");
                }
            } else {
                KillTimer(hwnd_, 4);
            }
            return 0;
        }
        if (wp == 1 && app_state_ != AppState::STREAMING) {
            InvalidateRect(hwnd_, &rect_phone_, FALSE);
            return 0;
        }
        return 0;
    case WM_SIZE: {
        // Ugg! This shouts on every drag of the window edge as well, so cave man
        // only passes the word on when the window really went down or came back up.
        const bool now_minimized = (wp == SIZE_MINIMIZED);
        if (now_minimized != minimized_) {
            minimized_ = now_minimized;
            if (m_minimize_cb_) {
                m_minimize_cb_(now_minimized);
            }
        }
        if (!now_minimized) {
            recalc_layout();
            update_region();
            // Cave man trigger render now so live resize look smooth
            PostMessage(hwnd_, WM_VIDEO_RENDER, 0, 0);
        }
        return 0;
    }
    case WM_SIZING:
        handle_sizing(wp, lp); return TRUE;
    case WM_NCHITTEST:
        return handle_nchittest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
    case WM_NCRBUTTONUP: {
        if (wp == HTCAPTION) {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            POINT client_pt = pt;
            ScreenToClient(hwnd_, &client_pt);
            if (hit_test_button(client_pt) == 0) {  // 0 = drag button
                show_context_menu(client_pt);
                return 0;
            }
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int btn = hit_test_button(pt);
        if (btn == 5) {
            if (menu_cb_) menu_cb_(MenuAction::TOGGLE_RECORDING);
        }
        else if (btn == 4) show_capture_menu(pt);
        else if (btn == 1) ShowWindow(hwnd_, SW_MINIMIZE);
        else if (btn == 2) toggle_max_height();
        else if (btn == 3) PostMessage(hwnd_, WM_CLOSE, 0, 0);
        else if (app_state_ == AppState::STREAMING && PtInRect(&rect_phone_, pt)) {
            SetCapture(hwnd_);
            send_pointer_event(PointerAction::DOWN, pt.x - rect_phone_.left, pt.y - rect_phone_.top);
        }
        else if (is_start_button_hit(pt) && start_cb_) {
            // Visual feedback — immediately change state
            app_state_ = AppState::SCANNING;
            status_text_ = "Starte...";
            InvalidateRect(hwnd_, nullptr, FALSE);
            // Post message to self so callback runs after repaint
            PostMessage(hwnd_, WM_APP + 1, 0, 0);
        }
        return 0;
    }
    case WM_APP + 1:
        // Deferred start callback — cave man wait for paint, then go
        if (start_cb_) start_cb_();
        return 0;
    case WM_DROPFILES:
        handle_dropped_files(wp);
        return 0;
    case WM_FILES_DROPPED: {
        auto* paths = reinterpret_cast<std::vector<std::string>*>(lp);
        if (paths) {
            if (m_file_drop_cb_) m_file_drop_cb_(*paths);
            delete paths;
        }
        return 0;
    }
    case WM_NCMOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd_, &pt);
        int nh = hit_test_button(pt);
        bool sbh = is_start_button_hit(pt);
        if (nh != hovered_button_ || sbh != start_button_hovered_) {
            hovered_button_ = nh;
            start_button_hovered_ = sbh;
            InvalidateRect(hwnd_, nullptr, FALSE);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd_, 0};
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (app_state_ == AppState::STREAMING && (wp & MK_LBUTTON) != 0 && GetCapture() == hwnd_) {
            send_pointer_event(PointerAction::MOVE, pt.x - rect_phone_.left, pt.y - rect_phone_.top);
            return 0;
        }
        int nh = hit_test_button(pt);
        bool sbh = is_start_button_hit(pt);
        if (nh != hovered_button_ || sbh != start_button_hovered_) {
            hovered_button_ = nh;
            start_button_hovered_ = sbh;
            InvalidateRect(hwnd_, nullptr, FALSE);
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (app_state_ == AppState::STREAMING && GetCapture() == hwnd_) {
            send_pointer_event(PointerAction::UP, pt.x - rect_phone_.left, pt.y - rect_phone_.top);
            ReleaseCapture();
        }
        return 0;
    }
    case WM_NCMOUSELEAVE:
    case WM_MOUSELEAVE: {
        POINT sp;
        GetCursorPos(&sp);
        ScreenToClient(hwnd_, &sp);
        int nh = hit_test_button(sp);
        bool sbh = is_start_button_hit(sp);
        if (nh != hovered_button_ || sbh != start_button_hovered_) {
            hovered_button_ = nh;
            start_button_hovered_ = sbh;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_SETFOCUS:
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && m_focus_cb_ && app_state_ == AppState::STREAMING) {
            // focus get. cave man fetch clipboard.
            m_focus_cb_();
        }
        break;
    case WM_KEYDOWN:
    case WM_KEYUP: {
        if (msg == WM_KEYDOWN && app_state_ == AppState::STREAMING) {
            if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                // ctrl+c press. cave man fetch clipboard.
                if (m_focus_cb_) {
                    m_focus_cb_();
                }
            }
            if (wp == 'U' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                swallowed_shortcuts_.insert(static_cast<UINT>(wp));
                if (menu_cb_) {
                    menu_cb_(MenuAction::UNLOCK_DEVICE);
                }
                return 0;
            }
            if (wp == 'L' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                // Cave man lock screen and turn off light
                swallowed_shortcuts_.insert(static_cast<UINT>(wp));
                if (menu_cb_) {
                    menu_cb_(MenuAction::LOCK_DEVICE);
                }
                return 0;
            }
        }
        // Ugg! Cave man ate the press, so as far as the phone is concerned that key
        // never went down — and a "let go" for a key nobody pressed is a word the
        // phone did not need to hear. Eat the release of the same key too.
        if (msg == WM_KEYUP && swallowed_shortcuts_.erase(static_cast<UINT>(wp)) > 0) {
            return 0;
        }
        // With the virtual USB keyboard hanging on the phone every key travels as
        // a real key press instead. Volume keys stay behind: a boot keyboard has
        // no such button, so they keep riding the Android keycode path below.
        // (send_raw_key would hand them back anyway now — this only spares the
        // fake keyboard a question whose answer is already known.)
        if (uhid_keyboard_ && app_state_ == AppState::STREAMING &&
            wp != VK_VOLUME_UP && wp != VK_VOLUME_DOWN && wp != VK_VOLUME_MUTE) {
            if (send_raw_key(msg == WM_KEYDOWN, lp)) {
                return 0;
            }
        }
        if (app_state_ == AppState::STREAMING && m_key_cb_) {
            int ak = vk_to_android_keycode(wp);
            if (ak != 0) {
                m_key_cb_(msg == WM_KEYDOWN ? 0 : 1, ak);
                return 0;
            }
        }
        break;
    }
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP: {
        // Ugg! While Alt is pressed Windows sends SYS messages instead of the normal
        // ones. Everything we do not claim falls through to DefWindowProc, so Alt+F4
        // and friends keep working.
        //
        // AltGr is NOT Alt: the tribe sends it as Ctrl+Alt together. On a German
        // keyboard AltGr+Q is how a human writes "@". Claiming it here would eat that
        // letter before it ever reaches the phone, so Ctrl must be off the stone.
        const bool alt_alone = (GetKeyState(VK_MENU) & 0x8000) != 0 &&
                               (GetKeyState(VK_CONTROL) & 0x8000) == 0;
        if (app_state_ == AppState::STREAMING && alt_alone) {
            // The phone's top curtains are not keys — they ride the menu path, the
            // same way Strg+U/L do. Alt+Down pulls them down like a thumb on the
            // phone would, Alt+Up shoves them back. Down, up and every repeat in
            // between get eaten here, otherwise DefWindowProc hears a lonely Alt and
            // beeps at a menu bar that does not exist.
            if (wp == VK_DOWN || wp == VK_UP) {
                const bool first_press = (msg == WM_SYSKEYDOWN) && !(lp & (1 << 30));
                if (first_press && menu_cb_) {
                    if (wp == VK_UP) {
                        menu_cb_(MenuAction::COLLAPSE_PANELS);
                    } else {
                        // One thumb-pull opens the messages, a harder one the switches.
                        menu_cb_((GetKeyState(VK_SHIFT) & 0x8000)
                            ? MenuAction::EXPAND_SETTINGS_PANEL
                            : MenuAction::EXPAND_NOTIFICATION_PANEL);
                    }
                }
                return 0;
            }
            if (m_key_cb_) {
                const int ak = alt_shortcut_to_android_keycode(wp);
                if (ak != 0) {
                    m_key_cb_(msg == WM_SYSKEYDOWN ? 0 : 1, ak);
                    return 0;
                }
            }
        }
        // The USB keyboard needs the Alt key itself, otherwise the phone never
        // learns Alt went down and every Alt combo arrives naked. Alt+F4 stays
        // with the tribe so cave man can still close the cave.
        if (uhid_keyboard_ && app_state_ == AppState::STREAMING && wp != VK_F4) {
            if (send_raw_key(msg == WM_SYSKEYDOWN, lp)) {
                return 0;
            }
        }
        break;
    }
    case WM_KILLFOCUS: {
        // Cave man walks away mid-word. Everything he still held goes up now.
        hid_held_keys_.clear();
        swallowed_shortcuts_.clear();
        if (m_focus_lost_cb_) {
            m_focus_lost_cb_();
        }
        break;
    }
    case WM_CHAR: {
        // In USB keyboard mode the key itself already flew over — typing the
        // letter on top of it would double every single character.
        if (app_state_ == AppState::STREAMING && m_text_cb_ && !uhid_keyboard_) {
            wchar_t wch = static_cast<wchar_t>(wp);
            if (wch >= 32) {
                std::string utf8 = wchar_to_utf8(wch);
                if (!utf8.empty()) {
                    m_text_cb_(utf8);
                }
                return 0;
            }
        }
        break;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        if (app_state_ == AppState::STREAMING && m_scroll_cb_) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd_, &pt);
            if (PtInRect(&rect_phone_, pt)) {
                int delta = GET_WHEEL_DELTA_WPARAM(wp);
                float scroll_val = static_cast<float>(delta) / 120.0f;
                int rx = pt.x - rect_phone_.left;
                int ry = pt.y - rect_phone_.top;
                int rw = rect_phone_.right - rect_phone_.left;
                int rh = rect_phone_.bottom - rect_phone_.top;
                
                if (msg == WM_MOUSEWHEEL) {
                    m_scroll_cb_(rx, ry, rw, rh, 0.0f, scroll_val);
                } else {
                    m_scroll_cb_(rx, ry, rw, rh, scroll_val, 0.0f);
                }
                return 0;
            }
        }
        break;
    }
    case WM_VIDEO_RENDER:
        // Cave man render direct on child window — coordinates relative to child!
        if (app_state_ == AppState::STREAMING && m_render_cb_ && m_sdl_renderer) {
            int w = rect_phone_.right - rect_phone_.left;
            int h = rect_phone_.bottom - rect_phone_.top;
            if (w > 0 && h > 0) {
                SDL_RenderClear(m_sdl_renderer);
            }
            m_render_cb_(m_sdl_renderer, 0, 0, w, h);
            if (w > 0 && h > 0) {
                SDL_RenderPresent(m_sdl_renderer);
            }
        }
        return 0;
    case WM_APP + 3: {
        // Cave man run task on UI thread
        auto* task = reinterpret_cast<std::function<void()>*>(lp);
        if (task) {
            (*task)();
            delete task;
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_PAINT: handle_paint(); return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT Win32Window::handle_nchittest(POINT sp) {
    POINT pt = sp; ScreenToClient(hwnd_, &pt);
    if (PtInRect(&rect_drag_, pt)) return HTCAPTION;
    if (PtInRect(&rect_bubble_, pt)) return HTCLIENT;
    int b = 8;
    bool l = pt.x >= rect_phone_.left && pt.x <= rect_phone_.left + b;
    bool r = pt.x >= rect_phone_.right - b && pt.x <= rect_phone_.right;
    bool t = pt.y >= rect_phone_.top && pt.y <= rect_phone_.top + b;
    bool bo = pt.y >= rect_phone_.bottom - b && pt.y <= rect_phone_.bottom;
    if (t && l) return HTTOPLEFT;    if (t && r) return HTTOPRIGHT;
    if (bo && l) return HTBOTTOMLEFT; if (bo && r) return HTBOTTOMRIGHT;
    if (l) return HTLEFT; if (r) return HTRIGHT;
    if (t) return HTTOP;  if (bo) return HTBOTTOM;
    return HTCLIENT;
}

void Win32Window::handle_sizing(WPARAM edge, LPARAM lp) {
    if (aspect_ratio_ <= 0.0) return;
    RECT* r = reinterpret_cast<RECT*>(lp);
    int tw = r->right - r->left, th = r->bottom - r->top;
    int ph = th - BUBBLE_H - BUBBLE_GAP, pw = tw;
    int npw, nph;

    if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) {
        npw = pw; nph = (int)(pw / aspect_ratio_);
    } else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
        nph = ph; npw = (int)(ph * aspect_ratio_);
    } else { npw = pw; nph = (int)(pw / aspect_ratio_); }

    if (npw < MIN_PHONE_W) { npw = MIN_PHONE_W; nph = (int)(MIN_PHONE_W / aspect_ratio_); }
    int ntw = npw, nth = nph + BUBBLE_H + BUBBLE_GAP;

    switch (edge) {
    case WMSZ_RIGHT: case WMSZ_BOTTOM: case WMSZ_BOTTOMRIGHT:
        r->right = r->left + ntw; r->bottom = r->top + nth; break;
    case WMSZ_LEFT: case WMSZ_BOTTOMLEFT:
        r->left = r->right - ntw; r->bottom = r->top + nth; break;
    case WMSZ_TOP: case WMSZ_TOPRIGHT:
        r->right = r->left + ntw; r->top = r->bottom - nth; break;
    case WMSZ_TOPLEFT:
        r->left = r->right - ntw; r->top = r->bottom - nth; break;
    }
}

void Win32Window::post_task(std::function<void()> task) {
    if (!hwnd_ || !task) return;
    // Cave man allocate task on heap, UI thread delete after run.
    // If the message never reaches the queue, cave man cleans up himself.
    auto* heap_task = new std::function<void()>(std::move(task));
    if (!PostMessage(hwnd_, WM_APP + 3, 0, reinterpret_cast<LPARAM>(heap_task))) {
        delete heap_task;
    }
}

namespace {
    constexpr UINT ID_FACTORY_RESET = 1001;
    constexpr UINT ID_QUALITY_HEADER = 1002; // folds the three quality stones open
    constexpr UINT ID_QUALITY_BATTERY  = 1010;
    constexpr UINT ID_QUALITY_BALANCED = 1011;
    constexpr UINT ID_QUALITY_MAXIMUM  = 1012;
    constexpr UINT ID_SET_PIN       = 1004;
    constexpr UINT ID_UNLOCK_DEVICE = 1005;
    constexpr UINT ID_TOGGLE_COMPAT = 1006;
    constexpr UINT ID_TOGGLE_LOWEST_BRIGHTNESS = 1007;
    constexpr UINT ID_LOCK_DEVICE   = 1008;
    constexpr UINT ID_TOGGLE_AUDIO  = 1009;
    constexpr UINT ID_TOGGLE_SCREEN_OFF = 1013;
    // Ugg! 1010 already belongs to the battery preset since the quality stones
    // arrived. Two menu rows with the same number means one of them fires the
    // other's action, so the rotation switch moved out of the way.
    constexpr UINT ID_TOGGLE_AUTO_ROTATE = 1016;
    constexpr UINT ID_TOGGLE_UHID_KEYBOARD = 1014;
    constexpr UINT ID_OPEN_KEYBOARD_SETTINGS = 1015;
    constexpr UINT ID_TOGGLE_AUTO_PAUSE = 1017;

    constexpr UINT ID_SCREENSHOT = 1101;
    constexpr UINT ID_TOGGLE_RECORDING = 1102;
    constexpr UINT ID_TOGGLE_SEND_TO_PHONE = 1103;

    struct MenuItem {
        UINT id;
        std::wstring text;
        bool has_toggle;
        bool is_toggled;
        bool is_separator;
        // Ugg! A row that only tells, never listens. No hover, no click.
        bool is_info = false;
        // Row that folds its children open instead of doing something.
        bool is_expander = false;
        // One of the folded-out children. Only one of them wears the dot.
        bool is_radio = false;
        bool is_selected = false;
    };

    std::vector<MenuItem> g_menu_items;
    int g_hovered_item = -1;
    UINT g_selected_action = 0;
    bool g_menu_done = false;
    constexpr int MENU_WIDTH = 320;

    int menu_height_for_items() {
        int height = 10;
        for (const auto& item : g_menu_items) {
            height += item.is_separator ? 11 : 30;
        }
        return height;
    }

    // Ugg! Menu must not hang off the edge of the cave wall — especially once it
    // grows by three rows while it is already open.
    void clamp_menu_to_monitor(POINT& pt, int height) {
        HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = {sizeof(MONITORINFO)};
        if (!GetMonitorInfo(monitor, &mi)) return;
        if (pt.y + height > mi.rcWork.bottom) {
            pt.y = mi.rcWork.bottom - height;
        }
        if (pt.y < mi.rcWork.top) pt.y = mi.rcWork.top;
        if (pt.x + MENU_WIDTH > mi.rcWork.right) {
            pt.x = mi.rcWork.right - MENU_WIDTH;
        }
        if (pt.x < mi.rcWork.left) pt.x = mi.rcWork.left;
    }

    // Menu grew or shrank. New stone size, new round corners, fresh paint.
    void resize_menu_window(HWND hMenu) {
        const int height = menu_height_for_items();
        RECT wr = {0};
        GetWindowRect(hMenu, &wr);
        POINT pt = {wr.left, wr.top};
        clamp_menu_to_monitor(pt, height);
        SetWindowPos(hMenu, nullptr, pt.x, pt.y, MENU_WIDTH, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowRgn(hMenu, CreateRoundRectRgn(0, 0, MENU_WIDTH, height, 14, 14), TRUE);
        g_hovered_item = -1;
        InvalidateRect(hMenu, nullptr, TRUE);
    }

    LRESULT CALLBACK SettingsMenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                int corner_preference = 2; // DWMWCP_ROUND
                DwmSetWindowAttribute(hwnd, 33, &corner_preference, sizeof(corner_preference));
                return 0;
            }
            case WM_ERASEBKGND:
                return 1; // Prevent flickering
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc; GetClientRect(hwnd, &rc);
                
                Gdiplus::Bitmap bmp(rc.right, rc.bottom, PixelFormat32bppARGB);
                Gdiplus::Graphics g(&bmp);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

                Gdiplus::GraphicsPath bgPath;
                AddRoundedRect(bgPath, Gdiplus::RectF(0.5f, 0.5f, (float)rc.right - 1.0f, (float)rc.bottom - 1.0f), 12.0f);

                Gdiplus::SolidBrush bgBrush(Gdiplus::Color(255, 32, 32, 32));
                g.FillPath(&bgBrush, &bgPath);

                Gdiplus::Pen borderPen(Gdiplus::Color(255, 70, 70, 70), 1.0f);
                g.DrawPath(&borderPen, &bgPath);

                Gdiplus::FontFamily fontFamily(L"Segoe UI");
                Gdiplus::Font font(&fontFamily, 10.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 240, 240, 240));
                Gdiplus::StringFormat format;
                format.SetAlignment(Gdiplus::StringAlignmentNear);
                format.SetLineAlignment(Gdiplus::StringAlignmentCenter);

                int y = 5;
                for (size_t i = 0; i < g_menu_items.size(); ++i) {
                    const auto& item = g_menu_items[i];
                    if (item.is_separator) {
                        Gdiplus::Pen sepPen(Gdiplus::Color(255, 70, 70, 70), 1.0f);
                        g.DrawLine(&sepPen, 15, y + 5, rc.right - 15, y + 5);
                        y += 11;
                        continue;
                    }

                    if ((int)i == g_hovered_item) {
                        Gdiplus::SolidBrush hoverBrush(Gdiplus::Color(255, 60, 60, 60));
                        g.FillRectangle(&hoverBrush, 0, y, rc.right, 30);
                    }

                    Gdiplus::RectF textRect(15.0f, (float)y, (float)rc.right - 15.0f, 30.0f);

                    if (item.is_info) {
                        // Dimmer than the real entries so it reads as a hint, not a button.
                        Gdiplus::Font infoFont(&fontFamily, 8.5f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                        Gdiplus::SolidBrush infoBrush(Gdiplus::Color(255, 150, 150, 150));
                        g.DrawString(item.text.c_str(), -1, &infoFont, textRect, &format, &infoBrush);
                        y += 30;
                        continue;
                    }

                    if (item.is_radio) {
                        // Dot on the left, text pushed in — reads as "pick one of these".
                        const float cx = 26.0f;
                        const float cy = (float)y + 15.0f;
                        const Gdiplus::Color accent(255, 76, 175, 80);
                        Gdiplus::Pen ring(item.is_selected ? accent : Gdiplus::Color(255, 110, 110, 110), 1.5f);
                        g.DrawEllipse(&ring, cx - 7.0f, cy - 7.0f, 14.0f, 14.0f);
                        if (item.is_selected) {
                            Gdiplus::SolidBrush dot(accent);
                            g.FillEllipse(&dot, cx - 3.5f, cy - 3.5f, 7.0f, 7.0f);
                        }
                        Gdiplus::Font childFont(&fontFamily, 9.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
                        Gdiplus::SolidBrush childBrush(item.is_selected
                            ? Gdiplus::Color(255, 240, 240, 240)
                            : Gdiplus::Color(255, 190, 190, 190));
                        textRect.X = 42.0f;
                        textRect.Width = (float)rc.right - 42.0f - 15.0f;
                        g.DrawString(item.text.c_str(), -1, &childFont, textRect, &format, &childBrush);
                        y += 30;
                        continue;
                    }

                    if (item.is_expander) {
                        // Little arrow instead of a switch: points down while folded open.
                        const float ax = (float)rc.right - 35.0f;
                        const float ay = (float)y + 15.0f;
                        Gdiplus::PointF tri[3];
                        if (item.is_toggled) {
                            tri[0] = Gdiplus::PointF(ax - 5.0f, ay - 3.0f);
                            tri[1] = Gdiplus::PointF(ax + 5.0f, ay - 3.0f);
                            tri[2] = Gdiplus::PointF(ax, ay + 4.0f);
                        } else {
                            tri[0] = Gdiplus::PointF(ax - 3.0f, ay - 5.0f);
                            tri[1] = Gdiplus::PointF(ax + 4.0f, ay);
                            tri[2] = Gdiplus::PointF(ax - 3.0f, ay + 5.0f);
                        }
                        Gdiplus::SolidBrush arrow(Gdiplus::Color(255, 180, 180, 180));
                        g.FillPolygon(&arrow, tri, 3);
                        textRect.Width = ax - 25.0f;
                    }

                    if (item.has_toggle) {
                        // Draw toggle box
                        float boxW = 30.0f;
                        float boxH = 16.0f;
                        float boxX = textRect.Width - boxW - 15.0f;
                        float boxY = y + 7.0f;
                        
                        Gdiplus::GraphicsPath path;
                        AddRoundedRect(path, Gdiplus::RectF(boxX, boxY, boxW, boxH), boxH / 2.0f);
                        
                        if (item.is_toggled) {
                            Gdiplus::SolidBrush toggleBg(Gdiplus::Color(255, 76, 175, 80)); // Green
                            g.FillPath(&toggleBg, &path);
                            Gdiplus::SolidBrush circle(Gdiplus::Color(255, 255, 255, 255));
                            g.FillEllipse(&circle, boxX + boxW - 14.0f, boxY + 2.0f, 12.0f, 12.0f);
                        } else {
                            Gdiplus::SolidBrush toggleBg(Gdiplus::Color(255, 70, 70, 70)); // Dark grey
                            g.FillPath(&toggleBg, &path);
                            Gdiplus::Pen border(Gdiplus::Color(255, 100, 100, 100), 1.0f);
                            g.DrawPath(&border, &path);
                            Gdiplus::SolidBrush circle(Gdiplus::Color(255, 150, 150, 150));
                            g.FillEllipse(&circle, boxX + 2.0f, boxY + 2.0f, 12.0f, 12.0f);
                        }
                        
                        textRect.Width = boxX - 10.0f - 15.0f;
                    }

                    g.DrawString(item.text.c_str(), -1, &font, textRect, &format, &textBrush);
                    y += 30;
                }
                
                Gdiplus::Graphics gFinal(hdc);
                gFinal.DrawImage(&bmp, 0, 0);
                
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_MOUSEMOVE: {
                POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                int y = 5;
                int hovered = -1;
                for (size_t i = 0; i < g_menu_items.size(); ++i) {
                    if (g_menu_items[i].is_separator) {
                        y += 11;
                        continue;
                    }
                    if (g_menu_items[i].is_info) {
                        y += 30; // takes up space, but cannot be picked
                        continue;
                    }
                    RECT item_rc = {0, y, 320, y + 30};
                    if (PtInRect(&item_rc, pt)) {
                        hovered = (int)i;
                        break;
                    }
                    y += 30;
                }
                if (hovered != g_hovered_item) {
                    g_hovered_item = hovered;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    TRACKMOUSEEVENT tme = {sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                    TrackMouseEvent(&tme);
                }
                return 0;
            }
            case WM_MOUSELEAVE: {
                g_hovered_item = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            case WM_LBUTTONDOWN: {
                SetFocus(hwnd);
                return 0;
            }
            case WM_LBUTTONUP: {
                if (g_hovered_item >= 0 && g_hovered_item < (int)g_menu_items.size()) {
                    const auto& item = g_menu_items[g_hovered_item];
                    if (item.is_expander) {
                        // Folding happens where the window state lives — just tell it.
                        const UINT id = item.id;
                        PostMessage(hwnd, WM_APP + 5, id, 0);
                    } else if (item.has_toggle) {
                        g_menu_items[g_hovered_item].is_toggled = !item.is_toggled;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        PostMessage(hwnd, WM_APP + 5, item.id, 0);
                    } else {
                        g_selected_action = item.id;
                        g_menu_done = true;
                        PostMessage(hwnd, WM_NULL, 0, 0);
                    }
                }
                return 0;
            }
            case WM_KILLFOCUS: {
                HWND hNewFocus = (HWND)wp;
                if (hNewFocus != hwnd && GetParent(hNewFocus) != hwnd) {
                    g_menu_done = true;
                    PostMessage(hwnd, WM_NULL, 0, 0);
                }
                return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// Cave man carves the whole settings menu again — also while it stays open, because
// folding the quality stones out changes how many rows there are.
void Win32Window::build_settings_menu_items() {
    static const wchar_t* const QUALITY_NAMES[3] = {L"Akku sparen", L"Ausgewogen", L"Maximal"};
    const int quality = (quality_preset_ >= 0 && quality_preset_ <= 2) ? quality_preset_ : 1;

    g_menu_items.clear();

    MenuItem header{ID_QUALITY_HEADER, std::wstring(L"Qualität: ") + QUALITY_NAMES[quality],
                    false, quality_expanded_, false};
    header.is_expander = true;
    g_menu_items.push_back(header);

    if (quality_expanded_) {
        const struct { UINT id; const wchar_t* text; int index; } presets[3] = {
            {ID_QUALITY_BATTERY,  L"Akku sparen · 30 FPS · 720p · 4 Mbit/s",           0},
            {ID_QUALITY_BALANCED, L"Ausgewogen · 60 FPS · volle Auflösung · 15 Mbit/s", 1},
            {ID_QUALITY_MAXIMUM,  L"Maximal · 120 FPS · volle Auflösung · 40 Mbit/s",  2},
        };
        for (const auto& preset : presets) {
            MenuItem row{preset.id, preset.text, false, false, false};
            row.is_radio = true;
            row.is_selected = (preset.index == quality);
            g_menu_items.push_back(row);
        }
    }

    g_menu_items.push_back({ID_TOGGLE_COMPAT, L"Kompatibilitätsmodus (langsame PIN)", true, compatibility_mode_, false});
    g_menu_items.push_back({ID_TOGGLE_LOWEST_BRIGHTNESS, L"Bildschirm auf niedrigste Helligkeit", true, lowest_brightness_, false});
    g_menu_items.push_back({ID_TOGGLE_SCREEN_OFF, L"Handy-Display komplett aus", true, screen_off_, false});
    g_menu_items.push_back({ID_TOGGLE_AUDIO, L"Ton vom Handy übertragen", true, audio_enabled_, false});
    g_menu_items.push_back({ID_TOGGLE_AUTO_PAUSE, L"Stream pausieren, wenn Fenster minimiert",
                            true, auto_pause_minimized_, false});
    g_menu_items.push_back({ID_TOGGLE_UHID_KEYBOARD, L"Echte USB-Tastatur am Handy", true, uhid_keyboard_, false});
    if (uhid_keyboard_) {
        if (app_state_ == AppState::STREAMING) {
            g_menu_items.push_back({ID_OPEN_KEYBOARD_SETTINGS, L"Tastaturlayout am Handy einstellen", false, false, false});
        }
        // Without a German layout on the phone side the whole point is lost, and
        // nobody guesses that on their own. One line, or it does not fit the row.
        g_menu_items.push_back({0, L"Layout am Handy muss auf Deutsch stehen", false, false, false, true});
    }
    g_menu_items.push_back({0, L"", false, false, true});
    g_menu_items.push_back({ID_SET_PIN, L"PIN zum Entsperren festlegen", false, false, false});
    if (app_state_ == AppState::STREAMING) {
        g_menu_items.push_back({ID_TOGGLE_AUTO_ROTATE, L"Automatische Bildschirmdrehung (Handy)", true, auto_rotate_enabled_, false});
        g_menu_items.push_back({ID_UNLOCK_DEVICE, L"Handy entsperren (Strg+U)", false, false, false});
        g_menu_items.push_back({ID_LOCK_DEVICE, L"Handy sperren & Bildschirm aus (Strg+L)", false, false, false});
        // Two hint rows for the key shortcuts — otherwise nobody finds them.
        g_menu_items.push_back({0, L"Alt+B Zurück · Alt+H Start · Alt+S Übersicht", false, false, false, true});
        g_menu_items.push_back({0, L"Alt+↓ Benachrichtigungen · Alt+Umschalt+↓ Schnelleinstellungen", false, false, false, true});
        g_menu_items.push_back({0, L"Alt+↑ schließt sie wieder", false, false, false, true});
    }
    g_menu_items.push_back({0, L"", false, false, true});
    g_menu_items.push_back({ID_FACTORY_RESET, L"Werkseinstellungen zurücksetzen", false, false, false});
}

void Win32Window::show_context_menu(POINT pt) {

    quality_expanded_ = false; // Every fresh menu starts folded.
    build_settings_menu_items();
    const int height = menu_height_for_items();

    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        wc.lpfnWndProc = SettingsMenuProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"PixelMirroringSettingsMenu";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        class_registered = true;
    }

    ClientToScreen(hwnd_, &pt);
    clamp_menu_to_monitor(pt, height);

    g_selected_action = 0;
    g_hovered_item = -1;

    HWND hMenu = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"PixelMirroringSettingsMenu", L"",
        WS_POPUP | WS_VISIBLE,
        pt.x, pt.y, MENU_WIDTH, height,
        hwnd_, nullptr, GetModuleHandle(nullptr), nullptr);

    HRGN rgn = CreateRoundRectRgn(0, 0, MENU_WIDTH, height, 14, 14);
    SetWindowRgn(hMenu, rgn, TRUE);
    SetFocus(hMenu);
    g_menu_done = false;

    MSG msg;
    while (true) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) {
            // Ugg! Tribe shouted "leave cave" while menu still hangs open. Old cave
            // man ate that shout and the app kept breathing invisibly. Pass it on!
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (got == -1) break;
        if (!IsWindow(hMenu) || g_menu_done) break;
        if (msg.hwnd == hMenu && msg.message == WM_APP + 5) {
            UINT action_id = static_cast<UINT>(msg.wParam);
            if (action_id == ID_QUALITY_HEADER) {
                // Fold the three stones in or out and grow the menu around them.
                quality_expanded_ = !quality_expanded_;
                build_settings_menu_items();
                resize_menu_window(hMenu);
                continue;
            }
            MenuAction action;
            switch (action_id) {
                case ID_FACTORY_RESET: action = MenuAction::FACTORY_RESET; break;
                case ID_TOGGLE_COMPAT: action = MenuAction::TOGGLE_COMPATIBILITY_MODE; break;
                case ID_TOGGLE_LOWEST_BRIGHTNESS: action = MenuAction::TOGGLE_LOWEST_BRIGHTNESS; break;
                case ID_TOGGLE_SCREEN_OFF: action = MenuAction::TOGGLE_SCREEN_OFF; break;
                case ID_TOGGLE_AUDIO:  action = MenuAction::TOGGLE_AUDIO; break;
                case ID_TOGGLE_AUTO_PAUSE: action = MenuAction::TOGGLE_AUTO_PAUSE_MINIMIZED; break;
                case ID_TOGGLE_AUTO_ROTATE: action = MenuAction::TOGGLE_AUTO_ROTATE; break;
                case ID_TOGGLE_UHID_KEYBOARD: action = MenuAction::TOGGLE_UHID_KEYBOARD; break;
                case ID_OPEN_KEYBOARD_SETTINGS: action = MenuAction::OPEN_KEYBOARD_SETTINGS; break;
                case ID_SET_PIN:       action = MenuAction::SET_PIN; break;
                case ID_UNLOCK_DEVICE: action = MenuAction::UNLOCK_DEVICE; break;
                case ID_LOCK_DEVICE:   action = MenuAction::LOCK_DEVICE; break;
                default: continue;
            }
            if (menu_cb_) menu_cb_(action);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    
    if (IsWindow(hMenu)) {
        DestroyWindow(hMenu);
    }

    if (g_selected_action == 0) return;

    MenuAction action;
    switch (g_selected_action) {
        case ID_FACTORY_RESET: action = MenuAction::FACTORY_RESET; break;
        case ID_QUALITY_BATTERY:  action = MenuAction::SET_QUALITY_BATTERY; break;
        case ID_QUALITY_BALANCED: action = MenuAction::SET_QUALITY_BALANCED; break;
        case ID_QUALITY_MAXIMUM:  action = MenuAction::SET_QUALITY_MAXIMUM; break;
        case ID_TOGGLE_COMPAT: action = MenuAction::TOGGLE_COMPATIBILITY_MODE; break;
        case ID_TOGGLE_LOWEST_BRIGHTNESS: action = MenuAction::TOGGLE_LOWEST_BRIGHTNESS; break;
        case ID_TOGGLE_SCREEN_OFF: action = MenuAction::TOGGLE_SCREEN_OFF; break;
        case ID_TOGGLE_AUDIO:  action = MenuAction::TOGGLE_AUDIO; break;
        case ID_TOGGLE_AUTO_PAUSE: action = MenuAction::TOGGLE_AUTO_PAUSE_MINIMIZED; break;
        case ID_TOGGLE_AUTO_ROTATE: action = MenuAction::TOGGLE_AUTO_ROTATE; break;
        case ID_TOGGLE_UHID_KEYBOARD: action = MenuAction::TOGGLE_UHID_KEYBOARD; break;
        case ID_OPEN_KEYBOARD_SETTINGS: action = MenuAction::OPEN_KEYBOARD_SETTINGS; break;
        case ID_SET_PIN:       action = MenuAction::SET_PIN; break;
        case ID_UNLOCK_DEVICE: action = MenuAction::UNLOCK_DEVICE; break;
        case ID_LOCK_DEVICE:   action = MenuAction::LOCK_DEVICE; break;
        default: return;
    }
    if (menu_cb_) menu_cb_(action);
}

void Win32Window::show_capture_menu(POINT pt) {
    constexpr UINT ID_SCREENSHOT = 1101;
    constexpr UINT ID_TOGGLE_RECORDING = 1102;
    constexpr UINT ID_TOGGLE_SEND_TO_PHONE = 1103;

    g_menu_items.clear();
    g_menu_items.push_back({ID_SCREENSHOT, L"Bildschirmfoto aufnehmen", false, false, false});
    g_menu_items.push_back({ID_TOGGLE_RECORDING,
        recording_ ? L"Videoaufnahme beenden" : L"Videoaufnahme starten", false, false, false});
    g_menu_items.push_back({0, L"", false, false, true});
    g_menu_items.push_back({ID_TOGGLE_SEND_TO_PHONE, L"Fertige Aufnahmen ans Handy senden",
        true, capture_send_to_phone_, false});

    int height = 10;
    for (const auto& item : g_menu_items) {
        height += item.is_separator ? 11 : 30;
    }

    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
        wc.lpfnWndProc = SettingsMenuProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"PixelMirroringCaptureMenu";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        class_registered = true;
    }

    ClientToScreen(hwnd_, &pt);
    g_selected_action = 0;
    g_hovered_item = -1;
    HWND hMenu = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"PixelMirroringCaptureMenu", L"", WS_POPUP | WS_VISIBLE,
        pt.x, pt.y, 320, height, hwnd_, nullptr, GetModuleHandle(nullptr), nullptr);
    if (!hMenu) return;

    HRGN rgn = CreateRoundRectRgn(0, 0, 320, height, 14, 14);
    SetWindowRgn(hMenu, rgn, TRUE);
    SetFocus(hMenu);
    g_menu_done = false;

    MSG msg;
    while (true) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0) {
            PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }
        if (got == -1) break;
        if (!IsWindow(hMenu) || g_menu_done) break;
        if (msg.hwnd == hMenu && msg.message == WM_APP + 5) {
            UINT action_id = static_cast<UINT>(msg.wParam);
            MenuAction action;
            switch (action_id) {
                case ID_SCREENSHOT: action = MenuAction::TAKE_SCREENSHOT; break;
                case ID_TOGGLE_RECORDING: action = MenuAction::TOGGLE_RECORDING; break;
                case ID_TOGGLE_SEND_TO_PHONE: action = MenuAction::TOGGLE_SEND_CAPTURES_TO_PHONE; break;
                default: continue;
            }
            if (menu_cb_) menu_cb_(action);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (IsWindow(hMenu)) DestroyWindow(hMenu);

    MenuAction action;
    switch (g_selected_action) {
        case ID_SCREENSHOT: action = MenuAction::TAKE_SCREENSHOT; break;
        case ID_TOGGLE_RECORDING: action = MenuAction::TOGGLE_RECORDING; break;
        case ID_TOGGLE_SEND_TO_PHONE: action = MenuAction::TOGGLE_SEND_CAPTURES_TO_PHONE; break;
        default: return;
    }
    if (menu_cb_) menu_cb_(action);
}

void Win32Window::set_recording(bool recording) {
    recording_ = recording;
    if (recording_) {
        recording_start_time_ = std::chrono::steady_clock::now();
        if (hwnd_) SetTimer(hwnd_, 2, 1000, nullptr);
    } else {
        if (hwnd_) KillTimer(hwnd_, 2);
    }
    if (hwnd_) {
        recalc_layout();
        update_region();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Win32Window::handle_dropped_files(WPARAM wparam) {
    HDROP drop = reinterpret_cast<HDROP>(wparam);
    if (!drop) return;

    std::vector<std::string> paths;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT chars = DragQueryFileW(drop, i, nullptr, 0);
        if (chars == 0) continue;
        std::wstring path(chars, L'\0');
        if (DragQueryFileW(drop, i, path.data(), chars + 1) == 0) continue;
        paths.push_back(wstring_to_utf8(path));
    }
    // Ugg! Hand the rock back to Explorer FIRST. Whatever we do next — even open a
    // talking-stone the human must answer — must not freeze the cave we took it from.
    DragFinish(drop);

    if (paths.empty() || !m_file_drop_cb_) return;
    auto* handover = new std::vector<std::string>(std::move(paths));
    if (!PostMessage(hwnd_, WM_FILES_DROPPED, 0, reinterpret_cast<LPARAM>(handover))) {
        delete handover;
    }
}

void Win32Window::set_transfer_status(TransferState state, float progress, const std::string& label) {
    if (!hwnd_) return;

    // Rock carriers live on their own thread. Bubbles belong to the window thread.
    if (GetCurrentThreadId() != GetWindowThreadProcessId(hwnd_, nullptr)) {
        post_task([this, state, progress, label]() { set_transfer_status(state, progress, label); });
        return;
    }

    const bool was_shown = transfer_state_ != TransferState::IDLE;
    const bool is_shown = state != TransferState::IDLE;
    transfer_state_ = state;
    transfer_progress_ = progress;
    transfer_label_ = label;

    if (state == TransferState::DONE || state == TransferState::FAILED) {
        transfer_settled_at_ = std::chrono::steady_clock::now();
        SetTimer(hwnd_, 4, 250, nullptr);
    } else {
        KillTimer(hwnd_, 4);
    }

    if (was_shown != is_shown) {
        recalc_layout();
        update_region();
        InvalidateRect(hwnd_, nullptr, FALSE);
    } else if (is_shown) {
        InvalidateRect(hwnd_, &rect_transfer_bubble_, FALSE);
    }
}

void Win32Window::trigger_screenshot_flash() {
    screenshot_flash_frames_ = 6; 
    screenshot_flash_ = true;
    screenshot_flash_start_ = std::chrono::steady_clock::now();
    if (hwnd_) {
        SetTimer(hwnd_, 3, 100, nullptr);
        recalc_layout();
        update_region();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Win32Window::set_pc_clipboard(const std::string& text) {
    if (text.empty()) return;

    {
        std::lock_guard<std::mutex> lock(clipboard_mutex_);
        if (text == last_clipboard_text_) return;
        last_clipboard_text_ = text;
    }

    std::wstring wstr = utf8_to_wstring(text);
    if (wstr.empty()) return;

    size_t bytes = (wstr.size() + 1) * sizeof(wchar_t);

    for (int retry = 0; retry < 3; ++retry) {
        if (OpenClipboard(hwnd_)) {
            EmptyClipboard();
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (hMem) {
                void* ptr = GlobalLock(hMem);
                if (ptr) {
                    memcpy(ptr, wstr.c_str(), bytes);
                    GlobalUnlock(hMem);
                    SetClipboardData(CF_UNICODETEXT, hMem);
                } else {
                    GlobalFree(hMem);
                }
            }
            CloseClipboard();
            break;
        }
        Sleep(10);
    }
}

} // namespace pm::window
#endif
