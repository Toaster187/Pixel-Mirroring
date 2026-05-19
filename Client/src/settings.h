#pragma once
#include <string>
#include <filesystem>

namespace pm {

// MEOW. SETTINGS STRUCT. PRESERVE OPTIONS.
struct Settings {
    int max_fps = 60;       // 60 = unlocked, 30 = limited
    int max_size = 0;       // 0 = full resolution, 720 = 720p
};

Settings load_settings();
void save_settings(const Settings& s);
std::filesystem::path get_settings_path();

} // namespace pm
