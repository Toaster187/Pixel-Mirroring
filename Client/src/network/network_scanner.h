#pragma once

#include <string>
#include <vector>
#include <optional>

namespace pm::network {

struct DiscoveredDevice {
    std::string ip;
    int adb_port;
    std::string device_name;
    std::vector<std::string> all_ips;
};

class NetworkScanner {
public:
    NetworkScanner();
    ~NetworkScanner();

    std::optional<DiscoveredDevice> discover_and_connect(const std::string& client_id, const std::string& client_name);

    // Ugg! Poke a known IP directly instead of sweeping the whole subnet.
    std::optional<DiscoveredDevice> request_connect(const std::string& ip, const std::string& client_id, const std::string& client_name);

    // Ugg! Tell the phone "PC still alive" so it doesn't close the ADB door.
    bool send_heartbeat(const std::string& ip, const std::string& client_id, const std::string& client_name);

private:
    std::vector<std::string> get_local_ipv4_bases();
    std::optional<DiscoveredDevice> post_connect(const std::string& target_ip, const std::string& req_str, int conn_timeout_ms, int read_timeout_ms);
};

} // namespace pm::network
