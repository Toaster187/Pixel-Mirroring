#include "adb_client.h"

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <array>
#include <memory>
#include <regex>
#include <filesystem>
#include <thread>
#include <chrono>

#ifdef _WIN32
#include <windows.h>
#define POPEN _popen
#define PCLOSE _pclose
#else
#include <unistd.h>
#include <limits.h>
#define POPEN popen
#define PCLOSE pclose
#endif

namespace pm::adb {

std::string get_executable_dir() {
#ifdef _WIN32
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().string();
#else
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    if (count != -1) {
        return std::filesystem::path(std::string(result, count)).parent_path().string();
    }
    return ".";
#endif
}

std::string get_adb_path() {
    std::string exe_dir = get_executable_dir();
#ifdef _WIN32
    std::filesystem::path local_adb = std::filesystem::path(exe_dir) / "adb.exe";
#else
    std::filesystem::path local_adb = std::filesystem::path(exe_dir) / "adb";
#endif
    if (std::filesystem::exists(local_adb)) {
        return local_adb.string();
    }
    // Check scrcpy_download folder relative to exe
    std::filesystem::path sibling_adb = std::filesystem::path(exe_dir) / ".." / "scrcpy_download" / "adb.exe";
    if (std::filesystem::exists(sibling_adb)) {
        return sibling_adb.string();
    }
    
    std::filesystem::path current = std::filesystem::path(exe_dir);
    for (int i = 0; i < 3; ++i) {
        current = current.parent_path();
        std::filesystem::path check_adb = current / "scrcpy_download" / "adb.exe";
        if (std::filesystem::exists(check_adb)) {
            return check_adb.string();
        }
    }
    
    return "adb"; // fallback to PATH
}

bool Device::is_usb() const {
    // ADB over TCP/IP usually contains an IP address format, USB does not
    return id.find('.') == std::string::npos;
}

bool Device::is_tcp() const {
    return !is_usb();
}

AdbClient::AdbClient() {
}

AdbClient::~AdbClient() {
}

bool AdbClient::init() {
    // Start ADB server just in case
    run_adb_command({"start-server"});
    return true;
}

std::string AdbClient::run_adb_command(const std::vector<std::string>& args) {
    std::string command = "\"" + get_adb_path() + "\"";
    for (const auto& arg : args) {
        command += " " + arg;
    }

    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&PCLOSE)> pipe(POPEN(command.c_str(), "r"), PCLOSE);
    
    if (!pipe) {
        throw std::runtime_error("popen() failed!");
    }
    
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

std::vector<Device> AdbClient::get_connected_devices() {
    std::vector<Device> devices;
    std::string output = run_adb_command({"devices", "-l"});
    
    std::istringstream stream(output);
    std::string line;
    
    // Skip the first line ("List of devices attached")
    std::getline(stream, line);
    
    while (std::getline(stream, line)) {
        if (line.empty() || line.find_first_not_of(" \t\r\n") == std::string::npos) {
            continue;
        }

        std::istringstream linestream(line);
        std::string id, state;
        linestream >> id >> state;

        if (state == "device") {
            Device dev;
            dev.id = id;
            dev.state = state;
            
            // Extract model if available
            std::smatch match;
            if (std::regex_search(line, match, std::regex("model:([^\\s]+)"))) {
                dev.model = match[1].str();
            }
            
            devices.push_back(dev);
        }
    }
    
    return devices;
}

bool AdbClient::connect_device(const std::string& ip, int port) {
    std::string target = ip + ":" + std::to_string(port);
    std::cout << "[ADB] Connecting to " << target << "..." << std::endl;
    
    // Add retry loop to handle daemon startup delay
    int max_retries = 10;
    for (int i = 0; i < max_retries; ++i) {
        std::string output = run_adb_command({"connect", target});
        
        // Output looks like "connected to 192.168.1.5:5555" or "cannot connect to..."
        if (output.find("connected to") != std::string::npos || output.find("already connected") != std::string::npos) {
            std::cout << "[ADB] Successfully connected to " << target << std::endl;
            return true;
        }
        
        std::cerr << "[ADB] Attempt " << (i+1) << " failed to connect: " << output << std::endl;
        if (i < max_retries - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
    return false;
}

std::string AdbClient::execute_shell_command(const std::string& device_id, const std::string& command) {
    return run_adb_command({"-s", device_id, "shell", command});
}

void AdbClient::execute_shell_command_async(const std::string& device_id, const std::string& command, std::function<void(const std::string&)> on_line) {
    std::string full_command = "\"" + get_adb_path() + "\" -s " + device_id + " shell " + command;
    std::array<char, 128> buffer;
    std::unique_ptr<FILE, decltype(&PCLOSE)> pipe(POPEN(full_command.c_str(), "r"), PCLOSE);
    if (!pipe) return;
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        if (on_line) on_line(buffer.data());
    }
}

bool AdbClient::push_file(const std::string& device_id, const std::string& local_path, const std::string& remote_path) {
    std::string output = run_adb_command({"-s", device_id, "push", local_path, remote_path});
    if (output.find("error:") != std::string::npos || output.find("failed to copy") != std::string::npos) {
        std::cerr << "[ADB] Failed to push file: " << output << std::endl;
        return false;
    }
    return true;
}

bool AdbClient::forward_port(const std::string& device_id, const std::string& local, const std::string& remote) {
    std::string output = run_adb_command({"-s", device_id, "forward", local, remote});
    if (output.find("error:") != std::string::npos) return false;
    return true;
}

bool AdbClient::reverse_port(const std::string& device_id, const std::string& remote, const std::string& local) {
    std::string output = run_adb_command({"-s", device_id, "reverse", remote, local});
    if (output.find("error:") != std::string::npos) return false;
    return true;
}

bool AdbClient::remove_forward(const std::string& device_id, const std::string& local) {
    run_adb_command({"-s", device_id, "forward", "--remove", local});
    return true;
}

bool AdbClient::remove_reverse(const std::string& device_id, const std::string& remote) {
    run_adb_command({"-s", device_id, "reverse", "--remove", remote});
    return true;
}

bool AdbClient::auto_grant_secure_settings() {
    auto devices = get_connected_devices();
    
    Device* usb_device = nullptr;
    for (auto& dev : devices) {
        if (dev.is_usb()) {
            usb_device = &dev;
            break;
        }
    }

    if (!usb_device) {
        std::cout << "No USB device found. Please connect your Android device via USB." << std::endl;
        return false;
    }

    std::cout << "Found USB device: " << usb_device->model << " (" << usb_device->id << ")" << std::endl;
    std::cout << "Granting WRITE_SECURE_SETTINGS..." << std::endl;

    std::string output = execute_shell_command(
        usb_device->id, 
        "pm grant dev.pixelmirroring.app android.permission.WRITE_SECURE_SETTINGS"
    );

    // Usually ADB returns empty on success for pm grant
    if (output.find("Exception") != std::string::npos || output.find("Error") != std::string::npos) {
        std::cerr << "Failed to grant permission: " << output << std::endl;
        return false;
    }

    std::cout << "Permission granted successfully!" << std::endl;
    return true;
}

} // namespace pm::adb
