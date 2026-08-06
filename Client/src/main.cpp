#include <thread>
#include <future>
#include <random>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
#include "util/encoding.h"
#include "settings.h"
#include <httplib.h>
#include <nlohmann/json.hpp>

namespace {
constexpr const char* ANDROID_PACKAGE = "dev.pixelmirroring.app";
constexpr const char* ANDROID_SERVICE = "dev.pixelmirroring.app/.service.MirroringService";
constexpr int ADB_TCP_PORT = 5555;
constexpr int HEARTBEAT_INTERVAL_MS = 5000;
// Where dropped files land on the phone. Every Android file manager knows this path.
constexpr const char* DROP_TARGET_DIR = "/sdcard/Download";

// Both conversions between file names and UTF-8 used to exist twice, once here and
// once in pm::adb. There is one pair now and both ends of the chain use it: the
// window renders with CP_UTF8 and adb is spawned with CreateProcessW, so no second
// code page is left anywhere for a name to fall into.
using pm::util::path_from_utf8;
using pm::util::path_to_utf8;

bool has_extension(const std::filesystem::path& file, const char* wanted) {
    std::string ext = path_to_utf8(file.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == wanted;
}

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

// The phone's brightness as it was before we dimmed it, so it can be put back.
struct SavedBrightness {
    int brightness = -1;           // -1 = not saved
    int brightness_mode = -1;      // -1 = not saved, 0 = manual, 1 = auto
    std::string device_id;         // which phone this belongs to
};

// Reads the current brightness off the phone before it gets turned down.
SavedBrightness read_brightness(pm::adb::AdbClient& adb, const std::string& device_id) {
    SavedBrightness saved;
    saved.device_id = device_id;

    std::string res = adb.execute_shell_command(
        device_id, "settings get system screen_brightness; echo __DIV__; settings get system screen_brightness_mode");
    size_t div = res.find("__DIV__");
    if (div != std::string::npos) {
        std::string b_str = res.substr(0, div);
        std::string m_str = res.substr(div + 7);
        b_str.erase(b_str.find_last_not_of(" \n\r\t") + 1);
        m_str.erase(m_str.find_last_not_of(" \n\r\t") + 1);
        try { saved.brightness = std::stoi(b_str); } catch (...) {}
        try { saved.brightness_mode = std::stoi(m_str); } catch (...) {}
    } else {
        res.erase(res.find_last_not_of(" \n\r\t") + 1);
        try { saved.brightness = std::stoi(res); } catch (...) {}
    }

    return saved;
}

// Puts the saved brightness back on the phone.
void restore_brightness(pm::adb::AdbClient& adb, const SavedBrightness& saved) {
    if (saved.device_id.empty() || saved.brightness < 0) return;

    if (saved.brightness_mode >= 0) {
        adb.execute_shell_command(saved.device_id,
            "settings put system screen_brightness_mode " + std::to_string(saved.brightness_mode));
    }
    adb.execute_shell_command(saved.device_id,
        "settings put system screen_brightness " + std::to_string(saved.brightness));
}

// Reads the phone's own rotation setting instead of guessing. The menu toggle must
// always start out matching whatever the user already set on the phone — never a
// fixed app default that quietly flips it. See issue #46.
bool read_auto_rotate(pm::adb::AdbClient& adb, const std::string& device_id) {
    std::string res = adb.execute_shell_command(device_id, "settings get system accelerometer_rotation");
    res.erase(0, res.find_first_not_of(" \n\r\t"));
    res.erase(res.find_last_not_of(" \n\r\t") + 1);
    return res != "0"; // treat unknown/empty output as "on", same as a stock phone default
}

class ScopeExit {
public:
    explicit ScopeExit(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~ScopeExit() { if (fn_) fn_(); }

private:
    std::function<void()> fn_;
};

// Background work used to be std::thread::detach'ed and then forgotten. On shutdown
// those threads were still running and writing through references to objects that no
// longer existed. Every worker is tracked here and joined before teardown.
class BackgroundTasks {
public:
    ~BackgroundTasks() { join_all(); }

    void run(std::function<void()> work) {
        std::lock_guard<std::mutex> lock(m_mutex);
        collect_finished_locked();

        auto done = std::make_shared<std::atomic<bool>>(false);
        m_workers.push_back({std::thread([work = std::move(work), done]() {
            work();
            done->store(true);
        }), done});
    }

    void join_all() {
        std::vector<Worker> workers;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            workers.swap(m_workers);
        }
        for (auto& worker : workers) {
            if (worker.thread.joinable()) worker.thread.join();
        }
    }

private:
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };

    void collect_finished_locked() {
        for (auto it = m_workers.begin(); it != m_workers.end();) {
            if (it->done->load()) {
                if (it->thread.joinable()) it->thread.join();
                it = m_workers.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::mutex m_mutex;
    std::vector<Worker> m_workers;
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

    // Win32 hands out wchar_t, every std::string in this codebase is UTF-8.
    std::string wide_to_utf8(const wchar_t* wstr) {
        if (!wstr || !wstr[0]) return "";
        int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
        if (len <= 1) return "";
        std::string out(static_cast<size_t>(len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &out[0], len, nullptr, nullptr);
        return out;
    }

    LRESULT CALLBACK PinEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_KEYDOWN) {
            if (wp == VK_RETURN) {
                wchar_t buf[128] = {0};
                GetWindowTextW(hwnd, buf, 128);
                g_pin_result = wide_to_utf8(buf);
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
        return CallWindowProcW(g_old_edit_proc, hwnd, msg, wp, lp);
    }
    
    LRESULT CALLBACK PinDialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CREATE: {
                HWND hEdit = CreateWindowExW(0, L"EDIT", L"", 
                    WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL | ES_CENTER,
                    20, 50, 160, 24, hwnd, nullptr, GetModuleHandle(nullptr), nullptr);
                HFONT hFont = CreateFontW(18, 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET, 0,0,0,0, L"Segoe UI");
                SendMessage(hEdit, WM_SETFONT, (WPARAM)hFont, 0);
                g_old_edit_proc = (WNDPROC)SetWindowLongPtrW(hEdit, GWLP_WNDPROC, (LONG_PTR)PinEditProc);
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
                HFONT hFont = CreateFontW(16, 0,0,0, FW_BOLD, 0,0,0, DEFAULT_CHARSET, 0,0,0,0, L"Segoe UI");
                HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
                RECT tr = {0, 15, 200, 40};
                DrawTextW(hdc, L"PIN eingeben:", -1, &tr, DT_CENTER | DT_TOP);
                
                SetTextColor(hdc, RGB(150, 150, 150));
                HFONT smallFont = CreateFontW(12, 0,0,0, FW_NORMAL, 0,0,0, DEFAULT_CHARSET, 0,0,0,0, L"Segoe UI");
                SelectObject(hdc, smallFont);
                RECT br = {0, 85, 200, 110};
                DrawTextW(hdc, L"Enter (OK)  |  Esc (Abbrechen)", -1, &br, DT_CENTER | DT_TOP);
                
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
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}
#endif

std::string prompt_user_for_pin(void* parent_hwnd = nullptr) {
    while (true) {
#ifdef _WIN32
        g_pin_result = "";
        HINSTANCE hi = GetModuleHandle(nullptr);
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.lpfnWndProc = PinDialogProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = L"PixelMirroringPinDialog";
        RegisterClassExW(&wc);
        
        HWND parent = (HWND)parent_hwnd;
        int px = 0, py = 0;
        if (parent) {
            RECT pr; GetWindowRect(parent, &pr);
            px = pr.left + (pr.right - pr.left) / 2 - 100;
            py = pr.top + (pr.bottom - pr.top) / 2 - 55;
        }
        
        HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"PIN", 
            WS_POPUP, px, py, 200, 110, parent, nullptr, hi, nullptr);
            
        if (parent) EnableWindow(parent, FALSE);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        
        g_pin_done = false;
        MSG msg;
        while (true) {
            const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
            if (got == 0) {
                // WM_QUIT arrived while the PIN box owns the loop. Re-post it so the
                // main message loop still sees it and the app can shut down.
                PostQuitMessage(static_cast<int>(msg.wParam));
                break;
            }
            if (got == -1 || g_pin_done) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        
        if (parent) EnableWindow(parent, TRUE);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hi);
        
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
        MessageBoxW((HWND)parent_hwnd, L"PIN darf nur aus Ziffern bestehen und maximal 16 Zeichen lang sein.", L"Ungültige Eingabe", MB_OK | MB_ICONERROR);
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
    // A portable build must not touch %LOCALAPPDATA% — it keeps everything next to
    // the EXE. Only the installed build gets a per-user config directory.
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
    std::filesystem::path fallback = path_from_utf8(pm::adb::get_executable_dir());
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
    std::filesystem::path exe_dir = path_from_utf8(pm::adb::get_executable_dir());
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

// EXPERIMENT. The home screen for a display of our own. The phone's own launcher for
// secondary displays renders but cannot start anything (see VIRTUAL-DISPLAY.md), so
// this one ships with the client and is pushed over when the mode needs it.
const char* const FOSSIFY_PACKAGE = "org.fossify.home";

std::optional<std::filesystem::path> find_fossify_apk() {
    std::filesystem::path current = path_from_utf8(pm::adb::get_executable_dir());
    for (int i = 0; i < 4; ++i) {
        const std::filesystem::path candidate = current / "FossifyLauncher.apk";
        if (path_exists(candidate)) {
            return candidate;
        }
        const std::filesystem::path vendored = current / "vendor" / "FossifyLauncher.apk";
        if (path_exists(vendored)) {
            return vendored;
        }
        current = current.parent_path();
        if (current.empty()) break;
    }
    return std::nullopt;
}

// Installs the launcher on the phone if it is missing. Returns whether the phone has it
// afterwards — a false is not fatal anywhere, it only means the new display shows
// whatever the phone puts on it by itself.
//
// Deliberately WITHOUT the "-g" that the companion app gets. Pre-granting every runtime
// permission is defensible for our own app, which the user installed on purpose; a
// launcher fetched from a third-party repository has to ask the phone like any other app.
bool ensure_fossify_installed(pm::adb::AdbClient& adb, const std::string& device_id) {
    if (adb.is_app_installed(device_id, FOSSIFY_PACKAGE)) {
        return true;
    }

    const auto apk = find_fossify_apk();
    if (!apk) {
        return false;
    }

    const std::string remote = "/data/local/tmp/FossifyLauncher.apk";
    if (!adb.push_file(device_id, path_to_utf8(*apk), remote)) {
        return false;
    }
    const bool installed = adb.install_pushed_app(device_id, remote);
    adb.execute_shell_command(device_id, "rm -f " + remote);
    return installed;
}

// adb prints several lines and the useful one is rarely the first ("Performing
// Streamed Install" comes first). Pick the line that names the failure, otherwise
// fall back to the last non-empty line.
std::string extract_adb_reason(const std::string& output) {
    std::istringstream stream(output);
    std::string line;
    std::string fallback;

    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;

        const auto failure = line.find("Failure");
        if (failure != std::string::npos) {
            return line.substr(failure); // "Failure [INSTALL_FAILED_...]" — the actual reason
        }
        if (line.find("failed") != std::string::npos || line.find("Error") != std::string::npos) {
            fallback = line;
        } else if (fallback.empty()) {
            fallback = line;
        }
    }

    return fallback;
}

// "Installation failed" tells the user nothing actionable. Name the actual blocker.
std::string build_install_error_message(pm::adb::AdbClient& adb, const std::string& device_id) {
    const std::string error = adb.last_install_error();

    const bool signature_clash =
        error.find("INSTALL_FAILED_UPDATE_INCOMPATIBLE") != std::string::npos ||
        error.find("INCONSISTENT_CERTIFICATES") != std::string::npos ||
        error.find("signatures do not match") != std::string::npos;

    if (signature_clash) {
        // An older copy sits in another user profile or the private space. We never
        // install into or uninstall from profiles other than the active one, so the
        // user has to clear it there — all we can do is name the right profile.
        std::string where = "einem anderen Profil";
        auto users = adb.users_with_app(device_id, ANDROID_PACKAGE);
        if (!users.empty()) {
            where = "Profil " + users.front();
            for (size_t i = 1; i < users.size(); ++i) {
                where += (i + 1 == users.size()) ? " und " : ", ";
                where += users[i];
            }
        }
        return "Ältere Version in " + where + " blockiert die Installation (andere Signatur). "
               "Bitte dort deinstallieren, dann erneut verbinden. Details: adb.log";
    }

    if (error.empty()) {
        return "Android-App konnte nicht installiert werden. Details: adb.log";
    }

    std::string reason = extract_adb_reason(error);
    if (reason.size() > 120) {
        reason = reason.substr(0, 117) + "...";
    }
    return "Android-App konnte nicht installiert werden: " + reason + " (Details: adb.log)";
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
    SavedBrightness* out_saved_brightness,
    BackgroundTasks& tasks,
    std::atomic<bool>* out_phone_auto_rotate
);

// A switch that is on but does nothing is worse than no switch at all. When the
// phone refuses the virtual keyboard, say so once in a dialog — the status line
// under the video is not visible while the stream is running.
//
// ONLY for a user who just flipped the switch in the menu. On an automatic connect
// this stays silent: a modal box in front of somebody waiting for the picture
// answers a question they did not ask, and the switch has already turned itself
// off, so the menu tells the same story next time they open it.
void show_uhid_unavailable_message(pm::window::IWindow& window) {
#ifdef _WIN32
    MessageBoxW(
        (HWND)window.get_native_handle(),
        L"Dieses Handy erlaubt keine virtuelle USB-Tastatur.\n\n"
        L"Die Option wurde wieder abgeschaltet, die normale Texteingabe "
        L"funktioniert weiterhin.",
        L"USB-Tastatur nicht verfügbar",
        MB_OK | MB_ICONINFORMATION
    );
#else
    (void)window;
#endif
}

// user_requested = the user pressed Ctrl+U or used the menu. Then we type the PIN
// even when the lock state cannot be read; on automatic connects we stay quiet.
bool unlock_device_if_needed(const std::string& device_id, pm::window::IWindow* window = nullptr,
                             bool user_requested = false);

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
            window.set_status_text("Warte auf USB-Gerät...");
            requested_reconnect = false; // device unplugged — allow another reconnect attempt
        } else {
            if (usb->state == "unauthorized") {
                window.set_status_text("Bitte USB-Debugging auf dem Handy erlauben.");
                if (!requested_reconnect) {
                    adb.reconnect_offline();
                    requested_reconnect = true;
                }
            } else {
                window.set_status_text("Warte auf USB-Gerät: " + usb->state);
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

    // The previous session may still be inside the phone's 60s idle window, in which
    // case the TCP device is already connected and ADB does not need waking at all.
    if (!setup_state.device_ip.empty()) {
        auto devices = adb.get_connected_devices();
        if (auto tcp = find_tcp_device(devices, setup_state.device_ip)) {
            return tcp;
        }
    }

    if (should_stop) return std::nullopt;

    // ADB is off. Ask the phone's discovery server to turn it back on before connecting.
    if (!setup_state.device_ip.empty()) {
        window.set_status_text("Wecke Gerät " + setup_state.device_ip + "...");
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

    window.set_status_text("Suche eingerichtetes Gerät im Netzwerk...");
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
    SavedBrightness* out_saved_brightness,
    BackgroundTasks& tasks,
    std::atomic<bool>* out_phone_auto_rotate
) {
    clear_setup_state();

    window.post_task([&window]() { window.set_status_text("Prüfe USB-Verbindung..."); });
    auto usb_device = wait_for_usb_authorization(adb, window, should_stop);
    if (!usb_device || should_stop) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SETUP);
            window.set_status_text("Kein USB-Gerät bereit. Bitte erneut verbinden.");
        });
        return false;
    }

    // Skip the APK push entirely when the app is already there.
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

        if (!adb.install_app(usb_device->id, path_to_utf8(*apk_path))) {
            std::string message = build_install_error_message(adb, usb_device->id);
            window.post_task([&window, message]() {
                window.set_app_state(pm::window::AppState::SETUP);
                window.set_status_text(message);
            });
            return false;
        }
    }

    // The app may still be paired to a DIFFERENT PC. The phone stores exactly one
    // clientId and never drops it on its own; nothing in the app ever calls
    // removePairedClient(). So once this PC's client_id.txt changes (the
    // "Werkseinstellungen" menu deletes it), the phone answers every /connect with 403
    // and there is no way back.
    //
    // This runs after a FRESH install too, which it did not use to. A fresh install is
    // not a blank app: Android's automatic backup restores the app's data, pairing and
    // all, so a phone that was set up with this PC before comes back with a stale
    // clientId and refuses every /connect with 403 — observed on a Pixel 9 after
    // reinstalling the companion app.
    //
    // Running setup over USB means the user is physically holding the phone with a
    // cable attached. That is enough proof of ownership to restart the pairing.
    // WRITE_SECURE_SETTINGS is granted below, after this, because clearing the app data
    // revokes it too.
    window.post_task([&window]() { window.set_status_text("Alte Kopplung wird gelöst..."); });
    adb.execute_shell_command(usb_device->id, std::string("pm clear ") + ANDROID_PACKAGE);

    // Skip the grant when the permission survived from an earlier setup.
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

    // EXPERIMENT. The home screen for the virtual display goes over here, while a cable
    // is attached: it is 5 MB, and pushing that over WiFi at the start of the first
    // session is a wait nobody asked for. A failure is not fatal — the mode installs it
    // itself later if it has to (see ensure_fossify_installed).
    window.post_task([&window]() { window.set_status_text("PC-Startbildschirm wird installiert..."); });
    ensure_fossify_installed(adb, usb_device->id);

    window.post_task([&window]() { window.set_status_text("Android-App wird gestartet..."); });
    adb.start_app(usb_device->id, ANDROID_PACKAGE);
    adb.start_service(usb_device->id, ANDROID_SERVICE);

    const std::string device_ip = adb.get_device_ip(usb_device->id);
    if (device_ip.empty()) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SETUP);
            window.set_status_text("Keine WLAN-IP gefunden. Bitte WLAN prüfen.");
        });
        return false;
    }

    window.post_task([&window]() { window.set_status_text("ADB über WLAN wird aktiviert..."); });
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
    
    // Unlocking and starting the stream do not depend on each other. Running them
    // in parallel keeps first-picture latency under a second.
    std::string tcp_id = tcp_device->id;
    auto unlock_future = std::async(std::launch::async, [tcp_id, &window]() {
        return unlock_device_if_needed(tcp_id, &window);
    });

    bool stream_ok = start_stream(window, scrcpy, renderer, input, tcp_device->id, out_saved_brightness, tasks, out_phone_auto_rotate);
    bool unlock_ok = unlock_future.get();

    if (!unlock_ok || !stream_ok) {
        clear_setup_state();
        if (!unlock_ok) {
            window.post_task([&window]() {
#ifdef _WIN32
                PostMessageA((HWND)window.get_native_handle(), WM_CLOSE, 0, 0);
#else
                exit(0);
#endif
            });
        }
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
    SavedBrightness* out_saved_brightness,
    BackgroundTasks& tasks,
    std::atomic<bool>* out_phone_auto_rotate
) {
    window.post_task([&window]() { window.set_status_text("Starte Video-Stream..."); });

    // A new session gets a fresh keyboard. Whatever the last one left behind is
    // gone with its control socket, so nothing is sent here — only forgotten.
    input.reset_uhid_keyboard();

    pm::stream::ScrcpyClient::Config config;
    config.device_id = device_id;

    // Translate the saved settings into the scrcpy server command line.
    pm::Settings settings = pm::load_settings();
    const pm::QualitySpec quality = pm::quality_spec(settings.m_quality);
    config.max_fps = quality.max_fps;
    config.max_size = quality.max_size;
    config.video_bit_rate = quality.video_bit_rate;
    config.lowest_brightness = settings.m_lowest_brightness;
    config.audio = settings.m_audio_enabled;
    config.new_display = settings.m_virtual_display;
    config.new_display_app = settings.m_virtual_display_app;

    // EXPERIMENT. A display of our own is worthless without a home screen on it, and the
    // one the phone brings does not work (VIRTUAL-DISPLAY.md). So the mode brings its
    // own: put it on the phone if it is missing, and aim at it unless the human has
    // named a different app by hand.
    bool fossify_freshly_installed = false;
    if (settings.m_virtual_display) {
        pm::adb::AdbClient launcher_adb;
        const bool had_it = launcher_adb.is_app_installed(device_id, FOSSIFY_PACKAGE);
        if (!had_it) {
            window.post_task([&window]() {
                window.set_status_text("Startbildschirm für den PC wird installiert...");
            });
        }
        if (ensure_fossify_installed(launcher_adb, device_id)) {
            fossify_freshly_installed = !had_it;
            if (config.new_display_app.empty()) {
                config.new_display_app = FOSSIFY_PACKAGE;
                pm::Settings stored = pm::load_settings();
                stored.m_virtual_display_app = FOSSIFY_PACKAGE;
                pm::save_settings(stored);
            }
        }
    }

    // EXPERIMENT. With a display of our own the phone's OWN panel is none of our
    // business any more: it stays dark and locked, and a second human may be holding
    // it. So every hand that reaches for that panel — dimming it, waking it, watching
    // whether it went dark — has to stay down while this is on.
    const bool own_display = config.new_display;

    // One hour of the phone lying untouched. Long enough that nobody trips over it in a
    // session, short enough that a cleanup that never ran does not leave a phone which
    // refuses to sleep forever. Only in this mode: while the phone's own picture is
    // being mirrored, its sleep is the human's business and none of ours.
    if (own_display) {
        config.screen_off_timeout_ms = 60 * 60 * 1000;
    }


    // Also runs with a display of our own, and on purpose: turning the panel off goes
    // past Android's own idea of the screen, so anything that lights it again — the
    // lock screen after a poke, a notification — comes back at full brightness. The
    // dimming underneath makes that moment harmless instead of blinding.
    if (settings.m_lowest_brightness) {
        // Remember the phone's current brightness, then turn it all the way down.
        // This only dims via ADB; m_screen_off below is what really turns the panel off.
        std::string dev_id = device_id;
        auto dim_now = [dev_id, out_saved_brightness]() {
            pm::adb::AdbClient adb;
            if (out_saved_brightness && out_saved_brightness->brightness < 0) {
                *out_saved_brightness = read_brightness(adb, dev_id);
            }
            adb.execute_shell_command(dev_id, "settings put system screen_brightness_mode 0; settings put system screen_brightness 0");
        };

        // These two fight each other. Writing a brightness makes Android re-evaluate the
        // panel and hand it a fresh power mode, which switches back ON the panel that
        // SET_DISPLAY_POWER had just darkened behind Android's back. As a background job
        // it landed in either order, and roughly every second start came up with a bright
        // phone. With a display of our own the dimming therefore happens HERE, before the
        // stream exists, so the power-off further down always has the last word. While
        // mirroring nothing turns the panel off, so that path is left as it was.
        if (own_display) {
            dim_now();
        } else {
            tasks.run(dim_now);
        }
    }

    // Reads the phone's rotation setting, never writes it — the menu toggle has to
    // start out exactly where the phone already was (see issue #46).
    {
        std::string dev_id = device_id;
        pm::window::IWindow* w = &window;
        tasks.run([dev_id, out_phone_auto_rotate, w]() {
            pm::adb::AdbClient adb;
            bool enabled = read_auto_rotate(adb, dev_id);
            if (out_phone_auto_rotate) *out_phone_auto_rotate = enabled;
            w->post_task([w, enabled]() { w->set_auto_rotate_enabled(enabled); });
        });
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

    // Server 4.x says out loud when the picture changes shape mid-stream. Without this
    // the mouse keeps aiming at the old ruler and the window keeps the old proportions
    // — a turned display would put every poke in the wrong place.
    scrcpy.set_resolution_callback([&window, &input](int new_w, int new_h) {
        input.set_device_size(new_w, new_h);
        window.post_task([&window, new_w, new_h]() {
            window.set_aspect_ratio((double)new_w / (double)new_h);
            window.set_orientation(new_w > new_h);
        });
    });
    window.set_render_callback([&](SDL_Renderer* renderer_ptr, int x, int y, int w, int h) {
        renderer.paint(renderer_ptr, x, y, w, h);
    });

    if (!scrcpy.start(config)) {
        // Window state is only ever touched from the UI thread, as everywhere else.
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SCANNING);
            window.set_status_text("Stream konnte nicht gestartet werden.");
        });
        return false;
    }

    int w = scrcpy.video_width();
    int h = scrcpy.video_height();
    if (w <= 0 || h <= 0) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SCANNING);
            window.set_status_text("Stream-Fehler: Ungültige Video-Dimensionen.");
        });
        scrcpy.stop();
        return false;
    }

    input.set_device_size(w, h);

    // The control socket only exists once the stream is up, which is why the panel
    // power mode is handled here and not next to the brightness code above. The
    // "else" is not dead weight: it catches a session whose "panel back on" message
    // never reached the phone.
    //
    // With a display of our own this switch is not a nicety, it is THE way to keep the
    // phone dark. Measured on a Pixel 9 with Android 17: once the phone itself falls
    // asleep, WindowManager puts our display to sleep with it and the picture stops —
    // server 4.x's keep_active only wakes it every four seconds and cannot hold it.
    // Turning the PANEL off instead leaves the phone awake, and our display alive,
    // while its screen stays black.
    if (settings.m_screen_off) {
        scrcpy.inject_screen_power_mode(false);
    } else if (scrcpy.screen_forced_off()) {
        scrcpy.inject_screen_power_mode(true);
    }

    // The virtual keyboard can only be created once the control socket is open, and
    // only on a phone that actually allows it — sending UHID_CREATE to one that does
    // not kills the server's entire control thread (see device_supports_uhid).
    bool uhid_keyboard = false;
    if (settings.m_uhid_keyboard) {
        uhid_keyboard = input.enable_uhid_keyboard();
        if (!uhid_keyboard) {
            // Persist the switch as off instead of leaving one on that does nothing.
            // No dialog here — see show_uhid_unavailable_message. The refusal is already
            // in stream.log, and the menu shows the switch off from now on.
            pm::Settings stored = pm::load_settings();
            stored.m_uhid_keyboard = false;
            pm::save_settings(stored);
            window.post_task([&window]() {
                window.set_status_text("Dieses Handy erlaubt keine USB-Tastatur. Texteingabe bleibt aktiv.");
            });
        }
    }

    // With the phone's own launcher switched off, nothing would ever appear on the new
    // display — the app has to be pushed onto it by hand, and only once the control
    // hole is open. Without decorations and without an app there is no picture at all.
    // Reads the CONFIG, not the settings from further up: the launcher may have been
    // filled in only a moment ago, after those were read.
    if (own_display && !config.new_display_app.empty()) {
        scrcpy.start_app(config.new_display_app);

        // On its very first run the launcher asks whether it may become the phone's ONE
        // home screen. That is a question about the whole phone, which nobody here has
        // answered, and its dialog would sit on the display blocking everything. BACK is
        // that dialog's "no". It is sent only right after a fresh install: on a home
        // screen a back press does nothing, so a wrong guess costs nothing, but sending
        // it every session would still mean poking at something we cannot see.
        if (fossify_freshly_installed) {
            constexpr int ANDROID_KEYCODE_BACK = 4;
            std::this_thread::sleep_for(std::chrono::seconds(3));
            scrcpy.inject_keycode(0, ANDROID_KEYCODE_BACK);
            scrcpy.inject_keycode(1, ANDROID_KEYCODE_BACK);
        }
    }

    window.post_task([&window, w, h, uhid_keyboard, own_display]() {
        window.set_uhid_keyboard(uhid_keyboard);
        window.set_aspect_ratio((double)w / (double)h);
        window.set_orientation(w > h);
        window.set_app_state(pm::window::AppState::STREAMING);
        if (own_display) {
            window.set_status_text("Eigener PC-Bildschirm aktiv — Handy bleibt gesperrt.");
        }
    });
    return true;
}

// What the phone is doing right now. "known" says whether the lock state could be
// read at all — if it could not, we must NOT start typing the PIN blindly.
struct DeviceLockState {
    bool screen_on = true;
    bool locked = false;
    bool known = false;
    bool low_power = false;
};

// Splits the reply at the __DIV__ markers the shell command was told to print.
std::vector<std::string> split_sections(const std::string& text, const std::string& marker) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t hit = text.find(marker, start);
        if (hit == std::string::npos) {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, hit - start));
        start = hit + marker.size();
    }
    return parts;
}

// Reads a flag like "deviceLocked=true" or "deviceLocked=1". Android prints it both
// ways depending on version, so both spellings have to be accepted.
bool read_bool_flag(const std::string& text, const std::string& key, bool* out) {
    const auto pos = text.find(key);
    if (pos == std::string::npos) return false;

    size_t i = pos + key.size();
    while (i < text.size() && (text[i] == ' ' || text[i] == '=')) ++i;
    if (text.compare(i, 4, "true") == 0 || (i < text.size() && text[i] == '1')) {
        *out = true;
        return true;
    }
    if (text.compare(i, 5, "false") == 0 || (i < text.size() && text[i] == '0')) {
        *out = false;
        return true;
    }
    return false;
}

DeviceLockState read_lock_state(pm::adb::AdbClient& adb, const std::string& device_id) {
    DeviceLockState state;

    // One round trip for all four questions. The greps run on the phone, so only a
    // few lines travel back over the wire.
    const std::string out = adb.execute_shell_command(device_id,
        "dumpsys power | grep -iE 'mInteractive|mWakefulness'"
        "; echo __DIV__; dumpsys trust | grep -i '(current)'"
        "; echo __DIV__; dumpsys window 2>/dev/null | grep -iE 'mShowingLockscreen|keyguardShowing|mDreamingLockscreen'"
        "; echo __DIV__; settings get global low_power");

    const std::vector<std::string> parts = split_sections(out, "__DIV__");
    const std::string power  = parts.size() > 0 ? parts[0] : "";
    const std::string trust  = parts.size() > 1 ? parts[1] : "";
    const std::string window = parts.size() > 2 ? parts[2] : "";

    if (parts.size() > 3) {
        std::string low = parts[3];
        low.erase(0, low.find_first_not_of(" \n\r\t"));
        low.erase(low.find_last_not_of(" \n\r\t") + 1);
        state.low_power = (low == "1" || low == "true");
    }

    // Screen: mInteractive=false or a wakefulness other than Awake means dark.
    if (power.find("mInteractive=false") != std::string::npos ||
        power.find("mWakefulness=Asleep") != std::string::npos ||
        power.find("mWakefulness=Dozing") != std::string::npos) {
        state.screen_on = false;
    }

    // Lock: search inside the CURRENT user's block, not just the line that holds
    // "(current):". Android prints deviceLocked on a LATER line, so a single-line
    // search almost never saw "already unlocked" and typed the PIN into whatever was
    // on screen — that is how home screen widgets got launched by accident.
    const size_t current_pos = trust.find("(current)");
    if (current_pos != std::string::npos) {
        const std::string block = trust.substr(current_pos, 400);
        if (read_bool_flag(block, "deviceLocked", &state.locked)) {
            state.known = true;
        }
    }
    if (!state.known && read_bool_flag(trust, "deviceLocked", &state.locked)) {
        state.known = true;
    }
    if (!state.known) {
        // Every Android version names this differently. Checked against a Pixel 9,
        // which answers "isKeyguardShowing=true mDreamingLockscreen=true".
        for (const char* key : {"isKeyguardShowing", "mShowingLockscreen",
                                "KeyguardShowing", "mDreamingLockscreen"}) {
            bool showing = false;
            if (read_bool_flag(window, key, &showing)) {
                state.locked = showing;
                state.known = true;
                break;
            }
        }
    }

    return state;
}

bool unlock_device_if_needed(const std::string& device_id, pm::window::IWindow* window, bool user_requested) {
    pm::Settings settings = pm::load_settings();
    if (settings.m_pin.empty()) return true;

    // EXPERIMENT. With a display of our own the client keeps its hands off the phone's
    // lock. An automatic unlock was built here once, because a locked phone leaves the
    // launcher on the new display unresponsive — but it proved unreliable in practice,
    // and typing a PIN into a phone nobody is looking at is the wrong kind of clever.
    // Clicking "Handy entsperren" by hand still does exactly that.
    if (settings.m_virtual_display && !user_requested) return true;

    pm::adb::AdbClient adb;

    DeviceLockState state = read_lock_state(adb, device_id);
    if (state.known && !state.locked && state.screen_on) {
        // Screen on and unlocked already — typing anything here would land in
        // whatever app has focus.
        return true;
    }

    if (state.low_power) {
        bool disable_power_saving = false;
        if (window) {
#ifdef _WIN32
            int answer = MessageBoxW(
                (HWND)window->get_native_handle(),
                L"Das Gerät befindet sich im Energiesparmodus. Die PIN-Eingabe schlägt dadurch fehl.\n\nDarf die App den Energiesparmodus deaktivieren?",
                L"Energiesparmodus",
                MB_YESNO | MB_ICONQUESTION
            );
            if (answer == IDYES) disable_power_saving = true;
#endif
        }
        if (disable_power_saving) {
            adb.execute_shell_command(device_id, "settings put global low_power 0; cmd power set-mode 0");
        } else {
            return false;
        }
    }
    
    // Never type into a phone that cannot be proven locked. Without this guard the
    // keystrokes land on the home screen and trigger whatever widget has focus — that
    // is how the weather app kept opening itself. An explicit Ctrl+U is different:
    // the user asked for it, so type even when the state is unreadable.
    if (!state.known && !user_requested) {
        return true;
    }

    // ONE adb call: the sleeps run ON THE PHONE, so the whole unlock costs a single
    // round trip.
    //
    // 224 = WAKEUP, 66 = ENTER brings up the PIN pad, then the digits, then 66 to
    // submit. Do NOT replace the ENTER with "wm dismiss-keyguard": on a Pixel that
    // opens the FINGERPRINT prompt instead of the PIN pad, the digits go nowhere and
    // the unlock hangs on the sensor. Verified on a Pixel 9.
    //
    // The two short sleeps are load-bearing. Firing everything in one breath
    // ("input keyevent 224 66 <digits> 66") meant that on a sleeping phone the digits
    // arrived before the PIN pad existed and were dropped — that was the "PIN falsch"
    // on automatic connect, while a later Ctrl+U worked simply because the phone was
    // awake by then.
    const bool slow = settings.m_compatibility_mode;

    std::string digits;
    for (char c : settings.m_pin) {
        digits += " " + std::to_string(7 + (c - '0')); // KEYCODE_0 is 7
    }

    const std::string wake_delay = slow ? "0.6" : "0.3";
    const std::string pad_delay = slow ? "1.0" : "0.45";
    adb.execute_shell_command(device_id,
        "input keyevent 224; sleep " + wake_delay +
        "; input keyevent 66; sleep " + pad_delay +
        "; input keyevent" + digits + " 66");

    return true;
}

}

static int app_main() {
#ifdef _WIN32
    // Only one instance may run. A second launch hands focus to the first and exits.
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

    // Load the saved settings once and sync every menu checkbox to them.
    pm::Settings initial_settings = pm::load_settings();
    window->set_quality_preset(static_cast<int>(initial_settings.m_quality));
    window->set_compatibility_mode(initial_settings.m_compatibility_mode);
    window->set_lowest_brightness(initial_settings.m_lowest_brightness);
    window->set_screen_off(initial_settings.m_screen_off);
    window->set_capture_send_to_phone(initial_settings.m_send_captures_to_phone);
    window->set_audio_enabled(initial_settings.m_audio_enabled);
    window->set_uhid_keyboard(initial_settings.m_uhid_keyboard);
    window->set_virtual_display(initial_settings.m_virtual_display);
    window->set_auto_pause_minimized(initial_settings.m_auto_pause_minimized);

    SavedBrightness saved_brightness; // the phone's brightness before we dimmed it
    std::atomic<bool> phone_auto_rotate{true}; // Mirrors the phone's OWN rotation switch, never sets a default
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

    // Creating the virtual keyboard asks the phone first, which takes long enough for
    // the user to click the menu entry a second time. Without this the worker cannot
    // see the second click, finishes anyway and switches the keyboard on — while the
    // saved setting already says "off" and no UHID_DESTROY was ever sent.
    //
    // This holds the NEWEST requested state, and the worker re-checks it before
    // declaring success. A click counter would also detect "clicked again", but it
    // cannot tell on-off-on from on-off, and in the first case tearing the keyboard
    // down would be exactly wrong. What matters is the desired state, not the number
    // of clicks.
    //
    // Declared ABOVE background_tasks on purpose: the task pool joins its workers in
    // its destructor, so anything a task captures by reference must outlive the pool.
    std::atomic<bool> uhid_wanted{initial_settings.m_uhid_keyboard};

    // --- Auto-pause while the window is folded down ---------------------------
    //
    // A window minimized to the taskbar shows nothing, while the phone keeps encoding
    // 60 frames a second and pushing ~20 Mbit/s over the WLAN. So the stream is torn
    // down while the window is down — but the heartbeat that keeps the phone's ADB
    // session alive stays running. That is the whole trick: resuming is then just
    // start_stream() again, with no discovery, no /connect and no pairing. Without the
    // heartbeat the phone's watchdog disables ADB after 60s and coming back would cost
    // the full cold path.
    //
    // Same pattern as the UHID switch above: the minimize notification arrives on the
    // UI thread, so it may only record the desired state — never tear a stream down.
    std::atomic<bool> auto_pause_enabled{initial_settings.m_auto_pause_minimized};
    std::atomic<bool> pause_wanted{false};   // newest desired state from the window
    std::atomic<bool> auto_paused{false};    // true only while WE are holding the stream down
    std::atomic<bool> pause_worker_busy{false};
    // The phone this session belongs to, remembered so resuming needs no discovery.
    std::string paused_device_id;
    std::mutex paused_device_mutex;

    // Assigned further down, next to apply_quality_now, where the stream and the
    // heartbeat are in scope. The menu handler only needs the declaration.
    std::function<void()> reconcile_pause_state;

    BackgroundTasks background_tasks;

    auto run_on_device = [&scrcpy, &background_tasks](auto action) {
        if (!scrcpy.is_running()) return;
        std::string device_id = scrcpy.get_device_id();
        if (device_id.empty()) return;
        background_tasks.run([device_id, action]() {
            action(device_id);
        });
    };

    auto publish_capture_to_phone = [&scrcpy, &background_tasks, w = window.get()](const std::filesystem::path& path) {
        if (!scrcpy.is_running() || path.empty()) return;
        const std::string device_id = scrcpy.get_device_id();
        if (device_id.empty()) return;
        background_tasks.run([device_id, path, w]() {
            pm::adb::AdbClient adb;
            const std::string remote_dir = "/sdcard/Pictures/PixelMirroring";
            const std::string remote_path = remote_dir + "/" + path_to_utf8(path.filename());
            adb.execute_shell_command(device_id, "mkdir -p " + remote_dir);
            const bool sent = adb.push_file(device_id, path_to_utf8(path), remote_path);
            if (sent) {
                // Preferred path: our own BroadcastReceiver in the companion app runs
                // MediaScannerConnection, which is the reliable route on Android 11+.
                adb.execute_shell_command(device_id,
                    "am broadcast -a dev.pixelmirroring.app.SCAN_FILE -e path \"" + remote_path + "\"");
                // Fallback: insert into the media provider directly.
                adb.execute_shell_command(device_id,
                    "content insert --uri content://media/external/images/media --bind _data:s:\"" + remote_path + "\"");
                // Last resort: the legacy scan broadcast, for older Android versions.
                adb.execute_shell_command(device_id,
                    "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d file://" + remote_path);
            }
            w->post_task([w, sent]() {
                w->set_status_text(sent
                    ? "Aufnahme wurde ans Handy gesendet."
                    : "Aufnahme konnte nicht ans Handy gesendet werden.");
            });
        });
    };

    // Files dragged from the PC onto the window get pushed to the phone. Only one
    // transfer at a time — two would compete for the same WLAN airtime as the stream.
    std::atomic<bool> transfer_busy{false};
    window->set_file_drop_callback([&](const std::vector<std::string>& dropped) {
        // The status line is hidden while the video is live, so a refusal also has to
        // show up in the transfer bubble.
        auto refuse = [&](const char* bubble, const char* status) {
            window->set_status_text(status);
            window->set_transfer_status(pm::window::TransferState::FAILED, 1.0f, bubble);
        };

        const std::string device_id = scrcpy.is_running() ? scrcpy.get_device_id() : std::string();
        if (device_id.empty()) {
            refuse("Keine Verbindung", "Dateiübertragung braucht eine aktive Verbindung.");
            return;
        }

        // The bubble already shows the running transfer — do not overwrite it.
        if (transfer_busy.load()) {
            window->set_status_text("Es läuft bereits eine Dateiübertragung.");
            return;
        }

        std::vector<std::filesystem::path> files;
        for (const auto& raw : dropped) {
            const std::filesystem::path file = path_from_utf8(raw);
            std::error_code ec;
            // Directories would need recursive handling, which this path does not do.
            if (std::filesystem::is_regular_file(file, ec)) files.push_back(file);
        }
        if (files.empty()) {
            refuse("Nur Dateien", "Nur einzelne Dateien lassen sich übertragen, keine Ordner.");
            return;
        }

        // A dropped APK can be installed instead of just copied, but only on request.
        const auto apk_count = static_cast<size_t>(std::count_if(files.begin(), files.end(),
            [](const std::filesystem::path& file) { return has_extension(file, ".apk"); }));
        bool install_apks = false;
        if (apk_count > 0) {
#ifdef _WIN32
            std::wstring question = (apk_count == 1)
                ? L"Die abgelegte Datei ist eine Android-App (APK)."
                : L"Die abgelegten Dateien enthalten " + std::to_wstring(apk_count) + L" Android-Apps (APK).";
            question += L"\n\nJa\t\tauf dem Handy installieren"
                        L"\nNein\t\tnur nach /sdcard/Download kopieren"
                        L"\nAbbrechen\tnichts übertragen";
            const int answer = MessageBoxW((HWND)window->get_native_handle(), question.c_str(),
                L"APK abgelegt", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDCANCEL) return;
            install_apks = (answer == IDYES);
#endif
        }

        const std::string drop_label = files.size() == 1
            ? path_to_utf8(files.front().filename())
            : (std::to_string(files.size()) + " Dateien");
        transfer_busy.store(true);
        window->set_transfer_status(pm::window::TransferState::ACTIVE, 0.0f, drop_label);

        background_tasks.run([&transfer_busy, &should_stop, &scrcpy, w = window.get(),
                              device_id, files, install_apks, drop_label]() {
            pm::adb::AdbClient adb;
            adb.execute_shell_command(device_id, std::string("mkdir -p ") + DROP_TARGET_DIR);

            size_t carried = 0;
            std::string clipboard_path;
            std::string failure;

            for (size_t i = 0; i < files.size() && !should_stop; ++i) {
                const std::filesystem::path& file = files[i];
                // ONE spelling of the file name. There used to be two — an ANSI one for
                // adb and a UTF-8 one for the window — and mixing them up rendered
                // "Größe.pdf" as "Gr��e.pdf". adb is spawned with CreateProcessW now, so
                // both ends are UTF-8 and there is nothing left to mix up.
                const std::string name = path_to_utf8(file.filename());
                const bool as_app = install_apks && has_extension(file, ".apk");
                // An APK that is to be installed goes to the shell's own directory: the
                // package installer cannot read out of /sdcard on modern Android.
                const std::string remote_path = as_app
                    ? ("/data/local/tmp/" + name)
                    : (std::string(DROP_TARGET_DIR) + "/" + name);
                // With several files the bubble counts them; with one it shows the name.
                const std::string label = files.size() == 1
                    ? name
                    : (std::to_string(i + 1) + "/" + std::to_string(files.size()) + " " + name);

                const float base = static_cast<float>(i) / static_cast<float>(files.size());
                const float span = 1.0f / static_cast<float>(files.size());
                int last_percent = -1;
                const bool sent = adb.push_file_paced(device_id, path_to_utf8(file), remote_path,
                    [&](uint64_t done, uint64_t total) {
                        if (total == 0) return;
                        const int percent = static_cast<int>(done * 100 / total);
                        if (percent == last_percent) return;
                        last_percent = percent;
                        w->set_transfer_status(pm::window::TransferState::ACTIVE,
                            base + span * (static_cast<float>(percent) / 100.0f), label);
                    },
                    &should_stop);

                if (!sent) {
                    failure = should_stop ? "Übertragung abgebrochen." : ("Fehlgeschlagen: " + name);
                    break;
                }

                if (as_app) {
                    // The file is already on the phone, so install it from there and clean
                    // up the copy afterwards either way.
                    const bool installed = adb.install_pushed_app(device_id, remote_path);
                    adb.execute_shell_command(device_id,
                        "rm -f " + pm::adb::shell_quote(remote_path));
                    if (!installed) {
                        failure = "Installation fehlgeschlagen: " + adb.last_install_error();
                        break;
                    }
                } else {
                    // Tell the companion app to scan it, so the phone's file manager and
                    // gallery show it right away instead of after the next media scan.
                    adb.execute_shell_command(device_id,
                        "am broadcast -a dev.pixelmirroring.app.SCAN_FILE -e path " +
                        pm::adb::shell_quote(remote_path));
                    clipboard_path = remote_path;
                }
                ++carried;
            }

            if (should_stop) {
                transfer_busy.store(false);
                return;
            }

            // Put the last file's path on the phone's clipboard, so it can be pasted
            // straight into whichever app is supposed to open it.
            if (!clipboard_path.empty()) {
                scrcpy.inject_set_clipboard(clipboard_path);
            }

            const bool all_good = failure.empty() && carried == files.size();
            w->set_transfer_status(
                all_good ? pm::window::TransferState::DONE : pm::window::TransferState::FAILED,
                1.0f, all_good ? drop_label : failure);
            w->post_task([w, all_good, failure, carried]() {
                w->set_status_text(all_good
                    ? (std::to_string(carried) + " Datei(en) ans Handy übertragen.")
                    : failure);
            });
            transfer_busy.store(false);
        });
    });

    // A new quality preset has to take effect immediately. Assigned further down,
    // where the stream and the heartbeat are in scope; the menu only needs the
    // declaration.
    std::function<void()> apply_quality_now;

    // Context menu actions.
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
            case pm::window::MenuAction::SET_QUALITY_BATTERY:
            case pm::window::MenuAction::SET_QUALITY_BALANCED:
            case pm::window::MenuAction::SET_QUALITY_MAXIMUM: {
                const pm::QualityPreset preset =
                    (action == pm::window::MenuAction::SET_QUALITY_BATTERY) ? pm::QualityPreset::BATTERY :
                    (action == pm::window::MenuAction::SET_QUALITY_MAXIMUM) ? pm::QualityPreset::MAXIMUM :
                                                                             pm::QualityPreset::BALANCED;
                if (preset == current_settings.m_quality) break;
                current_settings.m_quality = preset;
                pm::save_settings(current_settings);
                window->set_quality_preset(static_cast<int>(preset));
                if (apply_quality_now) apply_quality_now();
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
            case pm::window::MenuAction::TOGGLE_SCREEN_OFF: {
                current_settings.m_screen_off = !current_settings.m_screen_off;
                pm::save_settings(current_settings);
                window->set_screen_off(current_settings.m_screen_off);
                // Unlike every other switch here this one takes effect immediately —
                // somebody unticking it wants the panel back on now, not next session.
                if (scrcpy.is_running()) {
                    scrcpy.inject_screen_power_mode(!current_settings.m_screen_off);
                }
                window->set_status_text(current_settings.m_screen_off
                    ? "Handy-Display wird während der Spiegelung ausgeschaltet."
                    : "Handy-Display bleibt an.");
                break;
            }
            case pm::window::MenuAction::TOGGLE_VIRTUAL_DISPLAY: {
                // EXPERIMENT. The server only ever reads this when it is born, so the
                // stream has to die and come back — same road the quality stones walk.
                current_settings.m_virtual_display = !current_settings.m_virtual_display;
                pm::save_settings(current_settings);
                window->set_virtual_display(current_settings.m_virtual_display);
                window->set_status_text(current_settings.m_virtual_display
                    ? "Eigener PC-Bildschirm wird aufgebaut..."
                    : "Zurück zur Spiegelung des Handy-Bildschirms...");
                if (apply_quality_now) apply_quality_now();
                break;
            }
            case pm::window::MenuAction::EXPAND_NOTIFICATION_PANEL:
                scrcpy.inject_expand_notification_panel();
                break;
            case pm::window::MenuAction::EXPAND_SETTINGS_PANEL:
                scrcpy.inject_expand_settings_panel();
                break;
            case pm::window::MenuAction::COLLAPSE_PANELS:
                scrcpy.inject_collapse_panels();
                break;
            case pm::window::MenuAction::TAKE_SCREENSHOT: {
                if (!scrcpy.is_running()) {
                    window->set_status_text("Bildschirmfoto nur bei aktivem Stream möglich.");
                    break;
                }
                auto screenshot = renderer.take_screenshot();
                if (!screenshot) {
                    window->set_status_text("Bildschirmfoto konnte nicht erstellt werden.");
                    break;
                }
#ifdef _WIN32
                // The renderer writes a BMP; re-encode it as PNG via GDI+.
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
                window->set_status_text("Bildschirmfoto gespeichert: " + path_to_utf8(screenshot->filename()));
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
                    window->set_status_text("Videoaufnahme gespeichert: " + path_to_utf8(recording->filename()));
                    if (current_settings.m_send_captures_to_phone) {
                        publish_capture_to_phone(*recording);
                    }
                } else if (scrcpy.is_running() && renderer.start_recording()) {
                    window->set_recording(true);
                    window->set_status_text("Videoaufnahme läuft.");
                } else {
                    window->set_status_text("Videoaufnahme braucht einen geladenen Stream-Frame.");
                }
                break;
            }
            case pm::window::MenuAction::TOGGLE_AUDIO: {
                current_settings.m_audio_enabled = !current_settings.m_audio_enabled;
                pm::save_settings(current_settings);
                window->set_audio_enabled(current_settings.m_audio_enabled);
                // The audio socket is negotiated when the stream starts, so this switch
                // only takes effect on the next connection. Say so, instead of leaving
                // the user waiting for a change that will not come.
                window->set_status_text(current_settings.m_audio_enabled
                    ? "Ton aktiviert — wirkt ab der nächsten Verbindung."
                    : "Ton deaktiviert — wirkt ab der nächsten Verbindung.");
                break;
            }
            case pm::window::MenuAction::TOGGLE_AUTO_ROTATE: {
                // This is NOT stored in Settings — it only ever mirrors and flips the
                // phone's own accelerometer_rotation value (issue #46). The toggle is
                // synced from the phone on connect (see start_stream), and a click here
                // is the only thing that ever changes it.
                bool new_value = !phone_auto_rotate.load();
                phone_auto_rotate = new_value;
                window->set_auto_rotate_enabled(new_value);
                run_on_device([new_value](const std::string& device_id) {
                    pm::adb::AdbClient adb;
                    adb.execute_shell_command(device_id,
                        "settings put system accelerometer_rotation " + std::to_string(new_value ? 1 : 0));
                });
                break;
            }
            case pm::window::MenuAction::TOGGLE_UHID_KEYBOARD: {
                const bool wanted = !current_settings.m_uhid_keyboard;
                current_settings.m_uhid_keyboard = wanted;
                pm::save_settings(current_settings);

                // Record the newest requested state for any worker still waiting on the
                // phone's /dev/uhid probe.
                uhid_wanted.store(wanted);

                if (!scrcpy.is_running()) {
                    window->set_uhid_keyboard(wanted);
                    window->set_status_text(wanted
                        ? "USB-Tastatur aktiviert — wirkt ab der nächsten Verbindung."
                        : "USB-Tastatur deaktiviert.");
                    break;
                }
                if (!wanted) {
                    input.disable_uhid_keyboard();
                    window->set_uhid_keyboard(false);
                    window->set_status_text("USB-Tastatur deaktiviert — normale Texteingabe ist wieder aktiv.");
                    break;
                }

                // Creating the keyboard probes the phone over ADB first, and that must
                // never block the UI thread. The switch that routes key presses only
                // flips once the phone really has the device — flipping it early would
                // drop every keystroke in between.
                background_tasks.run([&input, &uhid_wanted, w = window.get()]() {
                    const bool created = input.enable_uhid_keyboard();

                    // The user clicked again while the probe was still running, and this
                    // time wants it OFF. Tear the keyboard down and return silently — the
                    // later click already updated the window. Without this the phone keeps
                    // a keyboard the saved setting says is off, and UHID_DESTROY is never
                    // sent.
                    if (!uhid_wanted.load()) {
                        if (created) {
                            input.disable_uhid_keyboard();
                        }
                        return;
                    }

                    if (!created) {
                        pm::Settings failed = pm::load_settings();
                        failed.m_uhid_keyboard = false;
                        pm::save_settings(failed);
                    }
                    w->post_task([w, created]() {
                        w->set_uhid_keyboard(created);
                        if (created) {
                            w->set_status_text("USB-Tastatur aktiv. Layout am Handy auf Deutsch stellen.");
                        } else {
                            w->set_status_text("Dieses Handy erlaubt keine USB-Tastatur. Texteingabe bleibt aktiv.");
                            show_uhid_unavailable_message(*w);
                        }
                    });
                });
                break;
            }
            case pm::window::MenuAction::OPEN_KEYBOARD_SETTINGS: {
                if (!scrcpy.is_running()) {
                    window->set_status_text("Tastatur-Einstellungen nur bei aktivem Stream.");
                    break;
                }
                scrcpy.open_hard_keyboard_settings();
                window->set_status_text("Tastatur-Einstellungen am Handy geöffnet.");
                break;
            }
            case pm::window::MenuAction::TOGGLE_AUTO_PAUSE_MINIMIZED: {
                const bool wanted = !current_settings.m_auto_pause_minimized;
                current_settings.m_auto_pause_minimized = wanted;
                pm::save_settings(current_settings);
                auto_pause_enabled.store(wanted);
                window->set_auto_pause_minimized(wanted);

                // Switched off while the stream is currently paused: resume right away
                // instead of leaving it paused until the next minimize/restore cycle.
                if (!wanted && auto_paused.load()) {
                    pause_wanted.store(false);
                    reconcile_pause_state();
                }
                window->set_status_text(wanted
                    ? "Stream pausiert künftig, wenn das Fenster minimiert wird."
                    : "Stream läuft künftig auch bei minimiertem Fenster weiter.");
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
                    int answer = MessageBoxW(
                        (HWND)window->get_native_handle(),
                        L"Möchtest du die gespeicherte PIN löschen?",
                        L"PIN löschen",
                        MB_YESNO | MB_ICONQUESTION
                    );
                    if (answer == IDYES) {
                        current_settings.m_pin = "";
                        pm::save_settings(current_settings);
                        window->set_status_text("PIN gelöscht.");
                    }
#else
                    current_settings.m_pin = "";
                    pm::save_settings(current_settings);
                    window->set_status_text("PIN gelöscht.");
#endif
                }
                break;
            }
            case pm::window::MenuAction::UNLOCK_DEVICE: {
                if (current_settings.m_pin.empty()) {
                    window->set_status_text("PIN nicht eingerichtet.");
                    break;
                }
                // Explicit user command — type the PIN even if the lock state is unreadable.
                run_on_device([w = window.get()](const std::string& id) {
                    unlock_device_if_needed(id, w, true);
                });
                break;
            }
            case pm::window::MenuAction::LOCK_DEVICE: {
                run_on_device([](const std::string& id) {
                    // KEYCODE_SLEEP — locks the phone and turns the screen off.
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
            // Coming back from the tray: wake and unlock the phone, or build a whole
            // new connection if the old one is gone.
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
            MessageBoxW(nullptr, L"Tray-Icon konnte nicht erstellt werden.", L"Fehler", MB_OK | MB_ICONERROR);
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
        constexpr int ANDROID_KEYCODE_HOME = 3;

        // On a display of our own with system decorations switched off, no home screen
        // is registered — the phone answers HOME with nothing at all, so whoever opened
        // an app is stuck in it. Here the key does what it means: put the launcher back
        // on the display. Press only, or the launcher is started twice per keystroke.
        if (action == 0 && keycode == ANDROID_KEYCODE_HOME && scrcpy.uses_new_display()) {
            const pm::Settings current = pm::load_settings();
            if (!current.m_virtual_display_app.empty()) {
                scrcpy.start_app(current.m_virtual_display_app);
                return;
            }
        }

        // action: 0 = down, 1 = up.
        if (action == 0) {
            input.handle_key_down(keycode);
        } else {
            input.handle_key_up(keycode);
        }
    });
    window->set_text_callback([&](const std::string& text) {
        input.handle_text(text);
    });
    window->set_raw_key_callback([&](int action, uint32_t scancode, bool extended) {
        // Sends the physical key position; the phone's own layout picks the character.
        // Returning false lets the window fall back to the Android keycode path.
        return input.handle_raw_key(action, scancode, extended);
    });
    window->set_focus_lost_callback([&]() {
        // Focus is gone — nothing may stay held down on the phone.
        input.release_all_keys();
    });
    window->set_scroll_callback([&](int x, int y, int w, int h, float hscroll, float vscroll) {
        input.handle_scroll(x, y, w, h, hscroll, vscroll);
    });

    // If the server's control thread dies the video keeps flowing while every tap and
    // keystroke silently goes nowhere. Say so — nothing can be repaired from this side,
    // only reconnected.
    scrcpy.set_control_lost_callback([w = window.get()]() {
        w->post_task([w]() {
            w->set_status_text("Eingabe am Handy abgebrochen — bitte neu verbinden.");
        });
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
    window->set_focus_callback([&scrcpy]() {
        // Window regained focus — pull the phone's clipboard so PC and phone agree.
        if (scrcpy.is_running()) {
            scrcpy.inject_get_clipboard(0);
        }
    });
    std::thread connection_thread;
    std::atomic<bool> connection_running{false};
    std::thread screen_poll_thread;
    std::atomic<bool> stop_screen_poll{false};
    std::thread heartbeat_thread;
    std::atomic<bool> stop_heartbeat{false};
    const std::string client_name = get_client_name();

    // std::ifstream takes the path object directly. Going through path::string() would
    // hand it the ANSI code page, which cannot spell a user profile in Cyrillic.
    std::string client_id;
    const std::filesystem::path client_id_path = get_client_id_path();
    {
        std::ifstream file(client_id_path);
        if (file && std::getline(file, client_id)) {
            if (!client_id.empty() && client_id.back() == '\r') client_id.pop_back();
        }
    }
    if (client_id.empty()) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> distrib(100000, 999999);
        client_id = "desktop-client-" + std::to_string(distrib(gen));
        std::ofstream file(client_id_path, std::ios::trunc);
        if (file) file << client_id;
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
            // POST /heartbeat every 5s so the phone knows the PC is still there. This
            // only keeps the phone's ADB session alive; noticing a dropped connection is
            // the job of the scrcpy video socket's recv() failing.
            pm::network::NetworkScanner hb_scanner;
            // A paused stream still counts as alive. The video is gone, but the phone
            // must keep its ADB session open or resuming would cost the full cold path.
            // auto_paused is raised BEFORE the stream is stopped for exactly this
            // reason — the other order leaves a window in which both are false and the
            // heartbeat thread exits for good.
            const auto still_wanted = [&]() {
                return scrcpy.is_running() || auto_paused.load();
            };
            while (!should_stop && !stop_heartbeat && still_wanted()) {
                hb_scanner.send_heartbeat(ip, client_id, client_name);
                for (int i = 0; i < HEARTBEAT_INTERVAL_MS / 100
                     && !should_stop && !stop_heartbeat && still_wanted(); ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
        });
    };

    // The scrcpy server reads fps/size/bitrate from its command line at startup only,
    // so a new quality preset means stopping and restarting the stream. The heartbeat
    // thread ends with it and has to be started again.
    std::atomic<bool> quality_restart_running{false};
    apply_quality_now = [&]() {
        // Nothing streaming: the next connect picks the new preset up by itself.
        if (!scrcpy.is_running()) return;
        if (renderer.is_recording()) {
            // Restarting the stream mid-recording would leave a broken file behind.
            window->post_task([w = window.get()]() {
                w->set_status_text("Qualität wirkt nach dem Ende der Aufnahme.");
            });
            return;
        }
        const std::string device_id = scrcpy.get_device_id();
        if (device_id.empty()) return;
        if (quality_restart_running.exchange(true)) return;

        background_tasks.run([&, device_id]() {
            ScopeExit mark_done([&]() { quality_restart_running = false; });
            window->post_task([w = window.get()]() {
                w->set_app_state(pm::window::AppState::CONNECTED);
                w->set_status_text("Qualität wird umgestellt...");
            });
            scrcpy.stop();
            if (should_stop) return;
            if (start_stream(*window, scrcpy, renderer, input, device_id,
                             &saved_brightness, background_tasks, &phone_auto_rotate)) {
                start_heartbeat(device_id.substr(0, device_id.rfind(':')));
            }
        });
    };

    // A single worker reconciles the actual stream state with the newest requested one
    // (declared far above, where the menu can reach it). What matters is the requested
    // state, not how many times the user toggled it.
    reconcile_pause_state = [&]() {
        if (pause_worker_busy.exchange(true)) return; // one worker is enough
        background_tasks.run([&]() {
            ScopeExit mark_done([&]() { pause_worker_busy = false; });

            // Keep reconciling as long as the requested state keeps changing underneath.
            // The bound only exists so somebody hammering the minimize button cannot keep
            // this worker spinning forever.
            for (int rounds = 0; rounds < 8 && !should_stop; ++rounds) {
                const bool want_paused = pause_wanted.load();
                if (want_paused == auto_paused.load()) return;

                if (want_paused) {
                    // Never cut into somebody else's work: a torn stream would leave a
                    // recording as a broken file, and a quality restart already owns the
                    // stream. In those cases do nothing at all — auto_paused stays false,
                    // so restoring does nothing either.
                    //
                    // An in-flight connect is deliberately NOT in this list: it only
                    // raises is_running() once the stream really stands, and it re-runs
                    // this reconcile at the end of its own path (see start_connection).
                    if (!scrcpy.is_running() || renderer.is_recording() ||
                        quality_restart_running.load()) {
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> guard(paused_device_mutex);
                        paused_device_id = scrcpy.get_device_id();
                    }
                    // Raise the flag BEFORE stopping — see the heartbeat loop.
                    auto_paused.store(true);
                    scrcpy.stop();
                    window->post_task([w = window.get()]() {
                        w->set_app_state(pm::window::AppState::CONNECTED);
                        w->set_status_text("Pausiert — Fenster ist minimiert.");
                    });
                    continue;
                }

                std::string device_id;
                {
                    std::lock_guard<std::mutex> guard(paused_device_mutex);
                    device_id = paused_device_id;
                }
                window->post_task([w = window.get()]() {
                    w->set_app_state(pm::window::AppState::CONNECTED);
                    w->set_status_text("Stream wird fortgesetzt...");
                });

                // The warm path: ADB is still up because the heartbeat never stopped, so
                // there is nothing to discover and no pairing to negotiate.
                const bool resumed = !device_id.empty() &&
                    start_stream(*window, scrcpy, renderer, input, device_id,
                                 &saved_brightness, background_tasks, &phone_auto_rotate);

                // Lower the flag only now: while the stream was being rebuilt the
                // heartbeat still had to count as wanted.
                auto_paused.store(false);

                if (!resumed && !should_stop && !pause_wanted.load()) {
                    // Phone asleep, WLAN changed, ADB disabled after all — whatever it
                    // was, the warm path is gone. Fall back to a full connect.
                    window->post_task([w = window.get()]() {
                        w->set_status_text("Verbindung neu aufbauen...");
                    });
                    start_connection(true);
                    return;
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
            int clip_poll_counter = 0;
            int adb_poll_counter = 0;

            // One reused HTTP client instead of a fresh one on every poll round.
            httplib::Client screen_client(poll_ip.empty() ? "127.0.0.1" : poll_ip, 18294);
            screen_client.set_connection_timeout(0, 500000); // 500ms
            screen_client.set_read_timeout(0, 500000);       // 500ms

            while (!should_stop && !stop_screen_poll) {
                bool screen_on = true; // assume on unless a check proves otherwise
                bool phone_answered = false;

                // The screen is watched in EVERY mode, including with a display of our
                // own — pressing the side button is how someone says "I am done".
                //
                // This does not misfire on the panel WE darken: SET_DISPLAY_POWER goes
                // straight to SurfaceControl, so Android still reports the phone as
                // interactive and the checks below stay quiet. Only a phone that really
                // fell asleep reads as not interactive, and by then the display of our
                // own is dead anyway.
                if (scrcpy.is_running()) {
                    clip_poll_counter++;
                    if (clip_poll_counter >= 3) { // every ~1.5s
                        clip_poll_counter = 0;
                        // Pull the phone's clipboard so PC and phone stay in sync.
                        scrcpy.inject_get_clipboard(0);
                    }
                }

                // 1. The companion app's /screen endpoint — fastest and most accurate.
                if (!poll_ip.empty()) {
                    if (auto res = screen_client.Get("/screen")) {
                        if (res->status == 200) {
                            phone_answered = true;
                            if (res->body.find("\"screenOn\":false") != std::string::npos ||
                                res->body.find("\"screenOn\": false") != std::string::npos) {
                                screen_on = false;
                            }
                        }
                    }
                }

                // 2. An ADB shell check as a safety net. Every one of these spawns an
                // adb process, so it must not run on every poll round: roughly every 2s
                // while the phone stays silent, every 5s as a cross-check when it answers.
                const int adb_check_interval = phone_answered ? 10 : 4;
                if (screen_on && !device_id.empty() &&
                    ++adb_poll_counter >= adb_check_interval) {
                    adb_poll_counter = 0;
                    // Ask PowerManager, not "dumpsys display" — the latter prints a history
                    // of past OFF events and would read as "screen off" long after the fact.
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
                        // Screen went off: hand the phone its original brightness back.
                        if (saved_brightness.brightness >= 0) {
                            pm::adb::AdbClient restore_adb;
                            restore_brightness(restore_adb, saved_brightness);
                            saved_brightness = {}; // already restored, do not restore twice
                        }
                        
                        const bool own_display_session = scrcpy.uses_new_display();
                        window->post_task([&, own_display_session]() {
                            // Hide to the tray rather than showing a black window.
                            //
                            // With a display of our own the window folds down to the
                            // taskbar instead. The tray route exists because a mirrored
                            // picture without a phone screen is a black hole nobody wants
                            // to stare at — but this window carried a desktop of its own,
                            // and someone who switches the phone off expects to find it
                            // where every other window goes, not behind a tray icon.
                            if (window->is_visible()) {
                                if (own_display_session) {
                                    window->minimize();
                                } else {
                                    window->hide();
                                    if (tray) tray->show();
                                }
                            }
                            window->set_app_state(pm::window::AppState::SETUP);
                        });
                        // Tear the stream down so the phone stops encoding for nobody.
                        if (scrcpy.is_running()) {
                            scrcpy.stop();
                        }
                        break; // fully disconnected — this thread has nothing left to poll
                    }
                }
                
                // ~500ms until the next poll, in 100ms slices so shutdown stays responsive.
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
            // A fresh connection supersedes whatever the pause state was holding.
            auto_paused.store(false);
            pm::adb::AdbClient adb;

            // The adb server has to be up before anything else can talk to a device.
            window->post_task([w = window.get()]() { w->set_status_text("ADB wird gestartet..."); });
            adb.init();

            SetupState setup_state = load_setup_state();
            if (!setup_state.configured) {
                if (run_first_time_setup(adb, *window, scrcpy, renderer, input, should_stop,
                                         &saved_brightness, background_tasks, &phone_auto_rotate)) {
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
                // Do NOT clear the setup state here. A sleeping phone, a WLAN hiccup or a
                // companion app that was not running used to be enough to throw the whole
                // pairing away, and the next attempt then demanded a full USB re-setup.
                // The pairing has to survive a failed attempt.
                //
                // If a cable happens to be plugged in, redo the setup right away — that
                // is what a manual retry is asking for anyway. Otherwise keep everything
                // and show a message that actually helps.
                if (!automatic && !should_stop && find_usb_device(adb.get_devices(), "device")) {
                    window->post_task([w = window.get()]() {
                        w->set_status_text("Nicht über WLAN erreichbar — richte per USB neu ein...");
                    });
                    if (run_first_time_setup(adb, *window, scrcpy, renderer, input, should_stop,
                                             &saved_brightness, background_tasks, &phone_auto_rotate)) {
                        auto new_state = load_setup_state();
                        if (new_state.configured && !new_state.device_ip.empty()) {
                            start_heartbeat(new_state.device_ip);
                            start_screen_poll(new_state.device_ip, new_state.device_ip + ":5555");
                        }
                    }
                    return;
                }

                window->post_task([w = window.get()]() {
                    w->set_app_state(pm::window::AppState::SETUP);
                    w->set_status_text("Gerät nicht erreichbar. Bitte die Pixel-Mirroring-App "
                                       "am Handy öffnen, oder für eine Neueinrichtung per USB verbinden.");
                });
                return;
            }

            std::string name = setup_state.device_name;
            window->post_task([w = window.get(), name]() {
                w->set_app_state(pm::window::AppState::CONNECTED);
                w->set_status_text(name.empty() ? "Verbunden" : name);
            });
            
            // Unlock and stream startup are independent — run them in parallel.
            std::string tcp_id = tcp_device->id;
            auto unlock_future = std::async(std::launch::async, [tcp_id, w = window.get()]() {
                return unlock_device_if_needed(tcp_id, w);
            });

            bool stream_ok = start_stream(*window, scrcpy, renderer, input, tcp_device->id,
                                          &saved_brightness, background_tasks, &phone_auto_rotate);
            bool unlock_ok = unlock_future.get();

            if (!unlock_ok) {
                window->post_task([w = window.get()]() {
#ifdef _WIN32
                    PostMessageA((HWND)w->get_native_handle(), WM_CLOSE, 0, 0);
#else
                    exit(0);
#endif
                });
                return;
            }
            if (stream_ok) {
                std::string hb_ip = tcp_device->id.substr(0, tcp_device->id.rfind(':'));
                start_heartbeat(hb_ip);
                start_screen_poll(hb_ip, tcp_device->id);
                // The window was minimized while this connect was still running. The
                // stream is up now with nobody watching, so run the reconcile once more.
                if (pause_wanted.load()) {
                    reconcile_pause_state();
                }
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

    // Fires on the UI thread, so it may only record the desired state and hand off.
    window->set_minimize_callback([&](bool minimized) {
        if (!auto_pause_enabled.load()) {
            // Auto-pause was switched off mid-session while the stream was already
            // paused: resume anyway instead of leaving it paused forever.
            if (!minimized && auto_paused.load()) {
                pause_wanted.store(false);
                reconcile_pause_state();
            }
            return;
        }
        pause_wanted.store(minimized);
        reconcile_pause_state();
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

    // Restore the phone's brightness while ADB is still connected.
    if (saved_brightness.brightness >= 0) {
        pm::adb::AdbClient restore_adb;
        restore_brightness(restore_adb, saved_brightness);
    }

    scrcpy.stop();

    // Third and last layer of panel restore. stop() already sent NORMAL over the
    // control socket, and the scrcpy server runs its own device-side CleanUp for the
    // same job. If BOTH failed — socket dead before we could send, server killed too —
    // the user would be left holding a phone with a permanently dark screen.
    //
    // A bare wakeup is not enough: Android only re-issues a display power mode when its
    // OWN screen state changes, and it already thinks the screen is on. Putting the
    // phone to sleep and waking it makes it re-light the panel itself and discards the
    // power mode we forced on it.
    if (scrcpy.screen_forced_off()) {
        const std::string dark_device = scrcpy.get_device_id();
        if (!dark_device.empty()) {
            pm::adb::AdbClient wake_adb;
            wake_adb.execute_shell_command(dark_device,
                "input keyevent 223; sleep 0.5; input keyevent 224"); // SLEEP, then WAKEUP
        }
    }

    if (connection_thread.joinable()) connection_thread.join();
    if (screen_poll_thread.joinable()) screen_poll_thread.join();
    if (heartbeat_thread.joinable()) heartbeat_thread.join();
    // Every background worker must finish before the objects it captured go away.
    background_tasks.join_all();

    if (tray) tray->hide();

#ifdef _WIN32
    // The window still owns GDI+ font objects. It has to be destroyed BEFORE
    // GdiplusShutdown, or it releases them into a GDI+ that no longer exists.
    renderer.shutdown();
    tray.reset();
    window.reset();
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
