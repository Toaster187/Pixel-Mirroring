#pragma once
#include <string>
#include <filesystem>

namespace pm {

// MEOW. THREE STONES INSTEAD OF TWO SWITCHES. CAVE MAN PICK ONE, ALL NUMBERS FOLLOW.
enum class QualityPreset {
    BATTERY  = 0, // Akku sparen
    BALANCED = 1, // Ausgewogen — was the old default, stays the default
    MAXIMUM  = 2  // Maximal
};

// What a preset actually means on the wire.
struct QualitySpec {
    int max_fps;
    int max_size;       // 0 = full resolution
    int video_bit_rate; // bit/s
};

QualitySpec quality_spec(QualityPreset preset);

// MEOW. SETTINGS STRUCT. PRESERVE OPTIONS.
struct Settings {
    QualityPreset m_quality = QualityPreset::BALANCED;
    std::string m_pin = ""; // Saved PIN. Encrypted on Windows.
    bool m_compatibility_mode = false; // CAVE MAN USE SLOW PIN UNLOCK COMPATIBILITY.
    bool m_lowest_brightness = true;   // CAVE MAN MAKE SCREEN BRIGHTNESS VERY LOW AT START
    // The honest version of the line above: the phone's panel goes dark for real.
    // Opt-in only — a black phone that nobody asked for is a broken phone.
    bool m_screen_off = false;
    bool m_send_captures_to_phone = true; // CAVE MAN PUSH FINISHED CAPTURE BACK TO PHONE
    bool m_audio_enabled = true;       // CAVE MAN HEAR PHONE SOUND ON PC SPEAKER
};

Settings load_settings();
void save_settings(const Settings& s);
std::filesystem::path get_settings_path();

} // namespace pm
