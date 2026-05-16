#ifdef _WIN32
#include "win32_window.h"
#include <windowsx.h>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")

namespace pm::window {

namespace {
    const int PHONE_CORNER_RADIUS = 24;
    const int BUBBLE_CORNER_RADIUS = 18;
    const int BUBBLE_W = 155;
    const int BUBBLE_H = 36;
    const int BUBBLE_GAP = 6;
    const int MIN_PHONE_W = 140;
    const wchar_t* ICON_DRAG = L"\uE700";
    const wchar_t* ICON_MINIMIZE = L"\uE921";
    const wchar_t* ICON_MAXIMIZE = L"\uE922";
    const wchar_t* ICON_RESTORE = L"\uE923";
    const wchar_t* ICON_CLOSE = L"\uE8BB";

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
}

std::unique_ptr<IWindow> create_window(int w, int h, const std::string& t) {
    return std::make_unique<Win32Window>(w, h, t);
}

Win32Window::Win32Window(int w, int h, const std::string& t) : width_(w), height_(h), title_(t) {}

Win32Window::~Win32Window() {
    if (icon_font_) DeleteObject(icon_font_);
    if (hwnd_) DestroyWindow(hwnd_);
}

void Win32Window::set_app_state(AppState s) { app_state_ = s; if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
void Win32Window::set_status_text(const std::string& t) { status_text_ = t; if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }

bool Win32Window::create() {
    HINSTANCE hi = GetModuleHandle(nullptr);
    WNDCLASSEXA wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Win32Window::window_proc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "PixelMirroringWindowClass";
    RegisterClassExA(&wc);

    DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX;
    int th = height_ + BUBBLE_H + BUBBLE_GAP;
    hwnd_ = CreateWindowExA(0, wc.lpszClassName, title_.c_str(), style,
        CW_USEDEFAULT, CW_USEDEFAULT, width_, th, nullptr, nullptr, hi, this);
    if (!hwnd_) return false;

    icon_font_ = CreateFontW(14, 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

    recalc_layout();
    update_region();
    SetTimer(hwnd_, 1, 16, nullptr);
    return true;
}

void Win32Window::show() { ShowWindow(hwnd_, SW_SHOW); UpdateWindow(hwnd_); }

void Win32Window::process_messages() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
}

void Win32Window::set_aspect_ratio(double r) { aspect_ratio_ = r; }
void Win32Window::set_orientation(bool l) { is_landscape_ = l; is_max_height_ = false; }

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

    int btnw = bw / 4, rem = bw - btnw * 4;
    rect_drag_  = {rect_bubble_.left, 0, rect_bubble_.left + btnw + rem, BUBBLE_H};
    rect_min_   = {rect_drag_.right, 0, rect_drag_.right + btnw, BUBBLE_H};
    rect_max_   = {rect_min_.right, 0, rect_min_.right + btnw, BUBBLE_H};
    rect_close_ = {rect_max_.right, 0, rect_max_.right + btnw, BUBBLE_H};

    // Start button in phone area
    int sbw = 180, sbh = 40;
    int px = (rect_phone_.left + rect_phone_.right) / 2;
    int py = rect_phone_.top + (rect_phone_.bottom - rect_phone_.top) * 2 / 3;
    rect_start_btn_ = {px - sbw/2, py - sbh/2, px + sbw/2, py + sbh/2};
}

void Win32Window::update_region() {
    if (!hwnd_) return;
    RECT cr; GetClientRect(hwnd_, &cr);
    if (cr.right <= 0 || cr.bottom <= 0) return;

    HRGN br = CreateRoundRectRgn(rect_bubble_.left, rect_bubble_.top,
        rect_bubble_.right+1, rect_bubble_.bottom+1, BUBBLE_CORNER_RADIUS*2, BUBBLE_CORNER_RADIUS*2);
    HRGN pr = CreateRoundRectRgn(rect_phone_.left, rect_phone_.top,
        rect_phone_.right+1, rect_phone_.bottom+1, PHONE_CORNER_RADIUS*2, PHONE_CORNER_RADIUS*2);
    HRGN c = CreateRectRgn(0,0,0,0);
    CombineRgn(c, br, pr, RGN_OR);
    SetWindowRgn(hwnd_, c, TRUE);
    DeleteObject(br); DeleteObject(pr);
}

int Win32Window::hit_test_button(POINT pt) {
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
        if (hovered_button_ >= 0 && hovered_button_ <= 3) {
            RECT hrs[] = {rect_drag_, rect_min_, rect_max_, rect_close_};
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

        // Button icons
        Gdiplus::FontFamily ff(L"Segoe MDL2 Assets");
        Gdiplus::Font iconF(&ff, 10, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        auto drawIcon = [&](const wchar_t* icon, RECT& r, bool isClose) {
            Gdiplus::Color c = (isClose && hovered_button_ == 3)
                ? Gdiplus::Color(255,255,255,255) : Gdiplus::Color(255,220,220,220);
            Gdiplus::SolidBrush b(c);
            Gdiplus::RectF rf((float)r.left,(float)r.top,(float)(r.right-r.left),(float)(r.bottom-r.top));
            g.DrawString(icon, -1, &iconF, rf, &sf, &b);
        };
        drawIcon(ICON_DRAG, rect_drag_, false);
        drawIcon(ICON_MINIMIZE, rect_min_, false);
        drawIcon(is_max_height_ ? ICON_RESTORE : ICON_MAXIMIZE, rect_max_, false);
        drawIcon(ICON_CLOSE, rect_close_, true);

        // Phone content based on state
        switch (app_state_) {
            case AppState::SETUP:    draw_setup_screen(g); break;
            case AppState::SCANNING: draw_scanning_screen(g); break;
            case AppState::CONNECTED:draw_connected_screen(g); break;
            case AppState::STREAMING:draw_streaming_screen(g); break;
        }
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd_, &ps);
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
    Gdiplus::FontFamily uiFF(L"Segoe UI");
    Gdiplus::Font titleF(&uiFF, 16, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 230, 230, 230));
    Gdiplus::RectF titleR(px, py + ph * 0.15f, pw, 30);
    g.DrawString(L"Pixel Mirroring", -1, &titleF, titleR, &sf, &white);

    // Instructions
    Gdiplus::Font bodyF(&uiFF, 9, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush gray(Gdiplus::Color(255, 160, 160, 160));
    Gdiplus::RectF instrR(px + 20, py + ph * 0.30f, pw - 40, ph * 0.30f);
    g.DrawString(L"1. USB-Debugging am Handy\r\n    einmal aktivieren\r\n\r\n"
                 L"2. Handy per USB verbinden\r\n    (nur fuer die Einrichtung)\r\n\r\n"
                 L"3. Verbinden druecken - die\r\n    Android-App wird installiert",
                 -1, &bodyF, instrR, nullptr, &gray);

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
    Gdiplus::Font btnF(&uiFF, 11, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush btnText(Gdiplus::Color(255, 255, 255, 255));
    g.DrawString(L"Verbinden", -1, &btnF, btnR, &sf, &btnText);
}

void Win32Window::draw_scanning_screen(Gdiplus::Graphics& g) {
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    float pw = (float)(rect_phone_.right - rect_phone_.left);
    float ph = (float)(rect_phone_.bottom - rect_phone_.top);
    float px = (float)rect_phone_.left;
    float py = (float)rect_phone_.top;
    float cy = py + ph * 0.4f;

    // Scanning dots animation
    scan_animation_frame_++;
    int dots = (scan_animation_frame_ / 15) % 4;
    std::wstring anim = L"Suche Ger\xE4t";
    for (int i = 0; i < dots; i++) anim += L".";

    Gdiplus::FontFamily uiFF(L"Segoe UI");
    Gdiplus::Font f(&uiFF, 13, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 200, 200, 200));
    Gdiplus::RectF r(px, cy, pw, 30);
    g.DrawString(anim.c_str(), -1, &f, r, &sf, &white);

    // Status text
    if (!status_text_.empty()) {
        Gdiplus::Font sf2(&uiFF, 8, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush gray(Gdiplus::Color(255, 120, 120, 120));
        std::wstring ws(status_text_.begin(), status_text_.end());
        Gdiplus::RectF sr(px + 10, cy + 40, pw - 20, 60);
        g.DrawString(ws.c_str(), -1, &sf2, sr, &sf, &gray);
    }
}

void Win32Window::draw_connected_screen(Gdiplus::Graphics& g) {
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    float pw = (float)(rect_phone_.right - rect_phone_.left);
    float ph = (float)(rect_phone_.bottom - rect_phone_.top);
    float px = (float)rect_phone_.left;
    float py = (float)rect_phone_.top;

    Gdiplus::FontFamily uiFF(L"Segoe UI");
    Gdiplus::Font f(&uiFF, 13, Gdiplus::FontStyleBold, Gdiplus::UnitPoint);
    Gdiplus::SolidBrush green(Gdiplus::Color(255, 80, 200, 120));
    Gdiplus::RectF r(px, py + ph * 0.35f, pw, 30);
    g.DrawString(L"\u2713 Verbunden!", -1, &f, r, &sf, &green);

    if (!status_text_.empty()) {
        Gdiplus::Font sf2(&uiFF, 9, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush gray(Gdiplus::Color(255, 160, 160, 160));
        std::wstring ws(status_text_.begin(), status_text_.end());
        Gdiplus::RectF sr(px, py + ph * 0.45f, pw, 30);
        g.DrawString(ws.c_str(), -1, &sf2, sr, &sf, &gray);
    }
}

void Win32Window::draw_streaming_screen(Gdiplus::Graphics& g) {
    if (render_cb_) {
        render_cb_();
    } else {
        Gdiplus::StringFormat sf;
        sf.SetAlignment(Gdiplus::StringAlignmentCenter);
        sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        float pw = (float)(rect_phone_.right - rect_phone_.left);
        float ph = (float)(rect_phone_.bottom - rect_phone_.top);
        Gdiplus::FontFamily uiFF(L"Segoe UI");
        Gdiplus::Font f(&uiFF, 10, Gdiplus::FontStyleRegular, Gdiplus::UnitPoint);
        Gdiplus::SolidBrush gray(Gdiplus::Color(255, 160, 160, 160));
        Gdiplus::RectF r((float)rect_phone_.left, (float)rect_phone_.top, pw, ph);
        g.DrawString(L"Warte auf Video-Stream...", -1, &f, r, &sf, &gray);
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
    return DefWindowProc(hwnd, msg, wp, lp);
}

LRESULT Win32Window::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        // Cave man remove window frame — no border artifacts
        if (wp == TRUE) return 0;
        break;
    case WM_TIMER:
        if (wp == 1) InvalidateRect(hwnd_, &rect_phone_, FALSE);
        return 0;
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { recalc_layout(); update_region(); }
        return 0;
    case WM_SIZING:
        handle_sizing(wp, lp); return TRUE;
    case WM_NCHITTEST:
        return handle_nchittest({GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
    case WM_LBUTTONDOWN: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        int btn = hit_test_button(pt);
        if (btn == 1) ShowWindow(hwnd_, SW_MINIMIZE);
        else if (btn == 2) toggle_max_height();
        else if (btn == 3) PostMessage(hwnd_, WM_CLOSE, 0, 0);
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
    case WM_MOUSEMOVE: {
        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
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
    case WM_MOUSELEAVE:
        if (hovered_button_ != -1 || start_button_hovered_) {
            hovered_button_ = -1; start_button_hovered_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_PAINT: handle_paint(); return 0;
    case WM_ERASEBKGND: return 1;
    }
    return DefWindowProc(hwnd_, msg, wp, lp);
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

} // namespace pm::window
#endif
