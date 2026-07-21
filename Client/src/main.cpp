#include <thread>
#include <random>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <optional>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#endif

#include "adb/adb_client.h"
#include "window/window_interface.h"
#include "stream/scrcpy_client.h"
#include "stream/video_decoder.h"
#include "stream/video_renderer.h"
#include "input/input_handler.h"
#include "network/network_scanner.h"
#include "tray/tray_interface.h"
#include "settings.h"
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace {
constexpr const char* ANDROID_PACKAGE = "dev.pixelmirroring.app";
constexpr const char* ANDROID_SERVICE = "dev.pixelmirroring.app/.service.MirroringService";
constexpr int ADB_TCP_PORT = 5555;
constexpr int HEARTBEAT_INTERVAL_MS = 5000;

#ifdef _WIN32
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo* pImageCodecInfo = (Gdiplus::ImageCodecInfo*)(malloc(size));
    if (pImageCodecInfo == nullptr) return -1;
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            free(pImageCodecInfo);
            return j;
        }
    }
    free(pImageCodecInfo);
    return -1;
}
#endif

struct SetupState {
    bool configured = false;
    std::string device_ip;
    std::string device_name;
};

// Cave man remember sun brightness before making screen dark
struct SavedBrightness {
    int brightness = -1;           // -1 = not saved
    int brightness_mode = -1;      // -1 = not saved, 0 = manual, 1 = auto
    std::string device_id;         // which phone this belong to
};

// Cave man read sun level from phone before smashing it to zero
SavedBrightness read_brightness(pm::adb::AdbClient& adb, const std::string& device_id) {
    SavedBrightness saved;
    saved.device_id = device_id;

    std::string brightness_str = adb.execute_shell_command(
        device_id, "settings get system screen_brightness");
    brightness_str.erase(brightness_str.find_last_not_of(" \n\r\t") + 1);
    try { saved.brightness = std::stoi(brightness_str); } catch (...) {}

    std::string mode_str = adb.execute_shell_command(
        device_id, "settings get system screen_brightness_mode");
    mode_str.erase(mode_str.find_last_not_of(" \n\r\t") + 1);
    try { saved.brightness_mode = std::stoi(mode_str); } catch (...) {}

    return saved;
}

// Cave man put sun brightness back to old level
void restore_brightness(pm::adb::AdbClient& adb, const SavedBrightness& saved) {
    if (saved.device_id.empty() || saved.brightness < 0) return;

    if (saved.brightness_mode >= 0) {
        adb.execute_shell_command(saved.device_id,
            "settings put system screen_brightness_mode " + std::to_string(saved.brightness_mode));
    }
    adb.execute_shell_command(saved.device_id,
        "settings put system screen_brightness " + std::to_string(saved.brightness));
}

class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~ScopeExit() { if (fn_) fn_(); }

private:
    std::function<void()> fn_;
};

std::string get_client_name() {
#ifdef _WIN32
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD size = sizeof(name);
    if (GetComputerNameA(name, &size) && size > 0) {
        return name;
    }
#endif
    const char* env_name = std::getenv("COMPUTERNAME");
    if (env_name && env_name[0] != '\0') return env_name;
    env_name = std::getenv("HOSTNAME");
    if (env_name && env_name[0] != '\0') return env_name;
    return "Desktop-PC";
}

#ifdef _WIN32
namespace {
    std::string g_pin_result;
    bool g_pin_done = false;
    WNDPROC g_old_edit_proc;
    
    LRESULT CALLBACK PinEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_KEYDOWN) {
            if (wp == VK_RETURN) {
                char buf[128] = {0};
                GetWindowTextA(hwnd, buf, sizeof(buf));
                g_pin_result = buf;
                g_pin_done = true;
                PostMessage(GetParent(hwnd), WM_NULL, 0, 0);
                return 0;
            } else if (wp == VK_ESCAPE) {
                g_pin_result = "";
                g_pin_done = true;
                PostMessage(GetParent(hwnd), WM_NULL, 0, 0);
                return 0;
            }
        }
        return CallWindowProcA(g_old_edit_proc, hwnd, msg, wp, lp);
    }
    
    LRESULT CALLBACK PinDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                HWND hEdit = CreateWindowExA(0, "EDIT", "", 
                    WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL | ES_CENTER,
                    20, 50, 160, 24, hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
                HFONT hFont = CreateFontA(18, 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET, 0,0,0,0, "Segoe UI");
                SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, 0);
                g_old_edit_proc = (WNDPROC)SetWindowLongPtrA(hEdit, GWLP_WNDPROC, (LONG_PTR)PinEditProc);
                SetFocus(hEdit);
                
                HRGN rgn = CreateRoundRectRgn(0, 0, 200, 110, 16, 16);
                SetWindowRgn(hwnd, rgn, TRUE);
                return 0;
            }
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC hdc = BeginPaint(hwnd, &ps);
                RECT rc; GetClientRect(hwnd, &rc);
                
                HBRUSH bg = CreateSolidBrush(RGB(35, 35, 35));
                FillRect(hdc, &rc, bg);
                DeleteObject(bg);
                
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(240, 240, 240));
                HFONT hFont = CreateFontA(16, 0,0,0, FW_BOLD, 0,0,0, DEFAULT_CHARSET, 0,0,0,0, "Segoe UI");
                HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
                RECT tr = {0, 15, 200, 40};
                DrawTextA(hdc, "PIN eingeben:", -1, &tr, DT_CENTER | DT_TOP);
                
                SetTextColor(hdc, RGB(150, 150, 150));
                HFONT smallFont = CreateFontA(12, 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET, 0,0,0,0, "Segoe UI");
                SelectObject(hdc, smallFont);
                RECT br = {0, 85, 200, 110};
                DrawTextA(hdc, "Enter (OK)  |  Esc (Abbrechen)", -1, &br, DT_CENTER | DT_TOP);
                
                SelectObject(hdc, oldFont);
                DeleteObject(hFont);
                DeleteObject(smallFont);
                EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_CTLCOLOREDIT: {
                HDC hdc = (HDC)wp;
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkColor(hdc, RGB(60, 60, 60));
                static HBRUSH hbr = CreateSolidBrush(RGB(60, 60, 60));
                return (LRESULT)hbr;
            }
            case WM_ACTIVATE: {
                if (LOWORD(wp) == WA_INACTIVE && !g_pin_done) {
                    g_pin_result = "";
                    g_pin_done = true;
                    PostMessage(hwnd, WM_NULL, 0, 0);
                }
                return 0;
            }
        }
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}
#endif

std::string prompt_user_for_pin(void* parent_hwnd = nullptr) {
    while (true) {
#ifdef _WIN32
        g_pin_result = "";
        HINSTANCE hi = GetModuleHandle(nullptr);
        WNDCLASSEXA wc = {sizeof(wc)};
        wc.lpfnWndProc = PinDialogProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = "PixelMirroringPinDialog";
        RegisterClassExA(&wc);
        
        HWND parent = (HWND)parent_hwnd;
        int px = 0, py = 0;
        if (parent) {
            RECT pr; GetWindowRect(parent, &pr);
            px = pr.left + (pr.right - pr.left) / 2 - 100;
            py = pr.top + (pr.bottom - pr.top) / 2 - 55;
        }
        
        HWND hwnd = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, "PIN", 
            WS_POPUP, px, py, 200, 110, parent, nullptr, hi, nullptr);
            
        if (parent) EnableWindow(parent, FALSE);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        
        g_pin_done = false;
        MSG msg;
        while (GetMessage(&msg, nullptr, 0, 0)) {
            if (g_pin_done) break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        if (parent) EnableWindow(parent, TRUE);
        DestroyWindow(hwnd);
        UnregisterClassA(wc.lpszClassName, hi);
        
        if (parent) {
            SetForegroundWindow(parent);
            SetFocus(parent);
        }
        
        std::string result = g_pin_result;
#else
        std::string command = "osascript -e 'display dialog \"PIN zum automatischen Entsperren eingeben (nur Ziffern):\" default answer \"\" with title \"PIN einrichten\"' -e 'text returned of result' 2>/dev/null";
        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) return "";
        char buffer[128];
        std::string result = "";
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
        pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t')) {
            result.pop_back();
        }
#endif

        if (result.empty()) return "";

        bool all_digits = true;
        for (char c : result) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                all_digits = false;
                break;
            }
        }

        if (all_digits && result.length() <= 16) {
            return result;
        }

#ifdef _WIN32
        MessageBoxA((HWND)parent_hwnd, "PIN darf nur aus Ziffern bestehen und maximal 16 Zeichen lang sein.", "Ungueltige Eingabe", MB_OK | MB_ICONERROR);
#endif
    }
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::filesystem::path get_config_dir() {
#ifndef PM_PORTABLE_BUILD
#ifdef _WIN32
    // CAVE MAN NO WRITE APPDATA FOR PORTABLE. ONLY EXE DIR.
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data && local_app_data[0] != '\0') {
        std::filesystem::path path = std::filesystem::path(local_app_data) / "PixelMirroring";
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        if (!ec) {
            return path;
        }
    }
#endif
#endif
    std::filesystem::path fallback = pm::adb::get_executable_dir();
    std::error_code ec;
    std::filesystem::create_directories(fallback, ec);
    return fallback;
}

std::filesystem::path get_setup_state_path() {
    return get_config_dir() / "setup_state.txt";
}

std::filesystem::path get_client_id_path() {
    return get_config_dir() / "client_id.txt";
}

SetupState load_setup_state() {
    SetupState state;
    std::ifstream file(get_setup_state_path());
    if (!file) {
        return state;
    }

    std::string line;
    while (std::getline(file, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto value = line.substr(eq + 1);
        if (key == "configured") {
            state.configured = value == "1";
        } else if (key == "device_ip") {
            state.device_ip = value;
        } else if (key == "device_name") {
            state.device_name = value;
        }
    }

    return state;
}

void save_setup_state(const SetupState& state) {
    std::ofstream file(get_setup_state_path(), std::ios::trunc);
    if (!file) {
        return;
    }

    file << "configured=" << (state.configured ? "1" : "0") << "\n";
    file << "device_ip=" << state.device_ip << "\n";
    file << "device_name=" << state.device_name << "\n";
}

void clear_setup_state() {
    std::error_code ec;
    std::filesystem::remove(get_setup_state_path(), ec);
}

std::optional<std::filesystem::path> find_android_apk() {
    std::filesystem::path exe_dir = pm::adb::get_executable_dir();
    std::vector<std::filesystem::path> roots;
    roots.push_back(exe_dir);

    auto current = exe_dir;
    for (int i = 0; i < 6; ++i) {
        current = current.parent_path();
        if (current.empty()) break;
        roots.push_back(current);
    }

    for (const auto& root : roots) {
        std::vector<std::filesystem::path> candidates = {
            root / "PixelMirroring.apk",
            root / "app-debug.apk",
            root / "Android" / "app" / "build" / "outputs" / "apk" / "debug" / "app-debug.apk",
            root / "Android" / "app" / "build" / "intermediates" / "apk" / "debug" / "app-debug.apk"
        };

        for (const auto& candidate : candidates) {
            if (path_exists(candidate)) {
                return candidate;
            }
        }
    }

    return std::nullopt;
}

std::optional<pm::adb::Device> find_usb_device(
    const std::vector<pm::adb::Device>& devices,
    const std::string& state = ""
) {
    for (const auto& device : devices) {
        if (device.is_usb() && (state.empty() || device.state == state)) {
            return device;
        }
    }
    return std::nullopt;
}

std::optional<pm::adb::Device> find_tcp_device(
    const std::vector<pm::adb::Device>& devices,
    const std::string& ip
) {
    for (const auto& device : devices) {
        if (device.is_tcp() && device.state == "device" && device.id.find(ip) != std::string::npos) {
            return device;
        }
    }
    return std::nullopt;
}

std::optional<pm::adb::Device> wait_for_tcp_device(
    pm::adb::AdbClient& adb,
    const std::string& ip,
    std::atomic<bool>& should_stop
);

bool start_stream(
    pm::window::IWindow& window,
    pm::stream::ScrcpyClient& scrcpy,
    pm::stream::VideoRenderer& renderer,
    pm::input::InputHandler& input,
    const std::string& device_id,
    SavedBrightness* out_saved_brightness
);

bool unlock_device_if_needed(const std::string& device_id, pm::window::IWindow* window = nullptr);

std::optional<pm::adb::Device> wait_for_usb_authorization(
    pm::adb::AdbClient& adb,
    pm::window::IWindow& window,
    std::atomic<bool>& should_stop
) {
    bool requested_reconnect = false;
    for (int i = 0; i < 60 && !should_stop; ++i) {
        auto devices = adb.get_devices();
        if (auto ready = find_usb_device(devices, "device")) {
            return ready;
        }

        auto usb = find_usb_device(devices);
        if (!usb) {
            window.set_status_text("Warte auf USB-Geraet...");
            requested_reconnect = false; // reset state
        } else {
            if (usb->state == "unauthorized") {
                window.set_status_text("Bitte USB-Debugging auf dem Handy erlauben.");
                if (!requested_reconnect) {
                    adb.reconnect_offline();
                    requested_reconnect = true;
                }
            } else {
                window.set_status_text("Warte auf USB-Geraet: " + usb->state);
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return std::nullopt;
}

std::optional<pm::adb::Device> connect_configured_device(
    pm::adb::AdbClient& adb,
    pm::window::IWindow& window,
    const SetupState& setup_state,
    const std::string& client_id,
    const std::string& client_name,
    std::atomic<bool>& should_stop
) {
    pm::network::NetworkScanner scanner;

    // Ugg! Maybe the cave connection from before is still warm — no need to wake ADB again.
    if (!setup_state.device_ip.empty()) {
        auto devices = adb.get_connected_devices();
        if (auto tcp = find_tcp_device(devices, setup_state.device_ip)) {
            return tcp;
        }
    }

    if (should_stop) return std::nullopt;

    // Ugg! ADB is off cold — poke the phone's discovery server to wake it up first.
    if (!setup_state.device_ip.empty()) {
        window.set_status_text("Wecke Geraet " + setup_state.device_ip + "...");
        if (auto discovered = scanner.request_connect(setup_state.device_ip, client_id, client_name)) {
            window.set_status_text("Verbinde mit " + discovered->ip + "...");
            if (adb.connect_device(discovered->ip, discovered->adb_port)) {
                if (auto tcp = wait_for_tcp_device(adb, discovered->ip, should_stop)) {
                    return tcp;
                }
            }
        }
    }

    if (should_stop) return std::nullopt;

    window.set_status_text("Suche eingerichtetes Geraet im Netzwerk...");
    auto discovered = scanner.discover_and_connect(client_id, client_name);
    if (!discovered || should_stop) {
        return std::nullopt;
    }

    window.set_status_text("Gefunden: " + discovered->device_name);
    if (!adb.connect_device(discovered->ip, discovered->adb_port)) {
        return std::nullopt;
    }

    return wait_for_tcp_device(adb, discovered->ip, should_stop);
}

bool run_first_time_setup(
    pm::adb::AdbClient& adb,
    pm::window::IWindow& window,
    pm::stream::ScrcpyClient& scrcpy,
    pm::stream::VideoRenderer& renderer,
    pm::input::InputHandler& input,
    std::atomic<bool>& should_stop,
    SavedBrightness* out_saved_brightness
) {
    clear_setup_state();

    window.post_task([&window]() { window.set_status_text("Pruefe USB-Verbindung..."); });
    auto usb_device = wait_for_usb_authorization(adb, window, should_stop);
    if (!usb_device || should_stop) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SETUP);
            window.set_status_text("Kein USB-Geraet bereit. Bitte erneut verbinden.");
        });
        return false;
    }

    // Cave man check if app already on phone before trying install.
    bool app_installed = adb.is_app_installed(usb_device->id, ANDROID_PACKAGE);

    if (!app_installed) {
        window.post_task([&window]() { window.set_status_text("Android-App wird installiert..."); });
        auto apk_path = find_android_apk();
        if (!apk_path) {
            window.post_task([&window]() {
                window.set_app_state(pm::window::AppState::SETUP);
                window.set_status_text("Android-App fehlt im PC-Paket. Bitte APK manuell installieren.");
            });
            return false;
        }

        if (!adb.install_app(usb_device->id, apk_path->string())) {
            window.post_task([&window]() {
                window.set_app_state(pm::window::AppState::SETUP);
                window.set_status_text("Android-App konnte nicht installiert werden.");
            });
            return false;
        }
    } else {
        // App already on phone, skip APK install.
    }

    // Cave man check if permission already granted before yelling at phone.
    bool has_perm = adb.has_permission(usb_device->id, ANDROID_PACKAGE, "android.permission.WRITE_SECURE_SETTINGS");

    if (!has_perm) {
        window.post_task([&window]() { window.set_status_text("Berechtigungen werden gesetzt..."); });
        if (!adb.grant_secure_settings(usb_device->id)) {
            window.post_task([&window]() {
                window.set_app_state(pm::window::AppState::SETUP);
                window.set_status_text("WRITE_SECURE_SETTINGS konnte nicht gesetzt werden.");
            });
            return false;
        }
    } else {
        // Permission already granted, skip.
    }

    window.post_task([&window]() { window.set_status_text("Android-App wird gestartet..."); });
    adb.start_app(usb_device->id, ANDROID_PACKAGE);
    adb.start_service(usb_device->id, ANDROID_SERVICE);

    const std::string device_ip = adb.get_device_ip(usb_device->id);
    if (device_ip.empty()) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SETUP);
            window.set_status_text("Keine WLAN-IP gefunden. Bitte WLAN pruefen.");
        });
        return false;
    }

    window.post_task([&window]() { window.set_status_text("ADB ueber WLAN wird aktiviert..."); });
    if (!adb.enable_tcpip(usb_device->id, ADB_TCP_PORT)) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SETUP);
            window.set_status_text("WLAN-ADB konnte nicht aktiviert werden.");
        });
        return false;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    window.post_task([&window, device_ip]() { window.set_status_text("Verbinde mit " + device_ip + "..."); });
    if (!adb.connect_device(device_ip, ADB_TCP_PORT)) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SETUP);
            window.set_status_text("ADB-Verbindung per WLAN fehlgeschlagen.");
        });
        return false;
    }

    auto tcp_device = wait_for_tcp_device(adb, device_ip, should_stop);
    if (!tcp_device || should_stop) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SETUP);
            window.set_status_text("WLAN-ADB ist noch nicht bereit.");
        });
        return false;
    }

    SetupState setup_state;
    setup_state.configured = true;
    setup_state.device_ip = device_ip;
    setup_state.device_name = usb_device->model.empty() ? "Android" : usb_device->model;

    window.post_task([&window, device_ip]() {
        window.set_app_state(pm::window::AppState::CONNECTED);
        window.set_status_text("Verbunden: " + device_ip);
    });
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!unlock_device_if_needed(tcp_device->id, &window)) {
        clear_setup_state();
        window.post_task([&window]() {
#ifdef _WIN32
            PostMessageA((HWND)window.get_native_handle(), WM_CLOSE, 0, 0);
#else
            exit(0);
#endif
        });
        return false;
    }
    if (!start_stream(window, scrcpy, renderer, input, tcp_device->id, out_saved_brightness)) {
        clear_setup_state();
        return false;
    }

    save_setup_state(setup_state);
    return true;
}

std::optional<pm::adb::Device> wait_for_tcp_device(
    pm::adb::AdbClient& adb,
    const std::string& ip,
    std::atomic<bool>& should_stop
) {
    for (int i = 0; i < 20 && !should_stop; ++i) {
        auto devices = adb.get_connected_devices();
        if (auto tcp = find_tcp_device(devices, ip)) {
            return tcp;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return std::nullopt;
}

bool start_stream(
    pm::window::IWindow& window,
    pm::stream::ScrcpyClient& scrcpy,
    pm::stream::VideoRenderer& renderer,
    pm::input::InputHandler& input,
    const std::string& device_id,
    SavedBrightness* out_saved_brightness
) {
    window.post_task([&window]() { window.set_status_text("Starte Video-Stream..."); });
    pm::stream::ScrcpyClient::Config config;
    config.device_id = device_id;

    // MEOW. WELD SETTINGS TO CONFIG.
    pm::Settings settings = pm::load_settings();
    config.max_fps = settings.max_fps;
    config.max_size = settings.max_size;
    config.lowest_brightness = settings.m_lowest_brightness;

    if (settings.m_lowest_brightness) {
        // CAVE MAN DECREASE SUN SHINE SO SMARTPHONE SCREEN IS DARKEST SHADE OF GREY
        pm::adb::AdbClient adb;
        if (out_saved_brightness && out_saved_brightness->brightness < 0) {
            *out_saved_brightness = read_brightness(adb, device_id);
        }
        adb.execute_shell_command(device_id, "settings put system screen_brightness_mode 0");
        adb.execute_shell_command(device_id, "settings put system screen_brightness 0");
    }

    if (!renderer.init(window.get_native_handle())) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SCANNING);
            window.set_status_text("Stream-Fehler: Renderer init fehlgeschlagen.");
        });
        return false;
    }
    scrcpy.set_frame_callback([&](AVFrame* frame) {
        renderer.render_frame(frame);
    });
    window.set_render_callback([&](SDL_Renderer* renderer_ptr, int x, int y, int w, int h) {
        renderer.paint(renderer_ptr, x, y, w, h);
    });

    if (!scrcpy.start(config)) {
        window.set_app_state(pm::window::AppState::SCANNING);
        window.set_status_text("Stream konnte nicht gestartet werden.");
        return false;
    }

    int w = scrcpy.video_width();
    int h = scrcpy.video_height();
    if (w <= 0 || h <= 0) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SCANNING);
            window.set_status_text("Stream-Fehler: Ungueltige Video-Dimensionen.");
        });
        scrcpy.stop();
        return false;
    }

    input.set_device_size(w, h);
    window.post_task([&window, w, h]() {
        window.set_aspect_ratio((double)w / (double)h);
        window.set_orientation(w > h);
        window.set_app_state(pm::window::AppState::STREAMING);
    });
    return true;
}

bool unlock_device_if_needed(const std::string& device_id, pm::window::IWindow* window) {
    pm::Settings settings = pm::load_settings();
    if (settings.m_pin.empty()) return true;
    
    pm::adb::AdbClient adb;
    
    // Cave man check if phone already open
    std::string trust_state = adb.execute_shell_command(device_id, "dumpsys trust");
    size_t current_pos = trust_state.find("(current):");
    if (current_pos != std::string::npos) {
        size_t line_end = trust_state.find("\n", current_pos);
        std::string current_user_line = (line_end == std::string::npos)
            ? trust_state.substr(current_pos)
            : trust_state.substr(current_pos, line_end - current_pos);
        if (current_user_line.find("deviceLocked=0") != std::string::npos) {
            // Cave man see screen already open, do not key smash!
            return true;
        }
    }
    
    // Cave man check power saving mode before PIN
    std::string low_power_state = adb.execute_shell_command(device_id, "settings get global low_power");
    low_power_state.erase(low_power_state.find_last_not_of(" \n\r\t") + 1);
    if (low_power_state == "1" || low_power_state == "true") {
        bool disable_power_saving = false;
        if (window) {
#ifdef _WIN32
            int answer = MessageBoxA(
                (HWND)window->get_native_handle(),
                "Das Geraet befindet sich im Energiesparmodus. Die PIN-Eingabe schlaegt dadurch fehl.\n\nDarf die App den Energiesparmodus deaktivieren?",
                "Energiesparmodus",
                MB_YESNO | MB_ICONQUESTION
            );
            if (answer == IDYES) disable_power_saving = true;
#endif
        }
        if (disable_power_saving) {
            adb.execute_shell_command(device_id, "settings put global low_power 0");
            adb.execute_shell_command(device_id, "cmd power set-mode 0");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } else {
            return false;
        }
    }
    
    // Cave man determine delays. Default is 0ms, compat mode is wakeup=400ms, fingerprint=800ms
    int wakeup_delay = 0;
    int fingerprint_delay = 0;
    if (settings.m_compatibility_mode) {
        wakeup_delay = 400;
        fingerprint_delay = 800;
    }
    
    // Cave man wake screen
    adb.execute_shell_command(device_id, "input keyevent 224");
    if (wakeup_delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(wakeup_delay));
    }
    
    // Cave man hit ENTER to dismiss fingerprint, show PIN pad
    adb.execute_shell_command(device_id, "input keyevent 66");
    if (fingerprint_delay > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(fingerprint_delay));
    }
    
    // Cave man blast all PIN digits and final ENTER fast in one single stone throw
    std::string pin_command = "input keyevent";
    for (char c : settings.m_pin) {
        int keycode = 7 + (c - '0');
        pin_command += " " + std::to_string(keycode);
    }
    pin_command += " 66"; // Confirm PIN
    adb.execute_shell_command(device_id, pin_command);
    
    // Cave man wait for unlock animation
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    return true;
}

}

static int app_main() {
#ifdef _WIN32
    // Cave man check for single instance!
    HANDLE mutex = CreateMutexA(nullptr, TRUE, "PixelMirroringSingleInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing_hwnd = FindWindowA("PixelMirroringWindowClass", nullptr);
        if (existing_hwnd) {
            UINT restore_msg = RegisterWindowMessageA("PixelMirroringRestoreMsg");
            PostMessageA(existing_hwnd, restore_msg, 0, 0);
            
            if (IsIconic(existing_hwnd)) {
                ShowWindow(existing_hwnd, SW_RESTORE);
            }
            SetForegroundWindow(existing_hwnd);
        }
        CloseHandle(mutex);
        return 0;
    }

    SetProcessDPIAware();
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);
#endif

    auto window = pm::window::create_window(340, 604, "Pixel Mirroring");
    if (!window->create()) {
#ifdef _WIN32
        Gdiplus::GdiplusShutdown(gdiplusToken);
#endif
        return 1;
    }

    auto tray = pm::tray::create_tray();

    window->set_aspect_ratio(340.0 / 604.0);
    window->set_app_state(pm::window::AppState::SETUP);
    window->set_status_text("");

    // MEOW. INITIAL LOAD SETTINGS AND SYNCHRONIZE CHECKBOXES.
    pm::Settings initial_settings = pm::load_settings();
    window->set_fps_limited(initial_settings.max_fps == 30);
    window->set_resolution_limited(initial_settings.max_size == 720);
    window->set_compatibility_mode(initial_settings.m_compatibility_mode);
    window->set_lowest_brightness(initial_settings.m_lowest_brightness);
    window->set_capture_send_to_phone(initial_settings.m_send_captures_to_phone);

    SavedBrightness saved_brightness; // Cave man remember phone sun level
    std::atomic<bool> should_stop{false};
    pm::stream::ScrcpyClient scrcpy;
    scrcpy.set_disconnect_callback([w = window.get()]() {
        w->post_task([w]() {
#ifdef _WIN32
            PostMessageA((HWND)w->get_native_handle(), WM_CLOSE, 0, 0);
#else
            exit(0);
#endif
        });
    });
    pm::stream::VideoRenderer renderer;
    pm::input::InputHandler input(&scrcpy);

    auto run_on_device = [&scrcpy](auto action) {
        if (!scrcpy.is_running()) return;
        std::string device_id = scrcpy.get_device_id();
        if (device_id.empty()) return;
        std::thread([device_id, action]() {
            action(device_id);
        }).detach();
    };

    auto publish_capture_to_phone = [&scrcpy, w = window.get()](const std::filesystem::path& path) {
        if (!scrcpy.is_running() || path.empty()) return;
        const std::string device_id = scrcpy.get_device_id();
        if (device_id.empty()) return;
        std::thread([device_id, path, w]() {
            pm::adb::AdbClient adb;
            const std::string remote_dir = "/sdcard/Pictures/PixelMirroring";
            const std::string remote_path = remote_dir + "/" + path.filename().string();
            adb.execute_shell_command(device_id, "mkdir -p " + remote_dir);
            const bool sent = adb.push_file(device_id, path.string(), remote_path);
            if (sent) {
                // Trigger our custom BroadcastReceiver in the companion app to scan the file via MediaScannerConnection (reliable on Android 11+)
                adb.execute_shell_command(device_id,
                    "am broadcast -a dev.pixelmirroring.app.SCAN_FILE -e path \"" + remote_path + "\"");
                // Fallback: Direct content provider insert for modern Android
                adb.execute_shell_command(device_id,
                    "content insert --uri content://media/external/images/media --bind _data:s:\"" + remote_path + "\"");
                // Legacy intent broadcast for older Android versions
                adb.execute_shell_command(device_id,
                    "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d file://" + remote_path);
            }
            w->post_task([w, sent]() {
                w->set_status_text(sent
                    ? "Aufnahme wurde ans Handy gesendet."
                    : "Aufnahme konnte nicht ans Handy gesendet werden.");
            });
        }).detach();
    };

    // MEOW. WIRE CONTEXT MENU CALLBACK.
    window->set_menu_callback([&](pm::window::MenuAction action) {
        pm::Settings current_settings = pm::load_settings();
        switch (action) {
            case pm::window::MenuAction::FACTORY_RESET: {
                clear_setup_state();
                std::error_code ec;
                std::filesystem::remove(get_client_id_path(), ec);
                window->post_task([w = window.get()]() {
                    w->set_app_state(pm::window::AppState::SETUP);
                    w->set_status_text("Werkseinstellungen gesetzt.");
                });
                break;
            }
            case pm::window::MenuAction::TOGGLE_FPS_LIMIT: {
                current_settings.max_fps = (current_settings.max_fps == 30) ? 60 : 30;
                pm::save_settings(current_settings);
                window->set_fps_limited(current_settings.max_fps == 30);
                break;
            }
            case pm::window::MenuAction::TOGGLE_RESOLUTION_LIMIT: {
                current_settings.max_size = (current_settings.max_size == 720) ? 0 : 720;
                pm::save_settings(current_settings);
                window->set_resolution_limited(current_settings.max_size == 720);
                break;
            }
            case pm::window::MenuAction::TOGGLE_COMPATIBILITY_MODE: {
                current_settings.m_compatibility_mode = !current_settings.m_compatibility_mode;
                pm::save_settings(current_settings);
                window->set_compatibility_mode(current_settings.m_compatibility_mode);
                break;
            }
            case pm::window::MenuAction::TOGGLE_LOWEST_BRIGHTNESS: {
                current_settings.m_lowest_brightness = !current_settings.m_lowest_brightness;
                pm::save_settings(current_settings);
                window->set_lowest_brightness(current_settings.m_lowest_brightness);
                break;
            }
            case pm::window::MenuAction::TAKE_SCREENSHOT: {
                if (!scrcpy.is_running()) {
                    window->set_status_text("Bildschirmfoto nur bei aktivem Stream moeglich.");
                    break;
                }
                auto screenshot = renderer.take_screenshot();
                if (!screenshot) {
                    window->set_status_text("Bildschirmfoto konnte nicht erstellt werden.");
                    break;
                }
#ifdef _WIN32
                // Convert BMP to PNG
                std::wstring bmpPath = screenshot->wstring();
                Gdiplus::Bitmap* bmp = new Gdiplus::Bitmap(bmpPath.c_str());
                if (bmp && bmp->GetLastStatus() == Gdiplus::Ok) {
                    CLSID pngClsid;
                    if (GetEncoderClsid(L"image/png", &pngClsid) != -1) {
                        std::filesystem::path pngPath = *screenshot;
                        pngPath.replace_extension(".png");
                        if (bmp->Save(pngPath.wstring().c_str(), &pngClsid, nullptr) == Gdiplus::Ok) {
                            delete bmp;
                            std::filesystem::remove(*screenshot);
                            screenshot = pngPath;
                        } else {
                            delete bmp;
                        }
                    } else {
                        delete bmp;
                    }
                } else {
                    if (bmp) delete bmp;
                }
#endif
                window->trigger_screenshot_flash();
                window->set_status_text("Bildschirmfoto gespeichert: " + screenshot->filename().string());
                if (current_settings.m_send_captures_to_phone) {
                    publish_capture_to_phone(*screenshot);
                }
                break;
            }
            case pm::window::MenuAction::TOGGLE_RECORDING: {
                if (renderer.is_recording()) {
                    auto recording = renderer.stop_recording();
                    window->set_recording(false);
                    if (!recording) {
                        window->set_status_text("Videoaufnahme enthielt keine speicherbaren Frames.");
                        break;
                    }
                    window->set_status_text("Videoaufnahme gespeichert: " + recording->filename().string());
                    if (current_settings.m_send_captures_to_phone) {
                        publish_capture_to_phone(*recording);
                    }
                } else if (scrcpy.is_running() && renderer.start_recording()) {
                    window->set_recording(true);
                    window->set_status_text("Videoaufnahme laeuft.");
                } else {
                    window->set_status_text("Videoaufnahme braucht einen geladenen Stream-Frame.");
                }
                break;
            }
            case pm::window::MenuAction::TOGGLE_SEND_CAPTURES_TO_PHONE: {
                current_settings.m_send_captures_to_phone = !current_settings.m_send_captures_to_phone;
                pm::save_settings(current_settings);
                window->set_capture_send_to_phone(current_settings.m_send_captures_to_phone);
                break;
            }
            case pm::window::MenuAction::SET_PIN: {
                std::string new_pin = prompt_user_for_pin(window->get_native_handle());
                if (!new_pin.empty()) {
                    current_settings.m_pin = new_pin;
                    pm::save_settings(current_settings);
                    window->set_status_text("PIN erfolgreich gespeichert.");
                } else {
#ifdef _WIN32
                    int answer = MessageBoxA(
                        (HWND)window->get_native_handle(),
                        "Moechtest du die gespeicherte PIN loeschen?",
                        "PIN loeschen",
                        MB_YESNO | MB_ICONQUESTION
                    );
                    if (answer == IDYES) {
                        current_settings.m_pin = "";
                        pm::save_settings(current_settings);
                        window->set_status_text("PIN geloescht.");
                    }
#else
                    current_settings.m_pin = "";
                    pm::save_settings(current_settings);
                    window->set_status_text("PIN geloescht.");
#endif
                }
                break;
            }
            case pm::window::MenuAction::UNLOCK_DEVICE: {
                if (current_settings.m_pin.empty()) {
                    window->set_status_text("PIN nicht eingerichtet.");
                    break;
                }
                run_on_device([w = window.get()](const std::string& id) {
                    unlock_device_if_needed(id, w);
                });
                break;
            }
            case pm::window::MenuAction::LOCK_DEVICE: {
                run_on_device([](const std::string& id) {
                    // Cave man lock screen and turn off light
                    pm::adb::AdbClient adb;
                    adb.execute_shell_command(id, "input keyevent 223");
                });
                break;
            }
        }
    });

    std::function<void(bool)> start_connection;

    auto do_restore = [&]() {
        window->post_task([&]() {
            if (!window->is_visible()) {
                window->show();
                if (tray) {
                    tray->hide();
                }
            }
#ifdef _WIN32
            HWND hw = (HWND)window->get_native_handle();
            if (IsIconic(hw)) ShowWindow(hw, SW_RESTORE);
            SetForegroundWindow(hw);
#endif
            // Cave man wake and unlock phone on restore
            if (scrcpy.is_running()) {
                run_on_device([w = window.get()](const std::string& id) {
                    unlock_device_if_needed(id, w);
                });
            } else {
                window->set_app_state(pm::window::AppState::SCANNING);
                window->set_status_text("Starte neue Verbindung...");
                start_connection(true);
            }
        });
    };

    window->set_restore_callback(do_restore);

    if (tray) {
        if (!tray->create("Pixel Mirroring", do_restore)) {
#ifdef _WIN32
            MessageBoxA(nullptr, "Tray-Icon konnte nicht erstellt werden.", "Fehler", MB_OK | MB_ICONERROR);
#endif
            tray.reset();
        }
    }

    window->set_video_viewport_callback([&](int x, int y, int w, int h) {
        renderer.update_viewport(x, y, w, h);
    });
    window->set_pointer_callback([&](pm::window::PointerAction action, int x, int y, int w, int h) {
        input.handle_pointer(action, x, y, w, h);
    });
    window->set_key_callback([&](int action, int keycode) {
        // key down/up. cave man click keys.
        if (action == 0) {
            input.handle_key_down(keycode);
        } else {
            input.handle_key_up(keycode);
        }
    });
    window->set_text_callback([&](const std::string& text) {
        // text write. cave man write words.
        input.handle_text(text);
    });
    window->set_scroll_callback([&](int x, int y, int w, int h, float hscroll, float vscroll) {
        // scroll wheel. cave man scroll screen.
        input.handle_scroll(x, y, w, h, hscroll, vscroll);
    });

    scrcpy.set_device_clipboard_callback([w = window.get()](const std::string& text) {
        w->post_task([w, text]() {
            w->set_pc_clipboard(text);
        });
    });
    window->set_os_clipboard_update_callback([&scrcpy](const std::string& text) {
        if (scrcpy.is_running()) {
            scrcpy.inject_set_clipboard(text);
        }
    });
    std::thread connection_thread;
    std::atomic<bool> connection_running{false};
    std::thread screen_poll_thread;
    std::atomic<bool> stop_screen_poll{false};
    std::thread heartbeat_thread;
    std::atomic<bool> stop_heartbeat{false};
    const std::string client_name = get_client_name();

    std::string client_id;
    std::string client_id_path = get_client_id_path().string();
    FILE* f = fopen(client_id_path.c_str(), "r");
    if (f) {
        char buf[256];
        if (fgets(buf, sizeof(buf), f)) {
            client_id = buf;
            if (!client_id.empty() && client_id.back() == '\n') client_id.pop_back();
        }
        fclose(f);
    }
    if (client_id.empty()) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(100000, 999999);
        client_id = "desktop-client-" + std::to_string(distrib(gen));
        FILE* fw = fopen(client_id_path.c_str(), "w");
        if (fw) { fputs(client_id.c_str(), fw); fclose(fw); }
    }

    auto stop_heartbeat_thread = [&]() {
        stop_heartbeat = true;
        if (heartbeat_thread.joinable()) {
            heartbeat_thread.join();
        }
    };
    auto start_heartbeat = [&](const std::string& ip) {
        stop_heartbeat_thread();
        stop_heartbeat = false;
        heartbeat_thread = std::thread([&, ip]() {
            // Cave man poke phone every 5s so phone knows PC still alive
            // Heartbeat only keeps phone ADB awake — disconnect detection is
            // handled by scrcpy video stream recv() failing
            pm::network::NetworkScanner hb_scanner;
            while (!should_stop && !stop_heartbeat && scrcpy.is_running()) {
                hb_scanner.send_heartbeat(ip, client_id, client_name);
                for (int i = 0; i < HEARTBEAT_INTERVAL_MS / 100
                     && !should_stop && !stop_heartbeat && scrcpy.is_running(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
    };

    auto stop_screen_poll_thread = [&]() {
        stop_screen_poll = true;
        if (screen_poll_thread.joinable()) {
            screen_poll_thread.join();
        }
    };

    auto start_screen_poll = [&](const std::string& poll_ip, const std::string& device_id) {
        stop_screen_poll_thread();
        stop_screen_poll = false;

        screen_poll_thread = std::thread([&, poll_ip, device_id]() {
            pm::adb::AdbClient poll_adb;
            bool last_screen_on = true;
            
            while (!should_stop && !stop_screen_poll) {
                bool screen_on = true; // assume on unless check confirms off

                // 1. Try HTTP /screen endpoint (fastest & most accurate)
                if (!poll_ip.empty()) {
                    httplib::Client cli(poll_ip, 18294);
                    cli.set_connection_timeout(0, 500000); // 500ms
                    cli.set_read_timeout(0, 500000);       // 500ms
                    if (auto res = cli.Get("/screen")) {
                        if (res->status == 200) {
                            if (res->body.find("\"screenOn\":false") != std::string::npos ||
                                res->body.find("\"screenOn\": false") != std::string::npos) {
                                screen_on = false;
                            }
                        }
                    }
                }

                // 2. ADB shell check fallback (broader pattern matching)
                if (screen_on && !device_id.empty()) {
                    // Cave man ask PowerManager instead. dumpsys display gives history log of past OFF events!
                    std::string power_state = poll_adb.execute_shell_command(
                        device_id, "dumpsys power | grep -iE 'mInteractive|mIsInteractive'");
                    if (!power_state.empty()) {
                        if (power_state.find("false") != std::string::npos ||
                            power_state.find("OFF") != std::string::npos) {
                            screen_on = false;
                        }
                    }
                }

                if (screen_on != last_screen_on) {
                    last_screen_on = screen_on;
                    
                    if (!screen_on) {
                        // Cave man put sun brightness back when screen goes off
                        if (saved_brightness.brightness >= 0) {
                            pm::adb::AdbClient restore_adb;
                            restore_brightness(restore_adb, saved_brightness);
                            saved_brightness = {}; // Cave man forget — already restored
                        }
                        
                        window->post_task([&]() {
                            // Cave man hide window to tray when phone screen off — no black screen!
                            if (window->is_visible()) {
                                window->hide();
                                if (tray) tray->show();
                            }
                            window->set_app_state(pm::window::AppState::SETUP);
                        });
                        // Cave man stop everything to save battery when screen off
                        if (scrcpy.is_running()) {
                            scrcpy.stop();
                        }
                        break; // Kill the poll thread entirely! Fully disconnected.
                    }
                }
                
                // Cave man sleep 500ms before next poll
                for (int i = 0; i < 5 && !should_stop && !stop_screen_poll; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
    };

    start_connection = [&](bool automatic) {
        if (connection_running) return;

        stop_screen_poll_thread();
        stop_heartbeat_thread();

        if (connection_thread.joinable()) {
            connection_thread.join();
        }

        connection_thread = std::thread([&, automatic]() {
            connection_running = true;
            ScopeExit mark_done([&]() { connection_running = false; });
            pm::adb::AdbClient adb;

            // Ugg wake ADB first. No pretend network magic.
            window->post_task([w = window.get()]() { w->set_status_text("ADB wird gestartet..."); });
            adb.init();

            SetupState setup_state = load_setup_state();
            if (!setup_state.configured) {
                if (run_first_time_setup(adb, *window, scrcpy, renderer, input, should_stop, &saved_brightness)) {
                    auto new_state = load_setup_state();
                    if (new_state.configured && !new_state.device_ip.empty()) {
                        start_heartbeat(new_state.device_ip);
                        start_screen_poll(new_state.device_ip, new_state.device_ip + ":5555");
                    }
                }
                return;
            }

            auto tcp_device = connect_configured_device(
                adb,
                *window,
                setup_state,
                client_id,
                client_name,
                should_stop
            );
            if (!tcp_device || should_stop) {
                window->post_task([w = window.get(), automatic]() {
                    w->set_app_state(pm::window::AppState::SETUP);
                    w->set_status_text(automatic
                        ? "Geraet nicht erreichbar. Verbinden fuer erneuten Versuch."
                        : "Neueinrichtung per USB beim naechsten Verbinden.");
                });
                if (!automatic) {
                    // Cave man give up on wireless cave path. Clear setup stone
                    // so next button press goes full USB setup with app install.
                    clear_setup_state();
                }
                return;
            }

            std::string name = setup_state.device_name;
            window->post_task([w = window.get(), name]() {
                w->set_app_state(pm::window::AppState::CONNECTED);
                w->set_status_text(name.empty() ? "Verbunden" : name);
            });
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!unlock_device_if_needed(tcp_device->id, window.get())) {
                window->post_task([w = window.get()]() {
#ifdef _WIN32
                    PostMessageA((HWND)w->get_native_handle(), WM_CLOSE, 0, 0);
#else
                    exit(0);
#endif
                });
                return;
            }
            if (start_stream(*window, scrcpy, renderer, input, tcp_device->id, &saved_brightness)) {
                std::string hb_ip = tcp_device->id.substr(0, tcp_device->id.rfind(':'));
                start_heartbeat(hb_ip);
                start_screen_poll(hb_ip, tcp_device->id);
            } else if (!should_stop) {
                window->post_task([w = window.get()]() {
                    w->set_app_state(pm::window::AppState::SETUP);
                    w->set_status_text("Stream fehlgeschlagen.");
                });
            }
        });
    };

    window->set_start_callback([&]() {
        start_connection(false);
    });

    window->show();
    if (load_setup_state().configured) {
        window->set_app_state(pm::window::AppState::SCANNING);
        window->set_status_text("Verbinde automatisch...");
        start_connection(true);
    }
    window->process_messages();

    should_stop = true;
    stop_screen_poll = true;
    stop_heartbeat = true;

    // Cave man put sun brightness back before leaving cave, while tunnel still strong
    if (saved_brightness.brightness >= 0) {
        pm::adb::AdbClient restore_adb;
        restore_brightness(restore_adb, saved_brightness);
    }

    scrcpy.stop();
    if (connection_thread.joinable()) connection_thread.join();
    if (screen_poll_thread.joinable()) screen_poll_thread.join();
    if (heartbeat_thread.joinable()) heartbeat_thread.join();

    if (tray) tray->hide();

#ifdef _WIN32
    Gdiplus::GdiplusShutdown(gdiplusToken);
    CloseHandle(mutex);
#endif
    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return app_main();
}
#else
int main(int argc, char* argv[]) {
    return app_main();
}
#endif
