#pragma once

#include <filesystem>
#include <string>

namespace pm::util {

// Two text encodings meet in this program and they do NOT agree.
//
// std::string here is ALWAYS UTF-8 — the window renders with CP_UTF8, adb speaks
// UTF-8, the device speaks UTF-8. std::filesystem::path on Windows stores wchar_t
// internally, and its narrow string() returns the ANSI code page (cp1252 on a German
// machine), where "Größe.pdf" loses its ö and Cyrillic becomes "?".
//
// So: never let path::string() near anything that leaves this process. These four
// functions are the only conversions between the two worlds.

// UTF-8 file name -> the representation std::filesystem expects.
std::filesystem::path path_from_utf8(const std::string& utf8);

// ... and the way back. Use this INSTEAD of path::string(), always.
std::string path_to_utf8(const std::filesystem::path& path);

#ifdef _WIN32
// For the Win32 calls that only take wide strings: CreateProcessW and friends.
std::wstring utf8_to_wide(const std::string& utf8);
std::string wide_to_utf8(const std::wstring& wide);
#endif

} // namespace pm::util
