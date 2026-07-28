#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pm::adb {

std::string get_executable_dir();

/**
 * A long-running "adb shell" child process that can be killed again.
 *
 * Ugg! Old cave man threw spear at process and forgot it. Spear never came back,
 * process never died. Now cave man keeps rope on spear: stop() pulls it back in.
 */
class ShellProcess {
public:
    ShellProcess() = default;
    ~ShellProcess();

    ShellProcess(const ShellProcess&) = delete;
    ShellProcess& operator=(const ShellProcess&) = delete;

    // Starts "adb -s <device> shell <command>" and feeds every stdout line to on_line
    // on a background thread. Returns false if the process could not be spawned.
    bool start(const std::string& device_id, const std::string& command,
               std::function<void(const std::string&)> on_line);

    // Kills the child process (if any) and joins the reader thread. Safe to call twice.
    void stop();

    bool is_running() const { return m_running.load(); }

private:
    std::thread m_reader;
    std::atomic<bool> m_running{false};

    // Native process handle, hidden so the header stays free of <windows.h>.
    std::atomic<void*> m_process_handle{nullptr};
    std::atomic<int> m_pid{-1};
};

/**
 * Basic representation of an ADB Device.
 */
struct Device {
    std::string id;
    std::string state;
    std::string model;
    
    bool is_usb() const;
    bool is_tcp() const;
};

/**
 * A simple ADB client that wraps around the adb command line tool or connects directly to the ADB server (port 5037).
 * For simplicity in this first step, we use subprocess execution.
 */
class AdbClient {
public:
    AdbClient();
    ~AdbClient();

    // Initializes the ADB connection/daemon
    bool init();

    // Returns a list of currently connected devices
    std::vector<Device> get_connected_devices();

    // Cave man sees all phones, even sleepy or not trusted.
    std::vector<Device> get_devices();

    // Forces offline/unauthorized devices to reconnect to trigger prompt
    void reconnect_offline();

    // Connects to a device via TCP/IP
    bool connect_device(const std::string& ip, int port = 5555);

    // Cave man tells ADB daemon to listen on air.
    bool enable_tcpip(const std::string& device_id, int port = 5555);

    // Cave man puts Android helper APK on phone. ONLY for user who sit at phone now,
    // never other users, never private space. Ugg! Other caves are not our cave.
    bool install_app(const std::string& device_id, const std::string& apk_path);

    // What went wrong last time install_app said no? Raw adb words for human eyes.
    const std::string& last_install_error() const { return m_last_install_error; }

    // Cave man ask phone which user sit in front of fire right now. Empty = no answer.
    std::string get_current_user(const std::string& device_id);

    // Cave man look in EVERY cave (user) for app, not only own one.
    std::vector<std::string> users_with_app(const std::string& device_id, const std::string& package_name);

    // Cave man wakes Android helper app and service.
    bool start_app(const std::string& device_id, const std::string& package_name);
    bool start_service(const std::string& device_id, const std::string& service_name);

    // Executes a shell command on a specific device
    std::string execute_shell_command(const std::string& device_id, const std::string& command);

    // Pushes a file to the device
    bool push_file(const std::string& device_id, const std::string& local_path, const std::string& remote_path);

    // Setup port forwarding/reversing
    bool forward_port(const std::string& device_id, const std::string& local, const std::string& remote);
    bool reverse_port(const std::string& device_id, const std::string& remote, const std::string& local);
    bool remove_forward(const std::string& device_id, const std::string& local);
    bool remove_reverse(const std::string& device_id, const std::string& remote);

    // Automatically finds a USB device and grants WRITE_SECURE_SETTINGS
    bool auto_grant_secure_settings();
    bool grant_secure_settings(const std::string& device_id);

    // Cave man peek if app already live on phone.
    bool is_app_installed(const std::string& device_id, const std::string& package_name);

    // Cave man check if phone already trust cave app.
    bool has_permission(const std::string& device_id, const std::string& package_name, const std::string& permission);

    // Cave man reads phone IP from Android route stones.
    std::string get_device_ip(const std::string& device_id);

private:
    std::string run_adb_command(const std::vector<std::string>& args);

    std::string m_last_install_error;

#ifdef _WIN32
    bool run_command_windows(const std::string& cmdline, const std::function<void(const char*, size_t)>& on_read, bool log_errors);
#endif
};

} // namespace pm::adb
