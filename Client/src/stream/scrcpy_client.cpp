#include "scrcpy_client.h"
#include "../adb/adb_client.h"
#include <iostream>
#include <random>
#include <filesystem>
#include <future>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>

extern "C" {
#include <libavutil/frame.h>
}

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

namespace pm::stream {

namespace {
void log_stream_event(const std::string& message) {
    std::cout << message << std::endl;

    std::filesystem::path log_dir;
#ifndef PM_PORTABLE_BUILD
#ifdef _WIN32
    // CAVE MAN NO WRITE APPDATA FOR PORTABLE. ONLY EXE DIR.
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data && local_app_data[0] != '\0') {
        log_dir = std::filesystem::path(local_app_data) / "PixelMirroring";
    }
#endif
#endif
    if (log_dir.empty()) {
        log_dir = pm::adb::get_executable_dir();
    }

    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    std::ofstream file(log_dir / "stream.log", std::ios::app);
    if (file) {
        file << message << "\n";
    }
}
}

ScrcpyClient::ScrcpyClient() {
    video_socket_ = INVALID_SOCKET;
    control_socket_ = INVALID_SOCKET;
}

ScrcpyClient::~ScrcpyClient() {
    stop();
}

void ScrcpyClient::set_frame_callback(FrameCallback cb) {
    frame_cb_ = std::move(cb);
}

void ScrcpyClient::set_disconnect_callback(DisconnectCallback cb) {
    disconnect_cb_ = std::move(cb);
}

void ScrcpyClient::set_device_clipboard_callback(ClipboardCallback cb) {
    clipboard_cb_ = std::move(cb);
}

bool ScrcpyClient::start(const Config& config) {
    config_ = config;
    
    // Generate random SCID (31-bit)
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0x7FFFFFFF);
    char scid_hex[9];
    snprintf(scid_hex, sizeof(scid_hex), "%08x", dis(gen));
    scid_ = std::string(scid_hex);

    std::cout << "[Scrcpy] Starting session with SCID: " << scid_ << std::endl;

    if (!setup_tunnel()) return false;
    if (!start_server_process()) return false;
    if (!connect_sockets()) return false;
    if (!read_metadata()) return false;

    running_ = true;

    // Cave man set recv timeout so video thread wakes up if phone goes silent
    // Without this, recv() blocks forever on dead TCP connection (no FIN/RST)
#ifdef _WIN32
    DWORD recv_timeout = 10000; // 10 seconds
    setsockopt(video_socket_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_timeout, sizeof(recv_timeout));
    if (config_.control && control_socket_ != INVALID_SOCKET) {
        setsockopt(control_socket_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&recv_timeout, sizeof(recv_timeout));
    }
#else
    struct timeval recv_timeout;
    recv_timeout.tv_sec = 10;
    recv_timeout.tv_usec = 0;
    setsockopt(video_socket_, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
    if (config_.control && control_socket_ != INVALID_SOCKET) {
        setsockopt(control_socket_, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
    }
#endif
    
    video_thread_ = std::thread(&ScrcpyClient::video_thread_loop, this);
    if (config_.control) {
        control_thread_ = std::thread(&ScrcpyClient::control_thread_loop, this);
    }

    return true;
}

void ScrcpyClient::stop() {
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }
    
    if (video_socket_ != INVALID_SOCKET) {
        closesocket(video_socket_);
        video_socket_ = INVALID_SOCKET;
    }
    if (control_socket_ != INVALID_SOCKET) {
        closesocket(control_socket_);
        control_socket_ = INVALID_SOCKET;
    }
    
    if (video_thread_.get_id() != std::this_thread::get_id() && video_thread_.joinable()) {
        video_thread_.join();
    }
    if (control_thread_.get_id() != std::this_thread::get_id() && control_thread_.joinable()) {
        control_thread_.join();
    }
    
    // Cleanup ADB ports
    pm::adb::AdbClient adb;
    std::string remote = "localabstract:scrcpy_" + scid_;
    std::string local = "tcp:" + std::to_string(local_port_);
    if (config_.tunnel_forward) {
        adb.remove_forward(config_.device_id, local);
    } else {
        adb.remove_reverse(config_.device_id, remote);
    }
}

bool ScrcpyClient::is_running() const {
    return running_;
}

bool ScrcpyClient::setup_tunnel() {
    pm::adb::AdbClient adb;
    std::string remote = "localabstract:scrcpy_" + scid_;
    
    std::cout << "[Scrcpy] Setting up adb tunnel..." << std::endl;
    bool tunnel_success = false;
    
    for (int port = 27183; port <= 27200; ++port) {
        std::string local = "tcp:" + std::to_string(port);
        
        if (adb.reverse_port(config_.device_id, remote, local)) {
            local_port_ = port;
            config_.tunnel_forward = false;
            tunnel_success = true;
            std::cout << "[Scrcpy] Using reverse tunnel on port " << port << std::endl;
            break;
        } else if (adb.forward_port(config_.device_id, local, remote)) {
            local_port_ = port;
            config_.tunnel_forward = true;
            tunnel_success = true;
            std::cout << "[Scrcpy] Using forward tunnel on port " << port << std::endl;
            break;
        }
    }
    
    if (!tunnel_success) {
        std::cerr << "[Scrcpy] Both reverse and forward tunnels failed on all ports!" << std::endl;
        return false;
    }
    
    return true;
}

bool ScrcpyClient::start_server_process() {
    pm::adb::AdbClient adb;
    
    // 1. Push server (Cave man check file size on device first to skip slow push over Wi-Fi)
    std::string exe_dir = pm::adb::get_executable_dir();
    std::filesystem::path server_path = std::filesystem::path(exe_dir) / "scrcpy-server.jar";
    
    if (!std::filesystem::exists(server_path)) {
        server_path = std::filesystem::path(exe_dir) / ".." / "scrcpy_download" / "scrcpy-server.jar";
    }
    
    if (!std::filesystem::exists(server_path)) {
        std::filesystem::path current = std::filesystem::path(exe_dir);
        for (int i = 0; i < 3; ++i) {
            current = current.parent_path();
            std::filesystem::path check_server = current / "scrcpy_download" / "scrcpy-server.jar";
            if (std::filesystem::exists(check_server)) {
                server_path = check_server;
                break;
            }
        }
    }

    if (!std::filesystem::exists(server_path)) {
        std::cerr << "[Scrcpy] ERROR: scrcpy-server.jar not found! Make sure it is downloaded." << std::endl;
        return false;
    }

    std::error_code ec;
    uintmax_t local_size = std::filesystem::file_size(server_path, ec);
    bool skip_push = false;

    if (!ec && local_size > 0) {
        std::string check_cmd = "stat -c %s /data/local/tmp/scrcpy-server.jar 2>/dev/null || ls -l /data/local/tmp/scrcpy-server.jar 2>/dev/null";
        std::string remote_res = adb.execute_shell_command(config_.device_id, check_cmd);
        remote_res.erase(remote_res.find_last_not_of(" \n\r\t") + 1);
        if (!remote_res.empty() && remote_res.find(std::to_string(local_size)) != std::string::npos) {
            std::cout << "[Scrcpy] scrcpy-server.jar already present on device (" << local_size << " bytes), skipping push." << std::endl;
            skip_push = true;
        }
    }

    if (!skip_push) {
        std::cout << "[Scrcpy] Pushing server..." << std::endl;
        if (!adb.push_file(config_.device_id, server_path.string(), "/data/local/tmp/scrcpy-server.jar")) {
            std::cerr << "[Scrcpy] Could not push scrcpy-server.jar!" << std::endl;
            return false;
        }
    }
    
    // 2. Start server
    std::string cmd = "CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / com.genymobile.scrcpy.Server 2.7 ";
    cmd += "scid=" + scid_ + " ";
    cmd += "log_level=info ";
    cmd += "audio=false ";
    cmd += "max_size=" + std::to_string(config_.max_size) + " ";
    cmd += "video_codec=h264 ";
    cmd += "video_bit_rate=" + std::to_string(config_.video_bit_rate) + " ";
    cmd += "max_fps=" + std::to_string(config_.max_fps) + " ";
    cmd += "control=" + std::string(config_.control ? "true" : "false") + " ";
    cmd += "clipboard_autosync=true ";
    cmd += "send_dummy_byte=false ";
    cmd += "send_device_meta=false ";
    cmd += "send_codec_meta=true ";
    cmd += "send_frame_meta=true ";
    cmd += "tunnel_forward=" + std::string(config_.tunnel_forward ? "true" : "false") + " ";
    
    log_stream_event("[Scrcpy] Executing server: " + cmd);
    
    // Dies müsste eigentlich asynchron laufen, da app_process blockiert.
    auto ready_promise = std::make_shared<std::promise<bool>>();
    auto future = ready_promise->get_future();
    std::string device_id_copy = config_.device_id;
    std::thread([cmd, device_id_copy, ready_promise]() {
        pm::adb::AdbClient a;
        bool ready_sent = false;
        a.execute_shell_command_async(device_id_copy, cmd, [ready_promise, &ready_sent](const std::string& line) {
            if (!ready_sent && line.find("[server]") != std::string::npos) {
                ready_promise->set_value(true);
                ready_sent = true;
            }
        });
        if (!ready_sent) {
            try { ready_promise->set_value(false); } catch(...) {}
        }
    }).detach();

    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready || !future.get()) {
        std::cerr << "[Scrcpy] Server did not print ready message, proceeding anyway..." << std::endl;
    }

    return true;
}

bool ScrcpyClient::connect_sockets() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    std::cout << "[Scrcpy] Connecting to local tunnel..." << std::endl;
    
    auto connect_to_port = [](SOCKET& sock, int port) -> bool {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) return false;

        sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        // Retry connecting for up to 5 seconds
        for (int i = 0; i < 50; ++i) {
            if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != SOCKET_ERROR) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        closesocket(sock);
        sock = INVALID_SOCKET;
        return false;
    };

    auto accept_connection = [](SOCKET& sock, int port) -> bool {
        SOCKET server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_fd == INVALID_SOCKET) return false;

        sockaddr_in addr = {0};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = INADDR_ANY;

        if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(server_fd);
            return false;
        }

        if (listen(server_fd, 1) == SOCKET_ERROR) {
            closesocket(server_fd);
            return false;
        }

        // Wait up to 5 seconds for connection
        fd_set set;
        FD_ZERO(&set);
        FD_SET(server_fd, &set);
        timeval timeout = {5, 0};

#ifdef _WIN32
        int nfds = 0; // Windows ignores this parameter
#else
        int nfds = server_fd + 1; // POSIX requires highest fd + 1
#endif

        if (select(nfds, &set, nullptr, nullptr, &timeout) > 0) {
            sock = accept(server_fd, nullptr, nullptr);
            closesocket(server_fd);
            return sock != INVALID_SOCKET;
        }

        closesocket(server_fd);
        return false;
    };

    if (config_.tunnel_forward) {
        if (!connect_to_port(video_socket_, local_port_)) {
            std::cerr << "[Scrcpy] Failed to connect video socket" << std::endl;
            return false;
        }
        if (config_.control) {
            if (!connect_to_port(control_socket_, local_port_)) {
                std::cerr << "[Scrcpy] Failed to connect control socket" << std::endl;
                return false;
            }
        }
    } else {
        if (!accept_connection(video_socket_, local_port_)) {
            std::cerr << "[Scrcpy] Failed to accept video socket" << std::endl;
            return false;
        }
        if (config_.control) {
            if (!accept_connection(control_socket_, local_port_)) {
                std::cerr << "[Scrcpy] Failed to accept control socket" << std::endl;
                return false;
            }
        }
    }

    std::cout << "[Scrcpy] Sockets connected!" << std::endl;
    return true;
}

bool ScrcpyClient::read_metadata() {
    // Read codec info (12 bytes: id, width, height)
    uint8_t codec_meta[12];
    int total_bytes_read = 0;
    while (total_bytes_read < 12) {
        int bytes_read = recv(video_socket_, (char*)codec_meta + total_bytes_read, 12 - total_bytes_read, 0);
        if (bytes_read == 0) {
            std::cerr << "[Scrcpy] Connection closed while reading codec meta" << std::endl;
            return false;
        }
        if (bytes_read < 0) {
            std::cerr << "[Scrcpy] Failed to read codec meta" << std::endl;
            return false;
        }
        total_bytes_read += bytes_read;
    }
    
    video_codec_id_ = (codec_meta[0] << 24) | (codec_meta[1] << 16) | (codec_meta[2] << 8) | codec_meta[3];
    initial_width_ = (codec_meta[4] << 24) | (codec_meta[5] << 16) | (codec_meta[6] << 8) | codec_meta[7];
    initial_height_ = (codec_meta[8] << 24) | (codec_meta[9] << 16) | (codec_meta[10] << 8) | codec_meta[11];
    
    {
        std::ostringstream oss;
        oss << "[Scrcpy] Metadata codec=" << video_codec_id_
            << " size=" << initial_width_ << "x" << initial_height_
            << " max_size=" << config_.max_size
            << " bit_rate=" << config_.video_bit_rate
            << " max_fps=" << config_.max_fps;
        log_stream_event(oss.str());
    }

    if (initial_width_ == 0 || initial_height_ == 0) {
        std::cerr << "[Scrcpy] Invalid video size in metadata" << std::endl;
        return false;
    }

    return true;
}

void ScrcpyClient::video_thread_loop() {
    if (!decoder_.init(video_codec_id_)) {
        std::cerr << "[Scrcpy] Failed to initialize decoder" << std::endl;
        return;
    }

    auto recv_all = [this](char* buf, int len) -> bool {
        int total = 0;
        while (total < len && running_) {
            int r = recv(video_socket_, buf + total, len - total, 0);
            if (r > 0) {
                total += r;
            } else if (r == 0) {
                // Clean close — phone hung up
                return false;
            } else {
                // Cave man check if just timeout or real death
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT) {
#else
                int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK) {
#endif
                    // Timeout — phone just not sending right now, retry if still running
                    if (running_) continue;
                    return false;
                }
                // Real socket error — connection dead
                return false;
            }
        }
        return running_;
    };

    std::vector<uint8_t> packet_data;
    const uint32_t MAX_PACKET_SIZE = 10 * 1024 * 1024; // 10 MB max packet size
    bool logged_first_frame = false;

    while (running_) {
        uint8_t header[12];
        if (!recv_all((char*)header, 12)) break;

        uint64_t pts = 0;
        for (int i = 0; i < 8; ++i) pts = (pts << 8) | header[i];
        uint32_t size = (header[8] << 24) | (header[9] << 16) | (header[10] << 8) | header[11];

        // Validate packet size
        if (size == 0 || size > MAX_PACKET_SIZE) {
            std::cerr << "[Scrcpy] Invalid packet size: " << size << " bytes" << std::endl;
            break;
        }

        constexpr uint64_t SC_PACKET_FLAG_CONFIG = 1ULL << 63;
        bool is_config = (pts & SC_PACKET_FLAG_CONFIG) != 0;

        packet_data.resize(size);
        if (!recv_all((char*)packet_data.data(), size)) break;
        
        if (decoder_.decode(packet_data.data(), size, is_config)) {
            if (frame_cb_) {
                AVFrame* frame = (AVFrame*)decoder_.get_frame();
                if (!logged_first_frame && frame) {
                    std::ostringstream oss;
                    oss << "[Scrcpy] First decoded frame "
                        << frame->width << "x" << frame->height
                        << " format=" << frame->format;
                    log_stream_event(oss.str());
                    logged_first_frame = true;
                }
                frame_cb_(frame);
            }
        }
    }

    bool was_running = running_;
    running_ = false;
    if (was_running && disconnect_cb_) {
        disconnect_cb_();
    }
}

void ScrcpyClient::control_thread_loop() {
    auto recv_all = [this](char* buf, int len) -> bool {
        int total = 0;
        while (total < len && running_) {
            int r = recv(control_socket_, buf + total, len - total, 0);
            if (r > 0) {
                total += r;
            } else if (r == 0) {
                return false;
            } else {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT) {
#else
                int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK) {
#endif
                    if (running_) continue;
                    return false;
                }
                return false;
            }
        }
        return running_;
    };

    while (running_) {
        uint8_t type = 0;
        if (!recv_all((char*)&type, 1)) break;

        if (type == 0) { // SC_DEVICE_MSG_TYPE_CLIPBOARD
            uint8_t len_buf[4];
            if (!recv_all((char*)len_buf, 4)) break;
            uint32_t len = (len_buf[0] << 24) | (len_buf[1] << 16) | (len_buf[2] << 8) | len_buf[3];
            if (len > 0 && len < 10 * 1024 * 1024) {
                std::string text(len, '\0');
                if (!recv_all(&text[0], len)) break;
                if (clipboard_cb_) {
                    clipboard_cb_(text);
                }
            }
        } else if (type == 1) { // SC_DEVICE_MSG_TYPE_ACK_CLIPBOARD
            uint8_t seq_buf[8];
            if (!recv_all((char*)seq_buf, 8)) break;
        } else if (type == 2) { // SC_DEVICE_MSG_TYPE_UHID_OUTPUT
            uint8_t uhid_buf[2];
            if (!recv_all((char*)uhid_buf, 2)) break;
            uint8_t size_buf[2];
            if (!recv_all((char*)size_buf, 2)) break;
            uint16_t size = (size_buf[0] << 8) | size_buf[1];
            if (size > 0 && size < 65536) {
                std::vector<char> data(size);
                if (!recv_all(data.data(), size)) break;
            }
        } else {
            break;
        }
    }
}

void ScrcpyClient::inject_touch(int action, float x, float y, int w, int h) {
    if (!running_ || control_socket_ == INVALID_SOCKET) return;

    auto write16 = [](uint8_t* out, uint16_t value) {
        out[0] = static_cast<uint8_t>((value >> 8) & 0xff);
        out[1] = static_cast<uint8_t>(value & 0xff);
    };
    auto write32 = [](uint8_t* out, uint32_t value) {
        out[0] = static_cast<uint8_t>((value >> 24) & 0xff);
        out[1] = static_cast<uint8_t>((value >> 16) & 0xff);
        out[2] = static_cast<uint8_t>((value >> 8) & 0xff);
        out[3] = static_cast<uint8_t>(value & 0xff);
    };
    auto write64 = [](uint8_t* out, uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            out[i] = static_cast<uint8_t>((value >> (56 - i * 8)) & 0xff);
        }
    };

    constexpr uint32_t AMOTION_EVENT_BUTTON_PRIMARY = 1;
    constexpr uint64_t POINTER_ID_MOUSE = 0xffffffffffffffffULL;

    uint8_t buf[32] = {0};
    buf[0] = 2; // SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT
    buf[1] = action; // 0=DOWN, 1=UP, 2=MOVE

    write64(buf + 2, POINTER_ID_MOUSE);
    write32(buf + 10, static_cast<uint32_t>(x));
    write32(buf + 14, static_cast<uint32_t>(y));
    write16(buf + 18, static_cast<uint16_t>(w));
    write16(buf + 20, static_cast<uint16_t>(h));
    write16(buf + 22, action == 1 ? 0 : 0xffff);
    write32(buf + 24, action == 0 || action == 1 ? AMOTION_EVENT_BUTTON_PRIMARY : 0);
    write32(buf + 28, action == 1 ? 0 : AMOTION_EVENT_BUTTON_PRIMARY);

    send(control_socket_, (const char*)buf, sizeof(buf), 0);
}

void ScrcpyClient::inject_keycode(int action, int keycode) {
    if (!running_ || control_socket_ == INVALID_SOCKET) return;
    
    // keycode send. cave man type keys.
    uint8_t buf[14] = {0};
    buf[0] = 0; // SC_CONTROL_MSG_TYPE_INJECT_KEYCODE
    buf[1] = action; // 0=DOWN, 1=UP
    
    // keycode BE
    buf[2] = (keycode >> 24) & 0xff;
    buf[3] = (keycode >> 16) & 0xff;
    buf[4] = (keycode >> 8) & 0xff;
    buf[5] = keycode & 0xff;
    
    // repeat BE = 0 (buf[6..9])
    // metaState BE = 0 (buf[10..13])
    
    send(control_socket_, (const char*)buf, 14, 0);
}

void ScrcpyClient::inject_scroll(float x, float y, int w, int h, float hscroll, float vscroll) {
    if (!running_ || control_socket_ == INVALID_SOCKET) return;

    // scroll screen. cave man spin wheel.
    auto write16 = [](uint8_t* out, uint16_t value) {
        out[0] = static_cast<uint8_t>((value >> 8) & 0xff);
        out[1] = static_cast<uint8_t>(value & 0xff);
    };
    auto write32 = [](uint8_t* out, uint32_t value) {
        out[0] = static_cast<uint8_t>((value >> 24) & 0xff);
        out[1] = static_cast<uint8_t>((value >> 16) & 0xff);
        out[2] = static_cast<uint8_t>((value >> 8) & 0xff);
        out[3] = static_cast<uint8_t>(value & 0xff);
    };

    uint8_t buf[21] = {0};
    buf[0] = 3; // SC_CONTROL_MSG_TYPE_INJECT_SCROLL_EVENT

    write32(buf + 1, static_cast<uint32_t>(x));
    write32(buf + 5, static_cast<uint32_t>(y));
    write16(buf + 9, static_cast<uint16_t>(w));
    write16(buf + 11, static_cast<uint16_t>(h));

    // fixed-point: float * 8192
    float hs_scaled = hscroll * 8192.0f;
    float vs_scaled = vscroll * 8192.0f;

    // Clamp to int16 range
    int16_t hs_val = static_cast<int16_t>((std::max)(-32768.0f, (std::min)(hs_scaled, 32767.0f)));
    int16_t vs_val = static_cast<int16_t>((std::max)(-32768.0f, (std::min)(vs_scaled, 32767.0f)));

    write16(buf + 13, static_cast<uint16_t>(hs_val));
    write16(buf + 15, static_cast<uint16_t>(vs_val));

    // buttons = 0 (4 bytes)
    write32(buf + 17, 0);

    send(control_socket_, (const char*)buf, sizeof(buf), 0);
}

void ScrcpyClient::inject_text(const std::string& text) {
    if (!running_ || control_socket_ == INVALID_SOCKET || text.empty()) return;

    // text send. cave man write words.
    auto write32 = [](uint8_t* out, uint32_t value) {
        out[0] = static_cast<uint8_t>((value >> 24) & 0xff);
        out[1] = static_cast<uint8_t>((value >> 16) & 0xff);
        out[2] = static_cast<uint8_t>((value >> 8) & 0xff);
        out[3] = static_cast<uint8_t>(value & 0xff);
    };

    uint32_t len = static_cast<uint32_t>(text.size());
    std::vector<uint8_t> buf(5 + len);
    buf[0] = 1; // SC_CONTROL_MSG_TYPE_INJECT_TEXT
    write32(buf.data() + 1, len);
    std::memcpy(buf.data() + 5, text.data(), len);

    send(control_socket_, (const char*)buf.data(), static_cast<int>(buf.size()), 0);
}

void ScrcpyClient::inject_set_clipboard(const std::string& text) {
    if (!running_ || control_socket_ == INVALID_SOCKET || text.empty()) return;

    auto write32 = [](uint8_t* out, uint32_t value) {
        out[0] = static_cast<uint8_t>((value >> 24) & 0xff);
        out[1] = static_cast<uint8_t>((value >> 16) & 0xff);
        out[2] = static_cast<uint8_t>((value >> 8) & 0xff);
        out[3] = static_cast<uint8_t>(value & 0xff);
    };

    uint32_t len = static_cast<uint32_t>(text.size());
    std::vector<uint8_t> buf(14 + len);
    buf[0] = 9; // SC_CONTROL_MSG_TYPE_SET_CLIPBOARD
    std::memset(&buf[1], 0, 8); // 8 bytes sequence = 0
    buf[9] = 0; // paste = false
    write32(buf.data() + 10, len);
    std::memcpy(buf.data() + 14, text.data(), len);

    send(control_socket_, (const char*)buf.data(), static_cast<int>(buf.size()), 0);
}

void ScrcpyClient::inject_get_clipboard(uint8_t copy_key) {
    if (!running_ || control_socket_ == INVALID_SOCKET) return;

    // get device clip. cave man request clip text from phone.
    uint8_t buf[2];
    buf[0] = 8; // SC_CONTROL_MSG_TYPE_GET_CLIPBOARD
    buf[1] = copy_key; // 0 = NONE, 1 = COPY, 2 = CUT

    send(control_socket_, (const char*)buf, sizeof(buf), 0);
}

} // namespace pm::stream
