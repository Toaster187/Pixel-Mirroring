#include "network_scanner.h"
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
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

NetworkScanner::~NetworkScanner() {
#ifdef _WIN32
    WSACleanup();
#endif
}

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
                        // For large subnets, try to scan the /24 of the PC and some nearby /24s
                        size_t first_dot = ip.find('.');
                        size_t second_dot = ip.find('.', first_dot + 1);
                        size_t third_dot = ip.find('.', second_dot + 1);
                        if (first_dot != std::string::npos && second_dot != std::string::npos) {
                            std::string base16 = ip.substr(0, second_dot + 1);
                            int third_octet = std::stoi(ip.substr(second_dot + 1, third_dot - second_dot - 1));
                            
                            // Scan a slightly larger window around the PC's third octet
                            int start = std::max(0, third_octet - 10);
                            int end = std::min(255, third_octet + 10);
                            for (int i = start; i <= end; ++i) {
                                bases.push_back(base16 + std::to_string(i) + ".");
                            }
                            
                            // Also always include common third octets
                            std::vector<int> common_octets = {0, 1, 2, 10, 20, 50, 100, 150, 200};
                            for (int octet : common_octets) {
                                bases.push_back(base16 + std::to_string(octet) + ".");
                            }
                        }
                    } else {
                        // Standard /24
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
                    bases.push_back(ip.substr(0, last_dot + 1)); // e.g., "192.168.1."
                }
            }
            freeaddrinfo(res);
        }
    }
#endif
    
    // Add common fallback bases if empty
    if (bases.empty()) {
        bases.push_back("192.168.0.");
        bases.push_back("192.168.1.");
        bases.push_back("192.168.178."); // FritzBox default
    }
    
    // Remove duplicates
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
        // JSON parsing error or invalid response
        return std::nullopt;
    }
}

std::optional<DiscoveredDevice> NetworkScanner::request_connect(const std::string& ip, const std::string& client_id, const std::string& client_name) {
    json req_body = {
        {"clientId", client_id},
        {"clientName", client_name}
    };
    // Ugg! Phone side sleeps 500ms+500ms toggling adbd, plus up to 3s mDNS wait, before it answers.
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
        
        // Use a fixed pool of threads to prevent thread exhaustion
        // and eliminate the overhead of creating/destroying threads for every batch.
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