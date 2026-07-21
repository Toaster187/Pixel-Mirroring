#include "settings.h"
#include "adb/adb_client.h"
#include <fstream>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace pm {

// MEOW. LOCAL CONFIG DIR FOR SETTINGS.
static std::filesystem::path get_config_dir() {
#ifndef PM_PORTABLE_BUILD
#ifdef _WIN32
    // CAVE MAN NO WRITE APPDATA FOR PORTABLE. ONLY EXE DIR.
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

#ifdef _WIN32
static std::string encrypt_pin(const std::string& pin) {
    if (pin.empty()) return "";
    DATA_BLOB input;
    input.pbData = (BYTE*)pin.data();
    input.cbData = (DWORD)pin.size();
    DATA_BLOB output;
    if (CryptProtectData(&input, L"PixelMirroringPin", nullptr, nullptr, nullptr, 0, &output)) {
        std::string hex;
        hex.reserve(output.cbData * 2);
        for (DWORD i = 0; i < output.cbData; ++i) {
            char buf[3];
            sprintf_s(buf, "%02x", output.pbData[i]);
            hex += buf;
        }
        LocalFree(output.pbData);
        return hex;
    }
    return "";
}

static std::string decrypt_pin(const std::string& hex) {
    if (hex.empty() || hex.size() % 2 != 0) return "";
    std::vector<BYTE> encrypted_data;
    encrypted_data.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        std::string byteString = hex.substr(i, 2);
        BYTE b = (BYTE)strtol(byteString.c_str(), nullptr, 16);
        encrypted_data.push_back(b);
    }
    DATA_BLOB input;
    input.pbData = encrypted_data.data();
    input.cbData = (DWORD)encrypted_data.size();
    DATA_BLOB output;
    if (CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, 0, &output)) {
        std::string pin((char*)output.pbData, output.cbData);
        LocalFree(output.pbData);
        return pin;
    }
    return "";
}
#else
static std::string encrypt_pin(const std::string& pin) { return pin; }
static std::string decrypt_pin(const std::string& hex) { return hex; }
#endif

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
        } else if (key == "compatibility_mode") {
            s.m_compatibility_mode = (value == "1");
        } else if (key == "turn_screen_off" || key == "lowest_brightness") {
            s.m_lowest_brightness = (value == "1");
        } else if (key == "send_captures_to_phone") {
            s.m_send_captures_to_phone = (value == "1");
        } else if (key == "pin") {
            s.m_pin = decrypt_pin(value);
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
    file << "compatibility_mode=" << (s.m_compatibility_mode ? "1" : "0") << "\n";
    file << "lowest_brightness=" << (s.m_lowest_brightness ? "1" : "0") << "\n";
    file << "send_captures_to_phone=" << (s.m_send_captures_to_phone ? "1" : "0") << "\n";
    file << "pin=" << encrypt_pin(s.m_pin) << "\n";
}

} // namespace pm
