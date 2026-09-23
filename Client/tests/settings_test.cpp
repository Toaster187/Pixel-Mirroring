#include "../src/settings.h"
#include "../src/util/encoding.h"
#include <chrono>
#include <cstdio>
#include <fstream>

static std::filesystem::path test_directory;
// Portable settings use this directory; no real user settings are read or written.
namespace pm::adb {
std::string get_executable_dir() { return pm::util::path_to_utf8(test_directory); }
}

int main() {
    test_directory = std::filesystem::temp_directory_path() /
        ("pm-settings-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(test_directory);
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        if (!ok) { std::printf("FAIL: %s\n", name); ++failures; }
    };
    auto write = [&](const char* contents) {
        std::ofstream file(test_directory / "settings.txt");
        file << contents;
    };
    check(pm::load_settings().m_virtual_display, "fresh install uses virtual display");
    write("audio_enabled=0\nmax_fps=30\n");
    auto old = pm::load_settings();
    check(!old.m_virtual_display, "upgrade preserves mirroring");
    check(!old.m_audio_enabled, "upgrade preserves other settings");
    check(old.m_quality == pm::QualityPreset::BATTERY, "legacy quality migrates");
    write("virtual_display=1\nvirtual_display_app=  org.example.home \t\n");
    auto enabled = pm::load_settings();
    check(enabled.m_virtual_display, "explicit opt-in wins");
    check(enabled.m_virtual_display_app == "org.example.home", "package name is trimmed");
    pm::save_settings(enabled);
    check(pm::load_settings().m_virtual_display, "opt-in survives save and reload");
    write("virtual_display=0\n");
    check(!pm::load_settings().m_virtual_display, "explicit opt-out wins");
    std::filesystem::remove(test_directory / "settings.txt");
    std::filesystem::remove(test_directory);
    std::printf("settings: %d failures\n", failures);
    return failures ? 1 : 0;
}
