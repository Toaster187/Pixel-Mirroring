#include <iostream>
#include <thread>
#include <random>
#include "adb/adb_client.h"
#include "window/window_interface.h"
#include "stream/scrcpy_client.h"
#include "stream/video_decoder.h"
#include "stream/video_renderer.h"
#include "input/input_handler.h"
#include "network/network_scanner.h"

int main(int argc, char* argv[]) {
    std::cout << "=== Pixel Mirroring Desktop Client ===" << std::endl;

    pm::adb::AdbClient adb;
    
    if (!adb.init()) {
        std::cerr << "Failed to initialize ADB." << std::endl;
        return 1;
    }

    std::cout << "Checking for USB devices for initial setup..." << std::endl;
    
    if (adb.auto_grant_secure_settings()) {
        std::cout << "\nSetup complete! The Android app now has the necessary permissions." << std::endl;
    } else {
        std::cout << "\nNo USB device found, skipping auto-setup." << std::endl;
    }

    std::cout << "\nScanning for Android devices on the network..." << std::endl;
    pm::network::NetworkScanner scanner;
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> distrib(100000, 999999);
    std::string client_id = "desktop-client-" + std::to_string(distrib(gen));
    
    auto discovered = scanner.discover_and_connect(client_id, "Desktop-PC");
    
    if (discovered) {
        std::cout << "Attempting to connect ADB to " << discovered->ip << ":" << discovered->adb_port << "..." << std::endl;
        adb.connect_device(discovered->ip, discovered->adb_port);
        
        std::cout << "Waiting for ADB device to become ready..." << std::endl;
        bool device_ready = false;
        for (int i = 0; i < 10; ++i) {
            auto devs = adb.get_connected_devices();
            for (const auto& dev : devs) {
                if (dev.id.find(discovered->ip) != std::string::npos && dev.state == "device") {
                    device_ready = true;
                    break;
                }
            }
            if (device_ready) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    } else {
        std::cout << "Could not discover the Android app. Make sure it is open and connected to WiFi." << std::endl;
    }

    std::cout << "\nStarting Native UI..." << std::endl;

    auto window = pm::window::create_window(340, 604, "Pixel Mirroring");
    if (!window->create()) {
        std::cerr << "Failed to create window." << std::endl;
        return 1;
    }

    // Initialize backend components
    pm::stream::ScrcpyClient scrcpy;
    pm::stream::VideoRenderer renderer;
    pm::input::InputHandler input(&scrcpy);

    auto devices = adb.get_connected_devices();
    if (!devices.empty()) {
        pm::stream::ScrcpyClient::Config config;
        config.device_id = devices[0].id;
        
        if (scrcpy.start(config)) {
            renderer.init(window->get_native_handle());
            
            scrcpy.set_frame_callback([&](AVFrame* frame) {
                // Ignore compilation error for AVFrame here since it's a stub
                renderer.render_frame(nullptr);
            });
            
            window->set_render_callback([&]() {
                renderer.render_frame(nullptr);
            });
        }
    }

    window->set_aspect_ratio(340.0 / 604.0); // Typical portrait aspect ratio
    window->show();
    window->process_messages();

    scrcpy.stop();
    return 0;
}
