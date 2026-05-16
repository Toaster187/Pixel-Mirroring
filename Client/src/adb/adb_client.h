#pragma once

#include <string>
#include <vector>
#include <functional>

namespace pm::adb {

std::string get_executable_dir();

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

    // Connects to a device via TCP/IP
    bool connect_device(const std::string& ip, int port = 5555);

    // Cave man tells ADB daemon to listen on air.
    bool enable_tcpip(const std::string& device_id, int port = 5555);

    // Cave man puts Android helper APK on phone.
    bool install_app(const std::string& device_id, const std::string& apk_path);

    // Cave man wakes Android helper app and service.
    bool start_app(const std::string& device_id, const std::string& package_name);
    bool start_service(const std::string& device_id, const std::string& service_name);

    // Executes a shell command on a specific device
    std::string execute_shell_command(const std::string& device_id, const std::string& command);

    // Executes a shell command asynchronously and returns the stdout lines via callback
    void execute_shell_command_async(const std::string& device_id, const std::string& command, std::function<void(const std::string&)> on_line);

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

    // Cave man reads phone IP from Android route stones.
    std::string get_device_ip(const std::string& device_id);

private:
    std::string run_adb_command(const std::vector<std::string>& args);
};

} // namespace pm::adb
