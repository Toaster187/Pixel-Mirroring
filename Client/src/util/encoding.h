#pragma once

#include <filesystem>
#include <string>

namespace pm::util {

// Ugg! Two letter-tables live in this cave and they do NOT agree.
//
// std::string here is ALWAYS UTF-8 — the window paints with CP_UTF8, adb speaks
// UTF-8, the phone speaks UTF-8. std::filesystem::path on Windows keeps wchar_t
// inside, and its narrow string() hands out the tribe's OLD table (cp1252 on a
// German machine), where "Größe.pdf" loses its ö and Cyrillic turns into "?".
//
// So: never let path::string() near anything that leaves this process. These four
// are the only roads between the two worlds.

// UTF-8 rock-name -> the name shape the file tools speak.
std::filesystem::path path_from_utf8(const std::string& utf8);

// ... and the same road back. Use this INSTEAD of path::string(), always.
std::string path_to_utf8(const std::filesystem::path& path);

#ifdef _WIN32
// For the Win32 calls that only eat wide stones: CreateProcessW and friends.
std::wstring utf8_to_wide(const std::string& utf8);
std::string wide_to_utf8(const std::wstring& wide);
#endif

} // namespace pm::util
