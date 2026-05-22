#include "settings.h"
#include "adb/adb_client.h"
#include <fstream>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pm {

// MEOW. LOCAL CONFIG DIR FOR SETTINGS.
static std::filesystem::path get_config_dir() {
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

// MEOW. PATH FOR SETTINGS FILE.
std::filesystem::path get_settings_path() {
    return get_config_dir() / "settings.txt";
}

// MEOW. LOAD SETTINGS FROM FILE.
Settings load_settings() {
    Settings s;
    std::ifstream file(get_settings_path());
    if (!file) {
        return s;
    }

    std::string line;
    while (std::getline(file, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto value = line.substr(eq + 1);
        if (key == "max_fps") {
            try {
                s.max_fps = std::stoi(value);
            } catch (...) {}
        } else if (key == "max_size") {
            try {
                s.max_size = std::stoi(value);
            } catch (...) {}
        }
    }
    return s;
}

// MEOW. SAVE SETTINGS TO FILE.
void save_settings(const Settings& s) {
    std::ofstream file(get_settings_path(), std::ios::trunc);
    if (!file) {
        return;
    }
    file << "max_fps=" << s.max_fps << "\n";
    file << "max_size=" << s.max_size << "\n";
}

} // namespace pm
