#pragma once
#include <string>
#include <filesystem>

namespace pm {

// MEOW. SETTINGS STRUCT. PRESERVE OPTIONS.
struct Settings {
    int max_fps = 60;       // 60 = unlocked, 30 = limited
    int max_size = 0;       // 0 = full resolution, 720 = 720p
    std::string m_pin = ""; // Saved PIN. Encrypted on Windows.
    bool m_compatibility_mode = false; // CAVE MAN USE SLOW PIN UNLOCK COMPATIBILITY.
    bool m_lowest_brightness = true;   // CAVE MAN MAKE SCREEN BRIGHTNESS VERY LOW AT START
    bool m_send_captures_to_phone = true; // CAVE MAN PUSH FINISHED CAPTURE BACK TO PHONE
};

Settings load_settings();
void save_settings(const Settings& s);
std::filesystem::path get_settings_path();

} // namespace pm
