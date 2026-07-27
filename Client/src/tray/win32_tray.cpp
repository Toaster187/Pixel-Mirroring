#ifdef _WIN32
#include "win32_tray.h"
#include <iostream>

namespace pm::tray {

namespace {
constexpr UINT WM_TRAYICON = WM_APP + 100;
}

std::unique_ptr<ITray> create_tray() {
    return std::make_unique<Win32Tray>();
}

Win32Tray::Win32Tray() {}

Win32Tray::~Win32Tray() {
    hide();
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
    }
}

bool Win32Tray::create(const std::string& tooltip, std::function<void()> on_click) {
    m_on_click = std::move(on_click);
    HINSTANCE hinst = GetModuleHandle(nullptr);

    // Cave man register message-only window class
    WNDCLASSEXA wc = { sizeof(wc) };
    wc.lpfnWndProc = Win32Tray::window_proc;
    wc.hInstance = hinst;
    wc.lpszClassName = "PixelMirroringTrayMsgClass";
    RegisterClassExA(&wc);

    // Create message-only window
    m_hwnd = CreateWindowExA(0, wc.lpszClassName, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, this);
    if (!m_hwnd) {
        std::cerr << "[Tray] Failed to create message-only window" << std::endl;
        return false;
    }

    // Set up NOTIFYICONDATA
    m_nid.cbSize = sizeof(m_nid);
    m_nid.hWnd = m_hwnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;
    m_nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION); // Standard application icon

    // Convert UTF-8 tooltip to UTF-16
    MultiByteToWideChar(CP_UTF8, 0, tooltip.c_str(), -1, m_nid.szTip, sizeof(m_nid.szTip) / sizeof(wchar_t));

    return true;
}

void Win32Tray::show() {
    if (m_shown) return;
    if (Shell_NotifyIconW(NIM_ADD, &m_nid)) {
        m_shown = true;
    } else {
        std::cerr << "[Tray] NIM_ADD failed" << std::endl;
    }
}

void Win32Tray::hide() {
    if (!m_shown) return;
    if (Shell_NotifyIconW(NIM_DELETE, &m_nid)) {
        m_shown = false;
    } else {
        std::cerr << "[Tray] NIM_DELETE failed" << std::endl;
    }
}

LRESULT CALLBACK Win32Tray::window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Win32Tray* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lp);
        self = reinterpret_cast<Win32Tray*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<Win32Tray*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->handle_message(msg, wp, lp);
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

LRESULT Win32Tray::handle_message(UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TRAYICON) {
        UINT event_id = LOWORD(lp);
        if (event_id == WM_LBUTTONUP || event_id == WM_LBUTTONDBLCLK) {
            if (m_on_click) {
                m_on_click();
            }
            return 0;
        }
    }
    return DefWindowProc(m_hwnd, msg, wp, lp);
}

} // namespace pm::tray
#else
#include <memory>
namespace pm::tray {
class ITray;
std::unique_ptr<ITray> create_tray() {
    return nullptr;
}
}
#endif
