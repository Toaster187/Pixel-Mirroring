#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pm::adb {

// EVERY std::string in this module — paths going in, output coming out — is UTF-8.
// adb speaks UTF-8 and the child process is spawned with CreateProcessW, so the whole
// chain uses one encoding. NEVER pass a path::string() to anything here: on Windows
// that returns the ANSI code page, which renders Cyrillic, CJK and emoji as "?".
// Convert with pm::util::path_to_utf8() instead.
std::string get_executable_dir();

// Wraps a string in single quotes so the device shell does not split it on spaces,
// and escapes embedded quotes so nothing inside the name can break out and become a
// command of its own. Every path that came from user input — dropped files above all
// — goes through here.
std::string shell_quote(const std::string& text);

/**
 * A long-running "adb shell" child process that can be killed again.
 *
 * The scrcpy server used to run on a detached thread with no handle kept, so it was
 * never terminated and every session leaked a thread and an adb.exe. This owns the
 * child instead: stop() kills it and joins the reader thread.
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

    // All devices adb knows about, including offline and unauthorized ones.
    std::vector<Device> get_devices();

    // Forces offline/unauthorized devices to reconnect to trigger prompt
    void reconnect_offline();

    // Connects to a device via TCP/IP
    bool connect_device(const std::string& ip, int port = 5555);

    // Switches the device's adbd to TCP/IP mode on the given port.
    bool enable_tcpip(const std::string& device_id, int port = 5555);

    // Installs the companion APK. ONLY into the user profile that is currently in the
    // foreground — never other users, never the private space.
    bool install_app(const std::string& device_id, const std::string& apk_path);

    // Installs an APK that is already on the device, so a dropped APK is transferred
    // once (paced) instead of a second time at full speed by "adb install". Used only
    // for files the user dropped in, so it deliberately does NOT pre-grant permissions
    // the way install_app does — the phone asks for them like it does for any app.
    bool install_pushed_app(const std::string& device_id, const std::string& remote_apk_path);

    // Why the last install_app failed — raw adb output, meant to be shown to the user.
    const std::string& last_install_error() const { return m_last_install_error; }

    // The user profile currently in the foreground. Empty if the device did not answer.
    std::string get_current_user(const std::string& device_id);

    // Every user profile that has the package installed, not just the current one.
    std::vector<std::string> users_with_app(const std::string& device_id, const std::string& package_name);

    // Starts the companion app and its foreground service.
    bool start_app(const std::string& device_id, const std::string& package_name);
    bool start_service(const std::string& device_id, const std::string& service_name);

    // Executes a shell command on a specific device
    std::string execute_shell_command(const std::string& device_id, const std::string& command);

    // Pushes a file to the device
    bool push_file(const std::string& device_id, const std::string& local_path, const std::string& remote_path);

    // Same as push_file, but in small chunks. After each chunk the transfer sleeps for
    // as long as that chunk took, so the video stream keeps at least half the WLAN
    // airtime. on_progress(sent, total) is called from the calling thread after every
    // chunk. cancel may be null; when it turns true the transfer stops and the partial
    // file is removed from the device.
    bool push_file_paced(const std::string& device_id, const std::string& local_path,
                         const std::string& remote_path,
                         const std::function<void(uint64_t, uint64_t)>& on_progress = {},
                         const std::atomic<bool>* cancel = nullptr);

    // Setup port forwarding/reversing
    bool forward_port(const std::string& device_id, const std::string& local, const std::string& remote);
    bool reverse_port(const std::string& device_id, const std::string& remote, const std::string& local);
    bool remove_forward(const std::string& device_id, const std::string& local);
    bool remove_reverse(const std::string& device_id, const std::string& remote);

    // Automatically finds a USB device and grants WRITE_SECURE_SETTINGS
    bool auto_grant_secure_settings();
    bool grant_secure_settings(const std::string& device_id);

    // Is the package installed for the current user?
    bool is_app_installed(const std::string& device_id, const std::string& package_name);

    // Has the package already been granted the given permission?
    bool has_permission(const std::string& device_id, const std::string& package_name, const std::string& permission);

    // Reads the device's WLAN IP out of its routing table.
    std::string get_device_ip(const std::string& device_id);

private:
    std::string run_adb_command(const std::vector<std::string>& args);

    std::string m_last_install_error;

#ifdef _WIN32
    bool run_command_windows(const std::string& cmdline, const std::function<void(const char*, size_t)>& on_read, bool log_errors);
#endif
};

} // namespace pm::adb
