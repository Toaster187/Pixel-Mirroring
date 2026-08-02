#include "network_scanner.h"
#include <algorithm>
#include <charconv>
#include <iostream>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

#include <httplib.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#endif

using json = nlohmann::json;

namespace pm::network {

NetworkScanner::NetworkScanner() {
#ifdef _WIN32
    // WSAStartup/WSACleanup run exactly once for the whole process. Every scanner used
    // to do its own pair, so a short-lived scanner's cleanup could tear the Winsock
    // state out from under the live video sockets.
    static const bool winsock_ready = []() {
        WSADATA wsa_data;
        return WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0;
    }();
    (void)winsock_ready;
#endif
}

NetworkScanner::~NetworkScanner() = default;

std::vector<std::string> NetworkScanner::get_local_ipv4_bases() {
    std::vector<std::string> bases;
#ifdef _WIN32
    ULONG outBufLen = 0;
    if (GetAdaptersInfo(NULL, &outBufLen) == ERROR_BUFFER_OVERFLOW) {
        PIP_ADAPTER_INFO pAdapterInfo = (IP_ADAPTER_INFO*)malloc(outBufLen);
        if (pAdapterInfo == NULL) {
            return bases;
        }
        if (GetAdaptersInfo(pAdapterInfo, &outBufLen) == NO_ERROR) {
            PIP_ADAPTER_INFO pAdapter = pAdapterInfo;
            while (pAdapter) {
                std::string ip(pAdapter->IpAddressList.IpAddress.String);
                std::string mask(pAdapter->IpAddressList.IpMask.String);
                if (ip != "0.0.0.0" && ip != "127.0.0.1") {
                    if (mask == "255.255.0.0" || mask == "255.240.0.0" || mask == "255.0.0.0") {
                        // For subnets larger than /24, scan the PC's own /24 plus a few
                        // neighbouring ones instead of the entire range.
                        size_t first_dot = ip.find('.');
                        size_t second_dot = ip.find('.', first_dot + 1);
                        size_t third_dot = ip.find('.', second_dot + 1);
                        if (first_dot != std::string::npos && second_dot != std::string::npos) {
                            std::string base16 = ip.substr(0, second_dot + 1);
                            const std::string octet_text =
                                ip.substr(second_dot + 1, third_dot - second_dot - 1);

                            // std::stoi throws on non-numeric input and nothing above
                            // catches it, which used to take the whole process down.
                            int third_octet = 0;
                            const auto parsed = std::from_chars(
                                octet_text.data(), octet_text.data() + octet_text.size(), third_octet);
                            if (parsed.ec == std::errc()) {
                                // Scan a small window around the PC's own third octet.
                                int start = std::max(0, third_octet - 10);
                                int end = std::min(255, third_octet + 10);
                                for (int i = start; i <= end; ++i) {
                                    bases.push_back(base16 + std::to_string(i) + ".");
                                }

                                // Always include the third octets home routers use most.
                                for (int octet : {0, 1, 2, 10, 20, 50, 100, 150, 200}) {
                                    bases.push_back(base16 + std::to_string(octet) + ".");
                                }
                            }
                        }
                    } else {
                        // Plain /24: one base is enough.
                        size_t last_dot = ip.find_last_of('.');
                        if (last_dot != std::string::npos) {
                            bases.push_back(ip.substr(0, last_dot + 1));
                        }
                    }
                }
                pAdapter = pAdapter->Next;
            }
        }
        free(pAdapterInfo);
    }
#else
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        struct addrinfo hints = {0}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(hostname, nullptr, &hints, &res) == 0) {
            for (struct addrinfo* ptr = res; ptr != nullptr; ptr = ptr->ai_next) {
                struct sockaddr_in* addr = (struct sockaddr_in*)ptr->ai_addr;
                char ip_str[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &(addr->sin_addr), ip_str, INET_ADDRSTRLEN);
                std::string ip(ip_str);
                
                size_t last_dot = ip.find_last_of('.');
                if (last_dot != std::string::npos) {
                    bases.push_back(ip.substr(0, last_dot + 1)); // e.g. "192.168.1."
                }
            }
            freeaddrinfo(res);
        }
    }
#endif
    
    // No usable interface found — fall back to the common home-router ranges.
    if (bases.empty()) {
        bases.push_back("192.168.0.");
        bases.push_back("192.168.1.");
        bases.push_back("192.168.178."); // FRITZ!Box default
    }
    
    // Drop duplicate bases so no range is scanned twice.
    std::sort(bases.begin(), bases.end());
    bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
    
    return bases;
}

std::optional<DiscoveredDevice> NetworkScanner::post_connect(const std::string& target_ip, const std::string& req_str, int conn_timeout_ms, int read_timeout_ms) {
    httplib::Client cli(target_ip, 18294);
    cli.set_connection_timeout(conn_timeout_ms / 1000, (conn_timeout_ms % 1000) * 1000);
    cli.set_read_timeout(read_timeout_ms / 1000, (read_timeout_ms % 1000) * 1000);

    auto res = cli.Post("/connect", req_str, "application/json");
    if (!res) {
        return std::nullopt;
    }

    if (res->status == 403) {
        std::cout << "\n[NetworkScanner] Access denied on " << target_ip << ". Device is paired with another client." << std::endl;
        return std::nullopt;
    }

    if (res->status != 200) {
        return std::nullopt;
    }

    try {
        auto resp_json = json::parse(res->body);
        if (!resp_json["success"].get<bool>()) {
            return std::nullopt;
        }
        DiscoveredDevice device;
        device.ip = target_ip;
        device.adb_port = resp_json["adbPort"].get<int>();
        device.device_name = resp_json["deviceName"].get<std::string>();
        for (const auto& ip : resp_json["ips"]) {
            device.all_ips.push_back(ip.get<std::string>());
        }
        return device;
    } catch (...) {
        // Malformed JSON or a response that is not from our companion app.
        return std::nullopt;
    }
}

std::optional<DiscoveredDevice> NetworkScanner::request_connect(const std::string& ip, const std::string& client_id, const std::string& client_name) {
    json req_body = {
        {"clientId", client_id},
        {"clientName", client_name}
    };
    // Generous timeout: the phone toggles adbd off and on (500ms + 500ms) and then
    // waits up to 3s for the mDNS port before it answers.
    auto discovered = post_connect(ip, req_body.dump(), 1500, 8000);
    if (discovered) {
        std::cout << "[NetworkScanner] Woke ADB on known device " << ip << std::endl;
    }
    return discovered;
}

bool NetworkScanner::send_heartbeat(const std::string& ip, const std::string& client_id, const std::string& client_name) {
    json req_body = {
        {"clientId", client_id},
        {"clientName", client_name}
    };
    std::string req_str = req_body.dump();

    try {
        httplib::Client cli(ip, 18294);
        cli.set_connection_timeout(1, 0);  // 1s
        cli.set_read_timeout(2, 0);        // 2s
        auto res = cli.Post("/heartbeat", req_str, "application/json");
        return res && res->status == 200;
    } catch (...) {
        return false;
    }
}

std::optional<DiscoveredDevice> NetworkScanner::discover_and_connect(const std::string& client_id, const std::string& client_name) {
    auto bases = get_local_ipv4_bases();
    
    std::mutex result_mutex;
    std::optional<DiscoveredDevice> discovered;
    std::atomic<bool> found{false};
    
    json req_body = {
        {"clientId", client_id},
        {"clientName", client_name}
    };
    std::string req_str = req_body.dump();

    std::cout << "[NetworkScanner] Scanning local network for Android app..." << std::endl;

    for (const auto& base : bases) {
        if (found) break;
        std::cout << "[NetworkScanner] Scanning subnet: " << base << "x" << std::endl;
        
        // A fixed thread pool: scanning a /24 one thread per host would exhaust the
        // system, and creating/destroying threads per batch is pure overhead.
        const int num_workers = 50;
        std::atomic<int> current_ip_ending{1};
        std::vector<std::thread> threads;

        for (int w = 0; w < num_workers; ++w) {
            threads.emplace_back([&, base]() {
                while (!found) {
                    int ip_ending = current_ip_ending.fetch_add(1);
                    if (ip_ending > 254) break;

                    std::string target_ip = base + std::to_string(ip_ending);
                    auto device = post_connect(target_ip, req_str, 800, 3500);
                    if (device) {
                        std::lock_guard<std::mutex> lock(result_mutex);
                        if (!found) {
                            discovered = device;
                            found = true;
                            std::cout << "[NetworkScanner] Found app on " << target_ip << std::endl;
                        }
                    }
                }
            });
        }

        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }
    
    return discovered;
}

} // namespace pm::network