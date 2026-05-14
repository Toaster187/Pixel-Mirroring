#include <thread>
#include <random>
#include <atomic>

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

static int app_main() {
#ifdef _WIN32
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);
#endif

    // Show window immediately
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
    std::thread connection_thread;

    // Read or generate client ID
    std::string client_id;
    std::string exe_dir = pm::adb::get_executable_dir();
    std::string client_id_path = exe_dir + "/client_id.txt";
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

    window->set_start_callback([&]() {
        if (connection_thread.joinable()) return;

        connection_thread = std::thread([&]() {
            pm::adb::AdbClient adb;

            // Step 1: Init ADB
            window->set_status_text("ADB wird gestartet...");
            adb.init();

            // Step 2: Look for USB device
            window->set_status_text("Suche USB-Geraet...");
            auto devices = adb.get_connected_devices();

            // Find USB device
            std::string usb_device_id;
            for (const auto& dev : devices) {
                if (dev.is_usb() && dev.state == "device") {
                    usb_device_id = dev.id;
                    break;
                }
            }

            if (!usb_device_id.empty()) {
                // USB device found — grant permissions
                window->set_status_text("USB-Geraet gefunden! Berechtigungen...");
                adb.auto_grant_secure_settings();
                window->set_app_state(pm::window::AppState::CONNECTED);
                window->set_status_text("Berechtigungen erteilt!");
                std::this_thread::sleep_for(std::chrono::seconds(2));

                // Now try to start scrcpy on USB device
                if (!should_stop) {
                    window->set_status_text("Starte Stream...");
                    pm::stream::ScrcpyClient::Config config;
                    config.device_id = usb_device_id;

                    if (scrcpy.start(config)) {
                        window->set_app_state(pm::window::AppState::STREAMING);
                        renderer.init(window->get_native_handle());
                        scrcpy.set_frame_callback([&](AVFrame* frame) {
                            renderer.render_frame(nullptr);
                        });
                        window->set_render_callback([&]() {
                            renderer.render_frame(nullptr);
                        });
                    } else {
                        window->set_app_state(pm::window::AppState::STREAMING);
                        window->set_status_text("Stream konnte nicht gestartet werden");
                    }
                }
            } else {
                // No USB device — try network discovery
                window->set_status_text("Kein USB-Geraet. Scanne Netzwerk...");

                pm::network::NetworkScanner scanner;
                auto discovered = scanner.discover_and_connect(client_id, "Desktop-PC");

                if (should_stop) return;

                if (discovered) {
                    window->set_status_text("Gefunden: " + discovered->device_name);
                    adb.connect_device(discovered->ip, discovered->adb_port);

                    // Wait for device ready
                    bool ready = false;
                    for (int i = 0; i < 10 && !should_stop; ++i) {
                        auto devs = adb.get_connected_devices();
                        for (const auto& dev : devs) {
                            if (dev.id.find(discovered->ip) != std::string::npos && dev.state == "device") {
                                ready = true; break;
                            }
                        }
                        if (ready) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                    }

                    if (ready && !should_stop) {
                        window->set_app_state(pm::window::AppState::CONNECTED);
                        window->set_status_text(discovered->device_name);
                        std::this_thread::sleep_for(std::chrono::seconds(1));

                        pm::stream::ScrcpyClient::Config config;
                        auto devs = adb.get_connected_devices();
                        for (const auto& dev : devs) {
                            if (dev.id.find(discovered->ip) != std::string::npos && dev.state == "device") {
                                config.device_id = dev.id; break;
                            }
                        }
                        if (!config.device_id.empty() && scrcpy.start(config)) {
                            window->set_app_state(pm::window::AppState::STREAMING);
                            renderer.init(window->get_native_handle());
                            scrcpy.set_frame_callback([&](AVFrame*) { renderer.render_frame(nullptr); });
                            window->set_render_callback([&]() { renderer.render_frame(nullptr); });
                        } else {
                            window->set_app_state(pm::window::AppState::SETUP);
                            window->set_status_text("Stream fehlgeschlagen");
                        }
                    } else {
                        window->set_app_state(pm::window::AppState::SETUP);
                        window->set_status_text("Geraet nicht bereit");
                    }
                } else {
                    window->set_app_state(pm::window::AppState::SETUP);
                    window->set_status_text("Kein Geraet gefunden. Handy per USB verbinden.");
                }
            }
        });
    });

    window->show();
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
