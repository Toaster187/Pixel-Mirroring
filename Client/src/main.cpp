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

struct SetupState {
    bool configured = false;
    std::string device_ip;
    std::string device_name;
};

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

std::string prompt_user_for_pin() {
    while (true) {
#ifdef _WIN32
        std::string command = "powershell -Command \"[void][System.Reflection.Assembly]::LoadWithPartialName('Microsoft.VisualBasic'); [Microsoft.VisualBasic.Interaction]::InputBox('PIN zum automatischen Entsperren eingeben (nur Ziffern):', 'PIN einrichten', '')\"";
        FILE* pipe = _popen(command.c_str(), "r");
#else
        std::string command = "osascript -e 'display dialog \"PIN zum automatischen Entsperren eingeben (nur Ziffern):\" default answer \"\" with title \"PIN einrichten\"' -e 'text returned of result' 2>/dev/null";
        FILE* pipe = popen(command.c_str(), "r");
#endif
        if (!pipe) return "";
        char buffer[128];
        std::string result = "";
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' ' || result.back() == '\t')) {
            result.pop_back();
        }

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
        MessageBoxA(nullptr, "PIN darf nur aus Ziffern bestehen und maximal 16 Zeichen lang sein.", "Ungueltige Eingabe", MB_OK | MB_ICONERROR);
#endif
    }
}

bool path_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::filesystem::path get_config_dir() {
#ifdef _WIN32
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
    const std::string& device_id
);

void unlock_device_if_needed(const std::string& device_id);

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
    if (!setup_state.device_ip.empty()) {
        window.set_status_text("Verbinde mit " + setup_state.device_ip + "...");
        if (adb.connect_device(setup_state.device_ip, ADB_TCP_PORT)) {
            if (auto tcp = wait_for_tcp_device(adb, setup_state.device_ip, should_stop)) {
                return tcp;
            }
        }
    }

    if (should_stop) return std::nullopt;

    window.set_status_text("Suche eingerichtetes Geraet im Netzwerk...");
    pm::network::NetworkScanner scanner;
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
    std::atomic<bool>& should_stop
) {
    clear_setup_state();

    window.post_task([&window]() { window.set_status_text("Pruefe USB-Verbindung..."); });
    auto usb_device = wait_for_usb_authorization(adb, window, should_stop);
    if (!usb_device || should_stop) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SCANNING);
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
                window.set_app_state(pm::window::AppState::SCANNING);
                window.set_status_text("Android-App fehlt im PC-Paket. Bitte APK manuell installieren.");
            });
            return false;
        }

        if (!adb.install_app(usb_device->id, apk_path->string())) {
            window.post_task([&window]() {
                window.set_app_state(pm::window::AppState::SCANNING);
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
                window.set_app_state(pm::window::AppState::SCANNING);
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
            window.set_app_state(pm::window::AppState::SCANNING);
            window.set_status_text("Keine WLAN-IP gefunden. Bitte WLAN pruefen.");
        });
        return false;
    }

    window.post_task([&window]() { window.set_status_text("ADB ueber WLAN wird aktiviert..."); });
    if (!adb.enable_tcpip(usb_device->id, ADB_TCP_PORT)) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SCANNING);
            window.set_status_text("WLAN-ADB konnte nicht aktiviert werden.");
        });
        return false;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    window.post_task([&window, device_ip]() { window.set_status_text("Verbinde mit " + device_ip + "..."); });
    if (!adb.connect_device(device_ip, ADB_TCP_PORT)) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SCANNING);
            window.set_status_text("ADB-Verbindung per WLAN fehlgeschlagen.");
        });
        return false;
    }

    auto tcp_device = wait_for_tcp_device(adb, device_ip, should_stop);
    if (!tcp_device || should_stop) {
        window.post_task([&window]() {
            window.set_app_state(pm::window::AppState::SCANNING);
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
    unlock_device_if_needed(tcp_device->id);
    if (!start_stream(window, scrcpy, renderer, input, tcp_device->id)) {
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
    const std::string& device_id
) {
    window.post_task([&window]() { window.set_status_text("Starte Video-Stream..."); });
    pm::stream::ScrcpyClient::Config config;
    config.device_id = device_id;

    // MEOW. WELD SETTINGS TO CONFIG.
    pm::Settings settings = pm::load_settings();
    config.max_fps = settings.max_fps;
    config.max_size = settings.max_size;

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

void unlock_device_if_needed(const std::string& device_id) {
    pm::Settings settings = pm::load_settings();
    if (settings.m_pin.empty()) return;
    
    pm::adb::AdbClient adb;
    
    // Cave man wake screen
    adb.execute_shell_command(device_id, "input keyevent 224");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Cave man hit ENTER to dismiss fingerprint, show PIN pad
    adb.execute_shell_command(device_id, "input keyevent 66");
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    
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
}

}

static int app_main() {
#ifdef _WIN32
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

    std::atomic<bool> should_stop{false};
    pm::stream::ScrcpyClient scrcpy;
    pm::stream::VideoRenderer renderer;
    pm::input::InputHandler input(&scrcpy);

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
            case pm::window::MenuAction::SET_PIN: {
                std::string new_pin = prompt_user_for_pin();
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
                if (!scrcpy.is_running()) {
                    break;
                }
                std::string device_id = scrcpy.get_device_id();
                if (device_id.empty()) {
                    break;
                }
                std::thread([device_id]() {
                    unlock_device_if_needed(device_id);
                }).detach();
                break;
            }
        }
    });

    if (tray) {
        if (!tray->create("Pixel Mirroring", [&]() {
            window->post_task([&]() {
                if (!window->is_visible()) {
                    window->show();
                    if (tray) {
                        tray->hide();
                        // Wake phone. KEYCODE_WAKEUP 224
                        if (scrcpy.is_running()) {
                            scrcpy.inject_keycode(0, 224); // WAKEUP down
                            scrcpy.inject_keycode(1, 224); // WAKEUP up
                        }
                    }
                }
            });
        })) {
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
    std::thread connection_thread;
    std::atomic<bool> connection_running{false};
    std::thread screen_poll_thread;
    std::atomic<bool> stop_screen_poll{false};
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

    auto start_connection = [&](bool automatic) {
        if (connection_running) return;
        
        stop_screen_poll = true;
        if (screen_poll_thread.joinable()) {
            screen_poll_thread.join();
        }

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
                run_first_time_setup(adb, *window, scrcpy, renderer, input, should_stop);
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
                    w->set_app_state(pm::window::AppState::SCANNING);
                    w->set_status_text(automatic
                        ? "Geraet nicht erreichbar. Verbinden fuer erneuten Versuch."
                        : "Geraet nicht erreichbar. USB nur fuer Neueinrichtung.");
                });
                return;
            }

            std::string name = setup_state.device_name;
            window->post_task([w = window.get(), name]() {
                w->set_app_state(pm::window::AppState::CONNECTED);
                w->set_status_text(name.empty() ? "Verbunden" : name);
            });
            std::this_thread::sleep_for(std::chrono::seconds(1));
            unlock_device_if_needed(tcp_device->id);
            if (start_stream(*window, scrcpy, renderer, input, tcp_device->id)) {
                // Parse IP from device ID (e.g. 192.168.1.100:5555)
                std::string device_ip = tcp_device->id;
                auto pos = device_ip.find(':');
                if (pos != std::string::npos) {
                    device_ip = device_ip.substr(0, pos);
                }

                stop_screen_poll = false;
                if (screen_poll_thread.joinable()) {
                    screen_poll_thread.join();
                }

                screen_poll_thread = std::thread([&, device_ip]() {
                    httplib::Client cli(device_ip, 18294);
                    cli.set_connection_timeout(1, 0); // 1s
                    cli.set_read_timeout(1, 500000);   // 1.5s
                    
                    bool last_screen_on = true;
                    
                    while (!should_stop && !stop_screen_poll && scrcpy.is_running()) {
                        auto res = cli.Get("/screen");
                        if (res && res->status == 200) {
                            try {
                                auto resp_json = nlohmann::json::parse(res->body);
                                bool screen_on = resp_json["screenOn"].get<bool>();
                                if (screen_on != last_screen_on) {
                                    last_screen_on = screen_on;
                                    window->post_task([&, screen_on]() {
                                        if (!screen_on) {
                                            if (window->is_visible()) {
                                                window->hide();
                                                if (tray) tray->show();
                                            }
                                        } else {
                                            if (!window->is_visible()) {
                                                window->show();
                                                if (tray) tray->hide();
                                            }
                                        }
                                    });
                                }
                            } catch (...) {
                                // Ignore JSON parse error
                            }
                        }
                        
                        // Sleep 1 second before next poll (using 10x 100ms sleep for responsiveness to stop request)
                        for (int i = 0; i < 10 && !should_stop && !stop_screen_poll && scrcpy.is_running(); ++i) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                    }
                });
            } else if (!should_stop) {
                window->post_task([w = window.get()]() {
                    w->set_app_state(pm::window::AppState::SCANNING);
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
    scrcpy.stop();
    if (connection_thread.joinable()) connection_thread.join();
    if (screen_poll_thread.joinable()) screen_poll_thread.join();
    if (tray) tray->hide();

#ifdef _WIN32
    Gdiplus::GdiplusShutdown(gdiplusToken);
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
