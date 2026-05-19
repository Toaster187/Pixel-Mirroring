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
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>
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
#ifdef _WIN32
    std::string adb_filename = "adb.exe";
#else
    std::string adb_filename = "adb";
#endif
    std::filesystem::path sibling_adb = std::filesystem::path(exe_dir) / ".." / "scrcpy_download" / adb_filename;
    if (std::filesystem::exists(sibling_adb)) {
        return sibling_adb.string();
    }

    std::filesystem::path current = std::filesystem::path(exe_dir);
    for (int i = 0; i < 6; ++i) {
        current = current.parent_path();
        if (current.empty()) break;
        std::filesystem::path check_adb = current / "scrcpy_download" / adb_filename;
        if (std::filesystem::exists(check_adb)) {
            return check_adb.string();
        }
        std::filesystem::path bundled_adb = current / "platform-tools" / adb_filename;
        if (std::filesystem::exists(bundled_adb)) {
            return bundled_adb.string();
        }
    }
    
    return "adb"; // developer fallback only
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
    std::string adb_path = get_adb_path();
    std::string result;

#ifdef _WIN32
    // Build command line for Windows
    std::string cmdline = "\"" + adb_path + "\"";
    for (const auto& arg : args) {
        cmdline += " ";
        // Quote arguments that contain spaces
        if (arg.find(' ') != std::string::npos) {
            cmdline += "\"" + arg + "\"";
        } else {
            cmdline += arg;
        }
    }

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hChildStdoutRd, hChildStdoutWr;
    if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &saAttr, 0)) {
        // Pipe no work. Sad caveman noises.
        std::cerr << "[ADB] CreatePipe failed" << std::endl;
        return "";
    }
    SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb = sizeof(STARTUPINFOA);
    si.hStdOutput = hChildStdoutWr;
    si.hStdError = hChildStdoutWr;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {0};

    std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back('\0');

    if (!CreateProcessA(NULL, cmdline_buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hChildStdoutRd);
        CloseHandle(hChildStdoutWr);
        // ADB process no spawn. Like trying to make fire in rain.
        std::cerr << "[ADB] CreateProcess failed for: " << cmdline << std::endl;
        return "";
    }

    CloseHandle(hChildStdoutWr);

    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hChildStdoutRd, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        result += buffer;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hChildStdoutRd);
#else
    // POSIX: use fork+execvp
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        std::cerr << "[ADB] pipe() failed" << std::endl;
        return "";
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        std::cerr << "[ADB] fork() failed" << std::endl;
        return "";
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]); // Close read end
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        // Build argv
        std::vector<const char*> argv;
        argv.push_back(adb_path.c_str());
        for (const auto& arg : args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execvp(adb_path.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127); // exec failed
    } else {
        // Parent process
        close(pipefd[1]); // Close write end

        char buffer[4096];
        ssize_t bytesRead;
        while ((bytesRead = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytesRead] = '\0';
            result += buffer;
        }

        close(pipefd[0]);
        waitpid(pid, nullptr, 0);
    }
#endif

    return result;
}

std::vector<Device> AdbClient::get_devices() {
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

        if (!id.empty() && !state.empty()) {
            Device dev;
            dev.id = id;
            dev.state = state;

            // Cave man read model mark from adb stone.
            std::smatch match;
            if (std::regex_search(line, match, std::regex("model:([^\\s]+)"))) {
                dev.model = match[1].str();
            }

            devices.push_back(dev);
        }
    }
    return devices;
}

void AdbClient::reconnect_offline() {
    run_adb_command({"reconnect", "offline"});
}

std::vector<Device> AdbClient::get_connected_devices() {
    auto devices = get_devices();
    devices.erase(
        std::remove_if(devices.begin(), devices.end(), [](const Device& device) {
            return device.state != "device";
        }),
        devices.end()
    );
    return devices;
}

bool AdbClient::connect_device(const std::string& ip, int port) {
    std::string target = ip + ":" + std::to_string(port);
    std::cout << "[ADB] Connecting to " << target << "..." << std::endl;
    
    // Add retry loop to handle daemon startup delay
    int max_retries = 10;
    for (int i = 0; i < max_retries; ++i) {
        try {
            std::string output = run_adb_command({"connect", target});

            // Output looks like "connected to 192.168.1.5:5555" or "cannot connect to..."
            if (output.find("connected to") != std::string::npos || output.find("already connected") != std::string::npos) {
                std::cout << "[ADB] Successfully connected to " << target << std::endl;
                return true;
            }

            std::cerr << "[ADB] Attempt " << (i+1) << " failed to connect: " << output << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "[ADB] Attempt " << (i+1) << " threw exception: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[ADB] Attempt " << (i+1) << " threw unknown exception" << std::endl;
        }

        if (i < max_retries - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
    return false;
}

bool AdbClient::enable_tcpip(const std::string& device_id, int port) {
    std::string output = run_adb_command({"-s", device_id, "tcpip", std::to_string(port)});
    if (output.find("restarting in TCP mode") != std::string::npos ||
        output.find("restarting in TCP") != std::string::npos) {
        return true;
    }

    std::cerr << "[ADB] Failed to enable TCP/IP: " << output << std::endl;
    return false;
}

bool AdbClient::install_app(const std::string& device_id, const std::string& apk_path) {
    if (!std::filesystem::exists(apk_path)) {
        std::cerr << "[ADB] APK not found: " << apk_path << std::endl;
        return false;
    }

    std::string output = run_adb_command({"-s", device_id, "install", "-r", "-d", apk_path});
    if (output.find("Success") != std::string::npos) {
        return true;
    }

    std::cerr << "[ADB] App install failed: " << output << std::endl;
    return false;
}

bool AdbClient::start_app(const std::string& device_id, const std::string& package_name) {
    std::string output = run_adb_command({
        "-s", device_id, "shell", "monkey -p " + package_name + " -c android.intent.category.LAUNCHER 1"
    });
    return output.find("Events injected: 1") != std::string::npos ||
           output.find("monkey aborted") == std::string::npos;
}

bool AdbClient::start_service(const std::string& device_id, const std::string& service_name) {
    std::string output = run_adb_command({
        "-s", device_id, "shell", "am start-foreground-service -n " + service_name
    });
    return output.find("Error") == std::string::npos &&
           output.find("Exception") == std::string::npos &&
           output.find("not found") == std::string::npos;
}

std::string AdbClient::execute_shell_command(const std::string& device_id, const std::string& command) {
    return run_adb_command({"-s", device_id, "shell", command});
}

void AdbClient::execute_shell_command_async(const std::string& device_id, const std::string& command, std::function<void(const std::string&)> on_line) {
    std::string adb_path = get_adb_path();

#ifdef _WIN32
    // Build command line for Windows
    std::string cmdline = "\"" + adb_path + "\" -s " + device_id + " shell " + command;

    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hChildStdoutRd, hChildStdoutWr;
    if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &saAttr, 0)) {
        return;
    }
    SetHandleInformation(hChildStdoutRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb = sizeof(STARTUPINFOA);
    si.hStdOutput = hChildStdoutWr;
    si.hStdError = hChildStdoutWr;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {0};

    std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
    cmdline_buf.push_back('\0');

    if (!CreateProcessA(NULL, cmdline_buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hChildStdoutRd);
        CloseHandle(hChildStdoutWr);
        return;
    }

    CloseHandle(hChildStdoutWr);

    char buffer[4096];
    DWORD bytesRead;
    std::string line;
    while (ReadFile(hChildStdoutRd, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        line += buffer;
        size_t pos;
        while ((pos = line.find('\n')) != std::string::npos) {
            if (on_line) on_line(line.substr(0, pos + 1));
            line.erase(0, pos + 1);
        }
    }
    if (!line.empty() && on_line) on_line(line);

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hChildStdoutRd);
#else
    // POSIX: use fork+execvp
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        // Child process
        close(pipefd[0]); // Close read end
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        // Build argv
        std::vector<const char*> argv;
        argv.push_back(adb_path.c_str());
        argv.push_back("-s");
        argv.push_back(device_id.c_str());
        argv.push_back("shell");
        argv.push_back(command.c_str());
        argv.push_back(nullptr);

        execvp(adb_path.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127); // exec failed
    } else {
        // Parent process
        close(pipefd[1]); // Close write end

        char buffer[4096];
        ssize_t bytesRead;
        std::string line;
        while ((bytesRead = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytesRead] = '\0';
            line += buffer;
            size_t pos;
            while ((pos = line.find('\n')) != std::string::npos) {
                if (on_line) on_line(line.substr(0, pos + 1));
                line.erase(0, pos + 1);
            }
        }
        if (!line.empty() && on_line) on_line(line);

        close(pipefd[0]);
        waitpid(pid, nullptr, 0);
    }
#endif
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
    std::string output = run_adb_command({"-s", device_id, "forward", "--remove", local});
    if (output.find("error:") != std::string::npos) return false;
    return true;
}

bool AdbClient::remove_reverse(const std::string& device_id, const std::string& remote) {
    std::string output = run_adb_command({"-s", device_id, "reverse", "--remove", remote});
    if (output.find("error:") != std::string::npos) return false;
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
    return grant_secure_settings(usb_device->id);
}

bool AdbClient::grant_secure_settings(const std::string& device_id) {
    std::cout << "Granting WRITE_SECURE_SETTINGS..." << std::endl;

    std::string output = execute_shell_command(
        device_id,
        "pm grant dev.pixelmirroring.app android.permission.WRITE_SECURE_SETTINGS"
    );

    // Usually ADB returns empty on success for pm grant.
    if (output.find("Exception") != std::string::npos || output.find("Error") != std::string::npos) {
        std::cerr << "Failed to grant permission: " << output << std::endl;
        return false;
    }

    std::cout << "Permission granted successfully!" << std::endl;
    return true;
}

std::string AdbClient::get_device_ip(const std::string& device_id) {
    std::string output = execute_shell_command(device_id, "ip route");
    std::smatch route_match;
    if (std::regex_search(output, route_match, std::regex("\\bsrc\\s+([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)"))) {
        return route_match[1].str();
    }

    output = execute_shell_command(device_id, "ip -f inet addr show wlan0");
    std::smatch wlan_match;
    if (std::regex_search(output, wlan_match, std::regex("\\binet\\s+([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)/"))) {
        return wlan_match[1].str();
    }

    return "";
}

} // namespace pm::adb
