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
    // CAVE MAN HANG REAL USB KEYBOARD ON PHONE INSTEAD OF SHOUTING LETTERS.
    // Off by default: not every phone opens /dev/uhid, and the layout has to be
    // set on the phone once before it is useful.
    bool m_uhid_keyboard = false;
    // A minimized window shows the human NOTHING, while the phone keeps painting
    // 60 pictures a second and throwing them over the WLAN. So the picture-river
    // dries up while the window is down and starts again when it comes back.
    // On by default: nobody wants to pay for a stream they cannot see.
    bool m_auto_pause_minimized = true;
    // EXPERIMENT. Instead of taking the phone's picture away from it, the phone builds
    // a second display that only this PC can see. Phone stays locked and usable by a
    // human at the same time. Needs server 3.x and Android 14+ to be worth anything.
    // ON by default, even though it is a beta road: it is what this build exists for.
    // The menu switch turns it off again and falls back to plain mirroring.
    bool m_virtual_display = true;
    // EXPERIMENT, only meaningful together with the switch above. Empty means "let the
    // phone hang its own launcher into the new display". A package name instead means
    // "no launcher, start THIS on it" — the way out when the phone's launcher for
    // second displays is useless, which is exactly what a Pixel 9 on Android 17 does.
    // No menu entry: this is set by hand in settings.txt while the experiment runs.
    std::string m_virtual_display_app = "";
};

Settings load_settings();
void save_settings(const Settings& s);
std::filesystem::path get_settings_path();

} // namespace pm
