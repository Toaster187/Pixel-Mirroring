#include "encoding.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace pm::util {

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                        &wide[0], len);
    return wide;
}

std::string wide_to_utf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                        static_cast<int>(wide.size()),
                                        nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string utf8(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()),
                        &utf8[0], len, nullptr, nullptr);
    return utf8;
}

std::filesystem::path path_from_utf8(const std::string& utf8) {
    // A failed conversion would leave nothing at all. A possibly mangled name is still
    // better than an empty one, so fall back to the narrow conversion.
    const std::wstring wide = utf8_to_wide(utf8);
    if (wide.empty() && !utf8.empty()) return std::filesystem::path(utf8);
    return std::filesystem::path(wide);
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty()) return std::string();
    const std::string utf8 = wide_to_utf8(wide);
    if (utf8.empty()) return path.string();
    return utf8;
}

#else

// Outside Windows everything is UTF-8 already, so there is nothing to convert.
std::filesystem::path path_from_utf8(const std::string& utf8) {
    return std::filesystem::path(utf8);
}

std::string path_to_utf8(const std::filesystem::path& path) {
    return path.string();
}

#endif

} // namespace pm::util
