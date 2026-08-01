#include "adb_client.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <array>
#include <cctype>
#include <memory>
#include <filesystem>
#include <thread>
#include <chrono>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <limits.h>
#include <csignal>
#include <sys/wait.h>
#endif

namespace pm::adb {

namespace {

// Ugg! GUI app has no console. Words to stderr fall in deep hole and are lost forever.
// So carve ADB words into stone tablet next to stream.log, where human can read them.
void log_adb_event(const std::string& message) {
    std::cerr << message << std::endl;

    std::filesystem::path log_dir;
#ifndef PM_PORTABLE_BUILD
#ifdef _WIN32
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data && local_app_data[0] != '\0') {
        log_dir = std::filesystem::path(local_app_data) / "PixelMirroring";
    }
#endif
#endif
    if (log_dir.empty()) {
        log_dir = get_executable_dir();
    }

    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    std::ofstream file(log_dir / "adb.log", std::ios::app);
    if (file) {
        file << message << "\n";
    }
}

// Cave man rubs whitespace off both ends of stone.
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Ugg! Magic rune engine (std::regex) is fat and slow, and cave man calls it every
// half heartbeat while waiting for phone. Plain stone-by-stone reading is faster.
// Grabs the word right after marker, up to the next whitespace.
std::string value_after(const std::string& text, const std::string& marker) {
    const auto pos = text.find(marker);
    if (pos == std::string::npos) return {};
    const auto start = pos + marker.size();
    const auto end = text.find_first_of(" \t\r\n", start);
    return end == std::string::npos ? text.substr(start) : text.substr(start, end - start);
}

// Cave man picks the first four-dot number stone after marker, e.g. "src 192.168.1.5".
std::string ipv4_after(const std::string& text, const std::string& marker) {
    size_t search = 0;
    while ((search = text.find(marker, search)) != std::string::npos) {
        size_t i = search + marker.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;

        const size_t start = i;
        int dots = 0;
        while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.')) {
            if (text[i] == '.') ++dots;
            ++i;
        }

        if (dots == 3 && i > start) {
            return text.substr(start, i - start);
        }
        search += marker.size();
    }
    return {};
}

} // namespace

std::string shell_quote(const std::string& text) {
    std::string quoted = "'";
    for (char c : text) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

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

#ifdef _WIN32
bool AdbClient::run_command_windows(const std::string& cmdline, const std::function<void(const char*, size_t)>& on_read, bool log_errors) {
    SECURITY_ATTRIBUTES saAttr;
    saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
    saAttr.bInheritHandle = TRUE;
    saAttr.lpSecurityDescriptor = NULL;

    HANDLE hChildStdoutRd, hChildStdoutWr;
    if (!CreatePipe(&hChildStdoutRd, &hChildStdoutWr, &saAttr, 0)) {
        if (log_errors) {
            std::cerr << "[ADB] CreatePipe failed" << std::endl;
        }
        return false;
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
        if (log_errors) {
            std::cerr << "[ADB] CreateProcess failed for: " << cmdline << std::endl;
        }
        return false;
    }

    CloseHandle(hChildStdoutWr);

    char buffer[4096];
    DWORD bytesRead;
    while (ReadFile(hChildStdoutRd, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
        if (on_read) {
            on_read(buffer, bytesRead);
        }
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hChildStdoutRd);

    return true;
}
#endif

ShellProcess::~ShellProcess() {
    stop();
}

bool ShellProcess::start(const std::string& device_id, const std::string& command,
                         std::function<void(const std::string&)> on_line) {
    stop();

    const std::string adb_path = get_adb_path();

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
        return false;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi = {};
    std::string cmdline = "\"" + adb_path + "\" -s " + device_id + " shell " + command;
    std::vector<char> cmd_buf(cmdline.begin(), cmdline.end());
    cmd_buf.push_back('\0');

    if (!CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        log_adb_event("[ADB] Could not spawn shell process: " + cmdline);
        return false;
    }

    CloseHandle(write_end);
    CloseHandle(pi.hThread);
    m_process_handle.store(pi.hProcess);
    m_running.store(true);

    m_reader = std::thread([this, read_end, callback = std::move(on_line)]() {
        std::string pending;
        char buffer[4096];
        DWORD read_bytes = 0;
        while (ReadFile(read_end, buffer, sizeof(buffer), &read_bytes, nullptr) && read_bytes > 0) {
            pending.append(buffer, read_bytes);
            size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos) {
                if (callback) callback(pending.substr(0, newline + 1));
                pending.erase(0, newline + 1);
            }
        }
        if (!pending.empty() && callback) callback(pending);
        CloseHandle(read_end);
        m_running.store(false);
    });
    return true;
#else
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        return false;
    }

    const pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl(adb_path.c_str(), adb_path.c_str(), "-s", device_id.c_str(), "shell",
              command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(pipefd[1]);
    m_pid.store(pid);
    m_running.store(true);

    m_reader = std::thread([this, fd = pipefd[0], pid, callback = std::move(on_line)]() {
        std::string pending;
        char buffer[4096];
        ssize_t read_bytes = 0;
        while ((read_bytes = read(fd, buffer, sizeof(buffer))) > 0) {
            pending.append(buffer, static_cast<size_t>(read_bytes));
            size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos) {
                if (callback) callback(pending.substr(0, newline + 1));
                pending.erase(0, newline + 1);
            }
        }
        if (!pending.empty() && callback) callback(pending);
        close(fd);
        waitpid(pid, nullptr, 0);
        m_running.store(false);
    });
    return true;
#endif
}

void ShellProcess::stop() {
#ifdef _WIN32
    // Ugg! Kill child first so its pipe end closes and reader thread stops waiting.
    HANDLE process = static_cast<HANDLE>(m_process_handle.exchange(nullptr));
    if (process) {
        TerminateProcess(process, 0);
        WaitForSingleObject(process, 2000);
        CloseHandle(process);
    }
#else
    const int pid = m_pid.exchange(-1);
    if (pid > 0) {
        kill(static_cast<pid_t>(pid), SIGKILL);
    }
#endif

    if (m_reader.joinable() && m_reader.get_id() != std::this_thread::get_id()) {
        m_reader.join();
    }
    m_running.store(false);
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
        if (arg.empty()) {
            cmdline += "\"\"";
        } else if (arg.find(' ') != std::string::npos || arg.find('"') != std::string::npos) {
            cmdline += "\"";
            for (char c : arg) {
                if (c == '"') {
                    cmdline += "\\\"";
                } else {
                    cmdline += c;
                }
            }
            cmdline += "\"";
        } else {
            cmdline += arg;
        }
    }

    run_command_windows(cmdline, [&result](const char* buffer, size_t bytesRead) {
        result.append(buffer, bytesRead);
    }, true);
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
            dev.model = value_after(line, "model:");

            devices.push_back(std::move(dev));
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
    m_last_install_error.clear();

    if (!std::filesystem::exists(apk_path)) {
        m_last_install_error = "APK nicht gefunden: " + apk_path;
        log_adb_event("[ADB] APK not found: " + apk_path);
        return false;
    }

    // Cave man ask phone who sit in front of it. Then install ONLY into that cave.
    // Without --user, phone would smear app over every user, also private space. NO!
    std::string user = get_current_user(device_id);

    std::vector<std::string> args = {"-s", device_id, "install"};
    if (!user.empty()) {
        args.push_back("--user");
        args.push_back(user);
    } else {
        log_adb_event("[ADB] Could not read current user, installing without --user");
    }
    for (const char* flag : {"-g", "-t", "-r", "-d"}) {
        args.push_back(flag);
    }
    args.push_back(apk_path);

    std::string output = run_adb_command(args);
    if (output.find("Success") != std::string::npos) {
        log_adb_event("[ADB] App installed for user " + (user.empty() ? std::string("<default>") : user));
        return true;
    }

    m_last_install_error = trim(output);
    log_adb_event("[ADB] App install failed (user " + (user.empty() ? std::string("<default>") : user) + "): " + output);
    return false;
}

bool AdbClient::install_pushed_app(const std::string& device_id, const std::string& remote_apk_path) {
    m_last_install_error.clear();

    // Same rule as install_app: ONLY the cave whose human sits at the fire right now.
    const std::string user = get_current_user(device_id);

    std::string command = "pm install";
    if (!user.empty()) {
        command += " --user " + user;
    } else {
        log_adb_event("[ADB] Could not read current user, installing pushed APK without --user");
    }
    // Ugg! NO -g here, unlike install_app. -g means "hand this app every key to the
    // cave at once" — camera, ear, where the human stands — without the phone ever
    // asking. For OUR OWN app the human already said yes by setting the thing up. A
    // rock somebody dragged into the window is a stranger: it gets installed, and then
    // it asks for its keys the normal way, like every app from the store does.
    // -t (test builds) and -d (older version) stay: those hand out nothing, they only
    // stop the phone from refusing an APK the human clearly meant to install.
    command += " -t -r -d " + shell_quote(remote_apk_path);

    const std::string output = execute_shell_command(device_id, command);
    if (output.find("Success") != std::string::npos) {
        log_adb_event("[ADB] Pushed app installed for user " + (user.empty() ? std::string("<default>") : user));
        return true;
    }

    m_last_install_error = trim(output);
    log_adb_event("[ADB] Pushed app install failed (user " +
        (user.empty() ? std::string("<default>") : user) + "): " + output);
    return false;
}

std::string AdbClient::get_current_user(const std::string& device_id) {
    std::string output = trim(execute_shell_command(device_id, "am get-current-user"));

    // Phone must answer with plain number. Anything else = cave man does not trust it.
    if (output.empty() || output.find_first_not_of("0123456789") != std::string::npos) {
        return {};
    }
    return output;
}

std::vector<std::string> AdbClient::users_with_app(const std::string& device_id, const std::string& package_name) {
    std::vector<std::string> users;

    // "pm list users" spits lines like: UserInfo{0:Friedrich:4c13} running
    const std::string user_list = execute_shell_command(device_id, "pm list users");
    const std::string marker = "UserInfo{";

    size_t pos = 0;
    while ((pos = user_list.find(marker, pos)) != std::string::npos) {
        const size_t id_start = pos + marker.size();
        const size_t first_colon = user_list.find(':', id_start);
        if (first_colon == std::string::npos) break;
        const size_t second_colon = user_list.find(':', first_colon + 1);
        if (second_colon == std::string::npos) break;
        pos = second_colon;

        const std::string id = user_list.substr(id_start, first_colon - id_start);
        if (id.empty() || id.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        const std::string name = trim(user_list.substr(first_colon + 1, second_colon - first_colon - 1));

        std::string packages = execute_shell_command(
            device_id, "pm list packages --user " + id + " " + package_name);
        if (packages.find("package:" + package_name) != std::string::npos) {
            users.push_back(name.empty() ? ("Profil " + id) : ("\"" + name + "\""));
        }
    }

    return users;
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

bool AdbClient::push_file(const std::string& device_id, const std::string& local_path, const std::string& remote_path) {
    std::string output = run_adb_command({"-s", device_id, "push", local_path, remote_path});
    if (output.find("error:") != std::string::npos || output.find("failed to copy") != std::string::npos) {
        std::cerr << "[ADB] Failed to push file: " << output << std::endl;
        return false;
    }
    return true;
}

bool AdbClient::push_file_paced(const std::string& device_id, const std::string& local_path,
                               const std::string& remote_path,
                               const std::function<void(uint64_t, uint64_t)>& on_progress,
                               const std::atomic<bool>* cancel) {
    // One piece per throw. Big enough that starting the adb helper does not eat the
    // whole trip, small enough that the stream only ever loses the air for a blink.
    constexpr uint64_t CHUNK_BYTES = 1024 * 1024;

    std::error_code ec;
    const uint64_t total = static_cast<uint64_t>(std::filesystem::file_size(local_path, ec));
    if (ec) {
        log_adb_event("[ADB] Cannot measure file for paced push: " + local_path);
        return false;
    }

    // Small rock fits in one hand. No point in slicing it.
    if (total <= CHUNK_BYTES) {
        const bool ok = push_file(device_id, local_path, remote_path);
        if (ok && on_progress) on_progress(total, total);
        return ok;
    }

    std::ifstream input(local_path, std::ios::binary);
    if (!input) {
        log_adb_event("[ADB] Cannot open file for paced push: " + local_path);
        return false;
    }

    const std::filesystem::path chunk_path = std::filesystem::temp_directory_path(ec) /
        ("pixelmirroring-push-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".part");
    // The rock grows under a side name and only takes the real one once it is whole.
    const std::string remote_part = remote_path + ".pmpart";
    const std::string remote_glue = remote_path + ".pmglue";

    std::vector<char> buffer(static_cast<size_t>(CHUNK_BYTES));
    uint64_t sent = 0;
    bool ok = true;

    while (sent < total) {
        if (cancel && cancel->load()) {
            ok = false;
            break;
        }

        input.read(buffer.data(), static_cast<std::streamsize>(CHUNK_BYTES));
        const uint64_t piece = static_cast<uint64_t>(input.gcount());
        if (piece == 0) {
            log_adb_event("[ADB] Paced push ran dry before the end: " + local_path);
            ok = false;
            break;
        }

        {
            std::ofstream chunk(chunk_path, std::ios::binary | std::ios::trunc);
            if (!chunk.write(buffer.data(), static_cast<std::streamsize>(piece))) {
                log_adb_event("[ADB] Cannot write push chunk: " + chunk_path.string());
                ok = false;
                break;
            }
        }

        const auto throw_start = std::chrono::steady_clock::now();

        // Every piece grows a side-rock, never the real name. An older rock of the same
        // name stays whole until the very last moment — a broken trip must not eat it.
        const bool first_piece = (sent == 0);
        if (!push_file(device_id, chunk_path.string(), first_piece ? remote_part : remote_glue)) {
            ok = false;
            break;
        }
        if (!first_piece) {
            // Ugg! Silence is a bad sign to trust — some phones mutter warnings even
            // when all went well. Cave man asks for a word he can actually look for.
            const std::string answer = execute_shell_command(device_id,
                "cat " + shell_quote(remote_glue) + " >> " + shell_quote(remote_part) +
                " && rm -f " + shell_quote(remote_glue) + " && echo PM_PIECE_OK");
            if (answer.find("PM_PIECE_OK") == std::string::npos) {
                log_adb_event("[ADB] Paced push could not glue piece on: " + answer);
                ok = false;
                break;
            }
        }

        sent += piece;
        if (on_progress) on_progress(sent, total);
        if (sent >= total) break;

        // Ugg! Picture-river first. Cave man rests as long as the throw took, so no
        // matter how fast or slow the air is today, at most half of it is ours.
        auto rest = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - throw_start);
        rest = std::clamp(rest, std::chrono::milliseconds(20), std::chrono::milliseconds(2000));
        std::this_thread::sleep_for(rest);
    }

    input.close();
    std::filesystem::remove(chunk_path, ec);

    // Whole at last — now it may take the real name. Only here does an older rock of
    // the same name give way.
    if (ok) {
        const std::string answer = execute_shell_command(device_id,
            "mv -f " + shell_quote(remote_part) + " " + shell_quote(remote_path) + " && echo PM_MOVE_OK");
        if (answer.find("PM_MOVE_OK") == std::string::npos) {
            log_adb_event("[ADB] Paced push could not give the rock its name: " + answer);
            ok = false;
        }
    }

    // Half a rock on the phone helps nobody — and would grow further on the next try.
    if (!ok) {
        execute_shell_command(device_id,
            "rm -f " + shell_quote(remote_part) + " " + shell_quote(remote_glue));
    }
    return ok;
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

bool AdbClient::is_app_installed(const std::string& device_id, const std::string& package_name) {
    // Cave man peek at app list of user who sit at phone NOW. Was hardcoded to user 0
    // before, so phone with second user or private space told lies.
    std::string user = get_current_user(device_id);
    if (user.empty()) user = "0";

    std::string output = execute_shell_command(device_id, "pm list packages --user " + user + " " + package_name);
    return output.find("package:" + package_name) != std::string::npos;
}

bool AdbClient::has_permission(const std::string& device_id, const std::string& package_name, const std::string& permission) {
    // Cave man dig through permission stones. Look for "granted=true".
    std::string output = execute_shell_command(device_id, "dumpsys package " + package_name);
    std::string search = permission + ": granted=true";
    return output.find(search) != std::string::npos;
}

std::string AdbClient::get_device_ip(const std::string& device_id) {
    std::string output = execute_shell_command(device_id, "ip route");
    std::string ip = ipv4_after(output, "src ");
    if (!ip.empty()) {
        return ip;
    }

    output = execute_shell_command(device_id, "ip -f inet addr show wlan0");
    return ipv4_after(output, "inet ");
}

} // namespace pm::adb
