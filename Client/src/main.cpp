#include <thread>
#include <random>
#include <atomic>
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

std::optional<pm::adb::Device> wait_for_usb_authorization(
    pm::adb::AdbClient& adb,
    pm::window::IWindow& window,
    std::atomic<bool>& should_stop
) {
    for (int i = 0; i < 60 && !should_stop; ++i) {
        auto devices = adb.get_devices();
        if (auto ready = find_usb_device(devices, "device")) {
            return ready;
        }

        auto usb = find_usb_device(devices);
        if (!usb) {
            window.set_status_text("Warte auf USB-Geraet...");
        } else {
            if (usb->state == "unauthorized") {
                window.set_status_text("Bitte USB-Debugging auf dem Handy erlauben.");
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

    window.set_status_text("Pruefe USB-Verbindung...");
    auto usb_device = wait_for_usb_authorization(adb, window, should_stop);
    if (!usb_device || should_stop) {
        window.set_app_state(pm::window::AppState::SETUP);
        window.set_status_text("Kein USB-Geraet bereit. Bitte erneut verbinden.");
        return false;
    }

    window.set_status_text("Android-App wird installiert...");
    auto apk_path = find_android_apk();
    if (!apk_path) {
        window.set_app_state(pm::window::AppState::SETUP);
        window.set_status_text("Android-App fehlt im PC-Paket.");
        return false;
    }

    if (!adb.install_app(usb_device->id, apk_path->string())) {
        window.set_app_state(pm::window::AppState::SETUP);
        window.set_status_text("Android-App konnte nicht installiert werden.");
        return false;
    }

    window.set_status_text("Berechtigungen werden gesetzt...");
    if (!adb.grant_secure_settings(usb_device->id)) {
        window.set_app_state(pm::window::AppState::SETUP);
        window.set_status_text("WRITE_SECURE_SETTINGS konnte nicht gesetzt werden.");
        return false;
    }

    window.set_status_text("Android-App wird gestartet...");
    adb.start_app(usb_device->id, ANDROID_PACKAGE);
    adb.start_service(usb_device->id, ANDROID_SERVICE);

    const std::string device_ip = adb.get_device_ip(usb_device->id);
    if (device_ip.empty()) {
        window.set_app_state(pm::window::AppState::SETUP);
        window.set_status_text("Keine WLAN-IP gefunden. Bitte WLAN pruefen.");
        return false;
    }

    window.set_status_text("ADB ueber WLAN wird aktiviert...");
    if (!adb.enable_tcpip(usb_device->id, ADB_TCP_PORT)) {
        window.set_app_state(pm::window::AppState::SETUP);
        window.set_status_text("WLAN-ADB konnte nicht aktiviert werden.");
        return false;
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    window.set_status_text("Verbinde mit " + device_ip + "...");
    if (!adb.connect_device(device_ip, ADB_TCP_PORT)) {
        window.set_app_state(pm::window::AppState::SETUP);
        window.set_status_text("ADB-Verbindung per WLAN fehlgeschlagen.");
        return false;
    }

    auto tcp_device = wait_for_tcp_device(adb, device_ip, should_stop);
    if (!tcp_device || should_stop) {
        window.set_app_state(pm::window::AppState::SETUP);
        window.set_status_text("WLAN-ADB ist noch nicht bereit.");
        return false;
    }

    SetupState setup_state;
    setup_state.configured = true;
    setup_state.device_ip = device_ip;
    setup_state.device_name = usb_device->model.empty() ? "Android" : usb_device->model;

    window.set_app_state(pm::window::AppState::CONNECTED);
    window.set_status_text("Verbunden: " + device_ip);
    std::this_thread::sleep_for(std::chrono::seconds(1));
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
    window.set_status_text("Starte Video-Stream...");
    pm::stream::ScrcpyClient::Config config;
    config.device_id = device_id;

    renderer.init(window.get_native_handle());
    scrcpy.set_frame_callback([&](AVFrame* frame) {
        renderer.render_frame(frame);
    });
    window.set_render_callback([&](void* hdc, int x, int y, int w, int h) {
        renderer.paint(hdc, x, y, w, h);
    });

    if (!scrcpy.start(config)) {
        window.set_app_state(pm::window::AppState::SETUP);
        window.set_status_text("Stream konnte nicht gestartet werden.");
        return false;
    }

    input.set_device_size(scrcpy.video_width(), scrcpy.video_height());
    window.set_aspect_ratio((double)scrcpy.video_width() / (double)scrcpy.video_height());
    window.set_orientation(scrcpy.video_width() > scrcpy.video_height());
    window.set_app_state(pm::window::AppState::STREAMING);
    return true;
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
    window->set_aspect_ratio(340.0 / 604.0);
    window->set_app_state(pm::window::AppState::SETUP);
    window->set_status_text("");

    std::atomic<bool> should_stop{false};
    pm::stream::ScrcpyClient scrcpy;
    pm::stream::VideoRenderer renderer;
    pm::input::InputHandler input(&scrcpy);
    window->set_video_viewport_callback([&](int x, int y, int w, int h) {
        renderer.update_viewport(x, y, w, h);
    });
    window->set_pointer_callback([&](pm::window::PointerAction action, int x, int y, int w, int h) {
        input.handle_pointer(action, x, y, w, h);
    });
    std::thread connection_thread;
    std::atomic<bool> connection_running{false};
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
        if (connection_thread.joinable()) {
            connection_thread.join();
        }

        connection_thread = std::thread([&, automatic]() {
            connection_running = true;
            ScopeExit mark_done([&]() { connection_running = false; });
            pm::adb::AdbClient adb;

            // Ugg wake ADB first. No pretend network magic.
            window->set_status_text("ADB wird gestartet...");
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
                window->set_app_state(pm::window::AppState::SETUP);
                window->set_status_text(automatic
                    ? "Geraet nicht erreichbar. Verbinden fuer erneuten Versuch."
                    : "Geraet nicht erreichbar. USB nur fuer Neueinrichtung.");
                return;
            }

            window->set_app_state(pm::window::AppState::CONNECTED);
            window->set_status_text(setup_state.device_name.empty() ? "Verbunden" : setup_state.device_name);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!start_stream(*window, scrcpy, renderer, input, tcp_device->id) && !should_stop) {
                window->set_app_state(pm::window::AppState::SETUP);
                window->set_status_text("Stream fehlgeschlagen.");
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
    scrcpy.stop();
    if (connection_thread.joinable()) connection_thread.join();

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
