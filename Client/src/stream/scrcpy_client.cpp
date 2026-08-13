#include "scrcpy_client.h"
#include "control_messages.h"
#include "../adb/adb_client.h"
#include "../util/encoding.h"
#include <algorithm>
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
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#define INVALID_SOCKET -1
#define SOCKET_ERROR -1
#define closesocket close
#endif

namespace pm::stream {

// Message types, packet flags and the byte layouts that go with them live in the nested
// namespace pm::stream::wire (control_messages.h). They are pinned by
// pm_control_messages_test, which is the only thing standing between a wrong offset and
// a silently dead control thread.

namespace {
void log_stream_event(const std::string& message) {
    std::cout << message << std::endl;

    std::filesystem::path log_dir;
#ifndef PM_PORTABLE_BUILD
#ifdef _WIN32
    // A portable build keeps everything next to the EXE and never writes to
    // %LOCALAPPDATA%.
    const char* local_app_data = std::getenv("LOCALAPPDATA");
    if (local_app_data && local_app_data[0] != '\0') {
        log_dir = std::filesystem::path(local_app_data) / "PixelMirroring";
    }
#endif
#endif
    if (log_dir.empty()) {
        log_dir = pm::util::path_from_utf8(pm::adb::get_executable_dir());
    }

    std::error_code ec;
    std::filesystem::create_directories(log_dir, ec);
    std::ofstream file(log_dir / "stream.log", std::ios::app);
    if (file) {
        file << message << "\n";
    }
}

// scrcpy raw audio is always this shape (AudioCapture on the phone side).
constexpr int AUDIO_SAMPLE_RATE = 48000;
constexpr int AUDIO_CHANNELS = 2;

// Which server jar lies next to the exe. The server compares this word against its own
// and refuses to start on any mismatch, so it may only ever be changed together with
// Client/scrcpy-server.jar — and with every wire format that moved in between.
constexpr const char* SERVER_VERSION = "4.1";

// Probes whether the device will let anything open /dev/uhid. It opens the node
// EXACTLY the way the server does: read AND write in one descriptor (3<>). The server
// reads the phone's UHID output reports (caps-lock LED and the like) back out of the
// same descriptor, so a write-only probe would pass on a device that refuses reads and
// then take down the very thread it exists to protect.
//
// The exec runs inside a subshell (...) because a failing exec kills the shell it sits
// in — inside the parentheses only the subshell dies and the outer one can still print
// FAIL. Opening the node creates nothing; the keyboard only exists once UHID_CREATE is
// sent.
constexpr const char* UHID_PROBE_COMMAND =
    "(exec 3<>/dev/uhid) 2>/dev/null && echo PM_UHID_OK || echo PM_UHID_FAIL";

// Big-endian writers, in one place instead of open-coded at every call site.
void write16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xff);
    out[1] = static_cast<uint8_t>(value & 0xff);
}

void write32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xff);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xff);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xff);
    out[3] = static_cast<uint8_t>(value & 0xff);
}

void write64(uint8_t* out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> (56 - i * 8)) & 0xff);
    }
}

// How many bytes are currently sitting unread in the socket buffer. Windows and POSIX
// spell the same query differently; the branch lives HERE rather than inline in a
// condition, where editing one side quietly breaks the other's parentheses.
unsigned long socket_backlog(SOCKET socket_handle) {
#ifdef _WIN32
    unsigned long pending = 0;
    return ioctlsocket(socket_handle, FIONREAD, &pending) == 0 ? pending : 0;
#else
    int pending = 0;
    return ioctl(socket_handle, FIONREAD, &pending) == 0
        ? static_cast<unsigned long>(pending) : 0;
#endif
}

// Wakes up a thread that is blocked in recv() on this socket.
void wake_socket(SOCKET socket_handle) {
    if (socket_handle == INVALID_SOCKET) return;
#ifdef _WIN32
    shutdown(socket_handle, SD_BOTH);
#else
    shutdown(socket_handle, SHUT_RDWR);
#endif
}

// Gives the socket a receive timeout so a dead peer cannot block a thread forever.
void set_recv_timeout(SOCKET socket_handle, int seconds) {
    if (socket_handle == INVALID_SOCKET) return;
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(seconds) * 1000;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}
}

ScrcpyClient::ScrcpyClient() {
    video_socket_ = INVALID_SOCKET;
    audio_socket_ = INVALID_SOCKET;
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

void ScrcpyClient::set_control_lost_callback(ControlLostCallback cb) {
    control_lost_cb_ = std::move(cb);
}

void ScrcpyClient::set_resolution_callback(ResolutionCallback cb) {
    resolution_cb_ = std::move(cb);
}

bool ScrcpyClient::start(const Config& config) {
    config_ = config;
    
    // Random 31-bit SCID, the session id the server uses to name its sockets.
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis(0, 0x7FFFFFFF);
    char scid_hex[9];
    snprintf(scid_hex, sizeof(scid_hex), "%08x", dis(gen));
    scid_ = std::string(scid_hex);

    std::cout << "[Scrcpy] Starting session with SCID: " << scid_ << std::endl;

    if (!setup_tunnel()) return false;

    // If anything below fails, tear down whatever was already built. Returning early
    // without this left the server process running on the device plus an open adb
    // tunnel behind.
    if (!start_server_process() || !connect_sockets() || !read_metadata()) {
        abort_start();
        return false;
    }

    running_ = true;

    video_thread_ = std::thread(&ScrcpyClient::video_thread_loop, this);
    if (audio_available_) {
        audio_thread_ = std::thread(&ScrcpyClient::audio_thread_loop, this);
    }
    if (config_.control) {
        control_thread_ = std::thread(&ScrcpyClient::control_thread_loop, this);
    }

    return true;
}

void ScrcpyClient::stop() {
    // Turn the panel back on BEFORE the control socket is closed — afterwards there is
    // no way left to reach the phone and the user is holding a device with a dark
    // screen. Has to run while running_ is still true, or send_control() refuses.
    if (screen_forced_off_.load()) {
        inject_screen_power_mode(true);
    }

    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return;
    }

    // Order matters: shutdown() first so every blocked reader returns, then join() to
    // wait for those threads, and only then closesocket(). Closing first would leave a
    // reading thread sitting on a socket number the OS has already handed to somebody
    // else.
    wake_socket(video_socket_);
    wake_socket(audio_socket_);
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        wake_socket(control_socket_);
    }

    if (video_thread_.get_id() != std::this_thread::get_id() && video_thread_.joinable()) {
        video_thread_.join();
    }
    if (audio_thread_.get_id() != std::this_thread::get_id() && audio_thread_.joinable()) {
        audio_thread_.join();
    }
    if (control_thread_.get_id() != std::this_thread::get_id() && control_thread_.joinable()) {
        control_thread_.join();
    }
    audio_player_.stop();
    audio_available_ = false;

    // Kill the adb shell that hosts the server. Leaving it running meant every
    // reconnect left another server process alive on the device.
    server_process_.stop();

    if (video_socket_ != INVALID_SOCKET) {
        closesocket(video_socket_);
        video_socket_ = INVALID_SOCKET;
    }
    if (audio_socket_ != INVALID_SOCKET) {
        closesocket(audio_socket_);
        audio_socket_ = INVALID_SOCKET;
    }
    {
        std::lock_guard<std::mutex> lock(control_mutex_);
        if (control_socket_ != INVALID_SOCKET) {
            closesocket(control_socket_);
            control_socket_ = INVALID_SOCKET;
        }
    }

    remove_tunnel();
}

void ScrcpyClient::abort_start() {
    // No reader threads exist yet at this point, so closing directly is safe.
    server_process_.stop();
    audio_player_.stop();
    audio_available_ = false;

    if (video_socket_ != INVALID_SOCKET) {
        closesocket(video_socket_);
        video_socket_ = INVALID_SOCKET;
    }
    if (audio_socket_ != INVALID_SOCKET) {
        closesocket(audio_socket_);
        audio_socket_ = INVALID_SOCKET;
    }
    if (control_socket_ != INVALID_SOCKET) {
        closesocket(control_socket_);
        control_socket_ = INVALID_SOCKET;
    }

    remove_tunnel();
}

void ScrcpyClient::remove_tunnel() {
    pm::adb::AdbClient adb;
    const std::string remote = "localabstract:scrcpy_" + scid_;
    const std::string local = "tcp:" + std::to_string(local_port_);
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

    // Remove any reverse tunnel left over from a previous run.
    adb.remove_reverse(config_.device_id, remote);

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
        
        // Release the port again if setting up forward/reverse on it failed.
        adb.remove_forward(config_.device_id, local);
        adb.remove_reverse(config_.device_id, remote);
    }
    
    if (!tunnel_success) {
        std::cerr << "[Scrcpy] Both reverse and forward tunnels failed on all ports!" << std::endl;
        return false;
    }
    
    return true;
}

bool ScrcpyClient::start_server_process() {
    pm::adb::AdbClient adb;
    
    // 1. Push the server. The size on the device is checked first, so an unchanged jar
    // does not get pushed over Wi-Fi again.
    // get_executable_dir() returns UTF-8, so it has to go through path_from_utf8 —
    // path(exe_dir) would read it in the ANSI code page.
    const std::filesystem::path exe_dir =
        pm::util::path_from_utf8(pm::adb::get_executable_dir());
    std::filesystem::path server_path = exe_dir / "scrcpy-server.jar";

    if (!std::filesystem::exists(server_path)) {
        server_path = exe_dir / ".." / "scrcpy_download" / "scrcpy-server.jar";
    }

    if (!std::filesystem::exists(server_path)) {
        std::filesystem::path current = exe_dir;
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
        if (!adb.push_file(config_.device_id, pm::util::path_to_utf8(server_path),
                           "/data/local/tmp/scrcpy-server.jar")) {
            std::cerr << "[Scrcpy] Could not push scrcpy-server.jar!" << std::endl;
            return false;
        }
    }
    
    // 2. Start server
    std::string cmd = "CLASSPATH=/data/local/tmp/scrcpy-server.jar app_process / com.genymobile.scrcpy.Server ";
    cmd += std::string(SERVER_VERSION) + " ";
    cmd += "scid=" + scid_ + " ";
    cmd += "log_level=info ";
    cmd += "audio=" + std::string(config_.audio ? "true" : "false") + " ";
    if (config_.audio) {
        // Raw PCM: no decoder needed on this side and no codec config packet to
        // mishandle. See the audio notes in CLAUDE.md before changing this.
        cmd += "audio_codec=raw ";
        cmd += "audio_source=output ";
    }
    cmd += "max_size=" + std::to_string(config_.max_size) + " ";
    cmd += "video_codec=h264 ";
    cmd += "video_bit_rate=" + std::to_string(config_.video_bit_rate) + " ";
    cmd += "max_fps=" + std::to_string(config_.max_fps) + " ";
    cmd += "control=" + std::string(config_.control ? "true" : "false") + " ";
    cmd += "clipboard_autosync=true ";
    cmd += "send_dummy_byte=false ";
    cmd += "send_device_meta=false ";
    // Renamed in server 4.0. It now covers the codec id AND the session packets that
    // carry the picture size, which used to sit in a fixed header (see read_metadata).
    cmd += "send_stream_meta=true ";
    cmd += "send_frame_meta=true ";
    cmd += "tunnel_forward=" + std::string(config_.tunnel_forward ? "true" : "false") + " ";

    if (config_.screen_off_timeout_ms > 0) {
        cmd += "screen_off_timeout=" + std::to_string(config_.screen_off_timeout_ms) + " ";
    }

    if (config_.new_display) {
        // An empty value means "same size and same density as the phone's own display",
        // which is the only shape the rest of the client already knows how to handle.
        // Numbers are only appended when somebody really asked for them.
        std::string display_arg;
        if (config_.new_display_width > 0 && config_.new_display_height > 0) {
            display_arg = std::to_string(config_.new_display_width) + "x" +
                          std::to_string(config_.new_display_height);
        }
        if (config_.new_display_dpi > 0) {
            display_arg += "/" + std::to_string(config_.new_display_dpi);
        }
        cmd += "new_display=" + display_arg + " ";

        // Without this the on-screen keyboard appears on the PHONE while someone types
        // into the PC — on a phone meant to stay dark, nobody finds it again. "local"
        // pins the keyboard to OUR display.
        cmd += "display_ime_policy=local ";

        // A display of our own follows the PHONE into sleep. The moment the phone is
        // locked, WindowManager adds a "Display-off" sleep token to our display as well,
        // the launcher stops rendering and every input lands in the dark — the very
        // thing this mode exists to avoid. Verified on a Pixel 9 with Android 17:
        //   Add SleepToken: tag=Display-off, displayId=0   (phone locked)
        //   Add SleepToken: tag=Display-off, displayId=17  (ours, 200 ms later)
        // FLAG_ALWAYS_UNLOCKED only bypasses the lock screen, not sleep. That is what
        // keep_active is for: the server calls PowerManager.userActivity() on OUR display
        // alone. It exists only since server 4.x, which is why this client is on 4.1.
        cmd += "keep_active=true ";

        // A Pixel 9 on Android 17 DOES put its launcher for secondary displays on the
        // new display, but that launcher is useless: the home screen stays empty and
        // tapping an app in its drawer starts nothing, without even a refusal in the log.
        // Naming an app leaves the whole system furniture out and gives that one app the
        // display to itself. This is the route upstream documents for phones whose
        // secondary-display launcher is no good.
        if (!config_.new_display_app.empty()) {
            cmd += "vd_system_decorations=false ";
        }
    }

    log_stream_event("[Scrcpy] Executing server: " + cmd);

    // app_process blocks for as long as the server lives, so it runs in a child process
    // we keep a handle on (server_process_) instead of on a detached thread.
    auto ready_promise = std::make_shared<std::promise<bool>>();
    auto ready_flag = std::make_shared<std::atomic<bool>>(false);
    auto future = ready_promise->get_future();

    if (!server_process_.start(config_.device_id, cmd, [this, ready_promise, ready_flag](const std::string& line) {
            bool expected = false;
            if (line.find("[server]") != std::string::npos &&
                ready_flag->compare_exchange_strong(expected, true)) {
                ready_promise->set_value(true);
            }

            // When the server's control thread dies, video keeps flowing as if nothing
            // happened while every input event silently goes nowhere. Report it instead
            // of letting the user poke at an unresponsive phone.
            if (line.find("Controller error") != std::string::npos) {
                log_stream_event("[Scrcpy] Phone control thread died: " + line);
                if (control_lost_cb_) {
                    control_lost_cb_();
                }
            }
        })) {
        std::cerr << "[Scrcpy] Could not start server process!" << std::endl;
        return false;
    }

    if (future.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
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

        // Retry connecting for up to 5 seconds while the server comes up.
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

        // Wait up to 5 seconds for the server to connect back.
        fd_set set;
        FD_ZERO(&set);
        FD_SET(server_fd, &set);
        timeval timeout = {5, 0};

#ifdef _WIN32
        int nfds = 0; // ignored on Windows
#else
        int nfds = server_fd + 1; // POSIX wants highest fd + 1
#endif

        if (select(nfds, &set, nullptr, nullptr, &timeout) > 0) {
            sock = accept(server_fd, nullptr, nullptr);
            closesocket(server_fd);
            return sock != INVALID_SOCKET;
        }

        closesocket(server_fd);
        return false;
    };

    // The order is fixed by the server and must not be changed: video, then audio, then
    // control. Getting it wrong makes every stream read from the wrong socket.
    if (config_.tunnel_forward) {
        if (!connect_to_port(video_socket_, local_port_)) {
            std::cerr << "[Scrcpy] Failed to connect video socket" << std::endl;
            return false;
        }
        if (config_.audio) {
            if (!connect_to_port(audio_socket_, local_port_)) {
                std::cerr << "[Scrcpy] Failed to connect audio socket" << std::endl;
                return false;
            }
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
        if (config_.audio) {
            if (!accept_connection(audio_socket_, local_port_)) {
                std::cerr << "[Scrcpy] Failed to accept audio socket" << std::endl;
                return false;
            }
        }
        if (config_.control) {
            if (!accept_connection(control_socket_, local_port_)) {
                std::cerr << "[Scrcpy] Failed to accept control socket" << std::endl;
                return false;
            }
        }
    }

    // Set the receive timeout NOW, before reading metadata: without it a device that
    // dies mid-handshake leaves the app blocked in recv() forever.
    set_recv_timeout(video_socket_, 10);
    set_recv_timeout(audio_socket_, 10);
    set_recv_timeout(control_socket_, 10);

    std::cout << "[Scrcpy] Sockets connected!" << std::endl;
    return true;
}

bool ScrcpyClient::read_metadata() {
    // No threads are running yet, so running_ is still false and recv_all() would
    // refuse every byte. This one reads on its own until the buffer is full.
    auto read_blocking = [this](uint8_t* out, int length, const char* what) {
        int total = 0;
        while (total < length) {
            const int got = recv(video_socket_, (char*)out + total, length - total, 0);
            if (got <= 0) {
                std::cerr << "[Scrcpy] Failed to read " << what << std::endl;
                return false;
            }
            total += got;
        }
        return true;
    };

    // Since server 4.0 the video header is the codec id ALONE. The picture size used to
    // sit right behind it in the same header; now it rides in the stream as a session
    // packet — the first one arrives before any picture, and a new one after every
    // rotation or resize (see video_thread_loop).
    uint8_t codec_id_buf[4];
    if (!read_blocking(codec_id_buf, 4, "video codec id")) {
        return false;
    }
    video_codec_id_ = (codec_id_buf[0] << 24) | (codec_id_buf[1] << 16)
                    | (codec_id_buf[2] << 8) | codec_id_buf[3];

    uint8_t session_meta[12];
    if (!read_blocking(session_meta, 12, "session meta")) {
        return false;
    }
    // The top bit says "this is a session packet, not a picture". If it is missing we
    // are reading a picture header as if it were a size and everything after is rubbish
    // — that is exactly what a wrong server version looks like, so say so and stop.
    if ((session_meta[0] & 0x80) == 0) {
        std::cerr << "[Scrcpy] Expected a session packet, got something else — "
                     "server/client version mismatch?" << std::endl;
        return false;
    }
    initial_width_ = (session_meta[4] << 24) | (session_meta[5] << 16)
                   | (session_meta[6] << 8) | session_meta[7];
    initial_height_ = (session_meta[8] << 24) | (session_meta[9] << 16)
                    | (session_meta[10] << 8) | session_meta[11];

    {
        std::ostringstream oss;
        oss << "[Scrcpy] Metadata codec=" << video_codec_id_
            << " size=" << initial_width_.load() << "x" << initial_height_.load()
            << " max_size=" << config_.max_size
            << " bit_rate=" << config_.video_bit_rate
            << " max_fps=" << config_.max_fps;
        log_stream_event(oss.str());
    }

    if (initial_width_ == 0 || initial_height_ == 0) {
        std::cerr << "[Scrcpy] Invalid video size in metadata" << std::endl;
        return false;
    }

    // The audio socket answers with a 4-byte codec id. The device may also report that
    // it could not capture audio at all (0 = disabled, -1 = error). Either way video
    // keeps running — audio is never a reason to fail the whole stream.
    if (config_.audio && audio_socket_ != INVALID_SOCKET) {
        uint8_t audio_meta[4];
        int read_total = 0;
        while (read_total < 4) {
            const int bytes = recv(audio_socket_, (char*)audio_meta + read_total, 4 - read_total, 0);
            if (bytes <= 0) {
                log_stream_event("[Scrcpy] No audio metadata, continuing without sound");
                return true;
            }
            read_total += bytes;
        }

        const uint32_t audio_codec_id =
            (audio_meta[0] << 24) | (audio_meta[1] << 16) | (audio_meta[2] << 8) | audio_meta[3];

        constexpr uint32_t AUDIO_CODEC_RAW = 0x00726177;      // "\0raw"
        constexpr uint32_t AUDIO_CODEC_DISABLED = 0;
        constexpr uint32_t AUDIO_CODEC_ERROR = 0xFFFFFFFF;

        if (audio_codec_id == AUDIO_CODEC_DISABLED || audio_codec_id == AUDIO_CODEC_ERROR) {
            log_stream_event("[Scrcpy] Phone cannot capture audio, continuing without sound");
        } else if (audio_codec_id != AUDIO_CODEC_RAW) {
            std::ostringstream oss;
            oss << "[Scrcpy] Unexpected audio codec " << audio_codec_id << ", continuing without sound";
            log_stream_event(oss.str());
        } else if (!audio_player_.start(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS)) {
            log_stream_event("[Scrcpy] No PC sound device, continuing without sound");
        } else {
            audio_available_ = true;
            log_stream_event("[Scrcpy] Audio stream ready (raw PCM)");
        }
    }

    return true;
}

bool ScrcpyClient::recv_all(SOCKET socket_handle, char* buffer, int length) {
    int total = 0;
    while (total < length && running_) {
        const int received = recv(socket_handle, buffer + total, length - total, 0);
        if (received > 0) {
            total += received;
        } else if (received == 0) {
            // Clean close — the device hung up.
            return false;
        } else {
            // Distinguish a plain recv timeout from a real socket error.
#ifdef _WIN32
            const int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) {
#else
            const int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
#endif
                // Timeout — the device is simply not sending right now; retry.
                if (running_) continue;
                return false;
            }
            // Real socket error — the connection is gone.
            return false;
        }
    }
    return running_;
}

void ScrcpyClient::video_thread_loop() {
    if (!decoder_.init(video_codec_id_)) {
        std::cerr << "[Scrcpy] Failed to initialize decoder" << std::endl;
        return;
    }

    auto recv_all = [this](char* buf, int len) { return this->recv_all(video_socket_, buf, len); };

    std::vector<uint8_t> packet_data;
    const uint32_t MAX_PACKET_SIZE = 10 * 1024 * 1024; // 10 MB max packet size
    bool logged_first_frame = false;

    // Latency has to be measurable, not guessed: count decoded frames, how long
    // decoding took, and how many bytes are still queued unread in the socket. A queue
    // that keeps growing IS the latency.
    auto stats_since = std::chrono::steady_clock::now();
    int stats_packets = 0;
    long long stats_decode_us = 0;
    unsigned long long stats_bytes = 0;
    unsigned long stats_backlog_peak = 0;

    while (running_) {
        uint8_t header[12];
        if (!recv_all((char*)header, 12)) break;

        uint64_t pts = 0;
        for (int i = 0; i < 8; ++i) pts = (pts << 8) | header[i];

        if ((pts & wire::PACKET_FLAG_SESSION) != 0) {
            // A session packet IS the whole message — no picture follows it. Its twelve
            // bytes are flags, width, height. It arrives once before the first picture
            // and again whenever the display turns or is resized under us.
            const uint32_t new_width = (header[4] << 24) | (header[5] << 16)
                                     | (header[6] << 8) | header[7];
            const uint32_t new_height = (header[8] << 24) | (header[9] << 16)
                                      | (header[10] << 8) | header[11];
            if (new_width > 0 && new_height > 0 &&
                (new_width != initial_width_.load() || new_height != initial_height_.load())) {
                initial_width_.store(new_width);
                initial_height_.store(new_height);
                std::ostringstream oss;
                oss << "[Scrcpy] Display resized to " << new_width << "x" << new_height;
                log_stream_event(oss.str());
                // Whoever maps mouse pokes onto the phone is now working with the wrong
                // ruler until they hear this.
                if (resolution_cb_) {
                    resolution_cb_(static_cast<int>(new_width), static_cast<int>(new_height));
                }
            }
            continue;
        }

        uint32_t size = (header[8] << 24) | (header[9] << 16) | (header[10] << 8) | header[11];

        // Reject implausible packet sizes before allocating for them.
        if (size == 0 || size > MAX_PACKET_SIZE) {
            std::cerr << "[Scrcpy] Invalid packet size: " << size << " bytes" << std::endl;
            break;
        }

        bool is_config = (pts & wire::PACKET_FLAG_CONFIG) != 0;

        packet_data.resize(size);
        if (!recv_all((char*)packet_data.data(), size)) break;
        
        // How much has piled up in the socket while this packet was being decoded.
        stats_backlog_peak = (std::max)(stats_backlog_peak, socket_backlog(video_socket_));

        const auto decode_begin = std::chrono::steady_clock::now();
        const bool got_frame = decoder_.decode(packet_data.data(), size, is_config);
        stats_decode_us += std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - decode_begin).count();
        ++stats_packets;
        stats_bytes += size;

        if (got_frame) {
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

        const auto now = std::chrono::steady_clock::now();
        const auto window_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - stats_since).count();
        if (window_ms >= 5000 && stats_packets > 0) {
            std::ostringstream oss;
            oss << "[Scrcpy] " << (stats_packets * 1000 / window_ms) << " fps"
                << ", decode " << (stats_decode_us / stats_packets / 1000.0) << " ms/frame"
                << ", " << (stats_bytes * 8 / window_ms / 1000) << " Mbit/s"
                << ", socket backlog peak " << (stats_backlog_peak / 1024) << " KiB";
            log_stream_event(oss.str());
            stats_since = now;
            stats_packets = 0;
            stats_decode_us = 0;
            stats_bytes = 0;
            stats_backlog_peak = 0;
        }
    }

    bool was_running = running_;
    running_ = false;
    if (was_running && disconnect_cb_) {
        disconnect_cb_();
    }
}

void ScrcpyClient::audio_thread_loop() {
    // Same frame layout as video: 8 bytes pts, 4 bytes length, then the payload. For
    // raw PCM the payload goes straight to the audio device, with nothing to decode.
    std::vector<uint8_t> packet_data;
    constexpr uint32_t MAX_AUDIO_PACKET = 1 * 1024 * 1024;

    while (running_) {
        uint8_t header[12];
        if (!recv_all(audio_socket_, (char*)header, 12)) break;

        const uint32_t size = (header[8] << 24) | (header[9] << 16) | (header[10] << 8) | header[11];
        if (size == 0 || size > MAX_AUDIO_PACKET) {
            std::cerr << "[Scrcpy] Invalid audio packet size: " << size << std::endl;
            break;
        }

        packet_data.resize(size);
        if (!recv_all(audio_socket_, (char*)packet_data.data(), size)) break;

        audio_player_.feed(packet_data.data(), size);
    }

    // Audio failing must never take video down with it.
    audio_player_.stop();
}

void ScrcpyClient::control_thread_loop() {
    auto recv_all = [this](char* buf, int len) { return this->recv_all(control_socket_, buf, len); };

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

bool ScrcpyClient::send_control(const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (!running_ || control_socket_ == INVALID_SOCKET || !data || length == 0) {
        return false;
    }

    // send() may accept only part of the buffer. Keep writing the rest, or the device
    // reads a truncated message and the control protocol desynchronises for good.
    size_t sent = 0;
    while (sent < length) {
        const int written = send(control_socket_, reinterpret_cast<const char*>(data) + sent,
                                 static_cast<int>(length - sent), 0);
        if (written <= 0) {
            return false;
        }
        sent += static_cast<size_t>(written);
    }
    return true;
}

void ScrcpyClient::inject_touch(int action, float x, float y, int w, int h) {
    constexpr uint32_t AMOTION_EVENT_BUTTON_PRIMARY = 1;
    constexpr uint64_t POINTER_ID_MOUSE = 0xffffffffffffffffULL;

    // Clamp to the screen: a negative coordinate becomes a huge value once written
    // into the unsigned wire field, and the touch lands far off-screen.
    const uint32_t safe_x = static_cast<uint32_t>((std::max)(0.0f, x));
    const uint32_t safe_y = static_cast<uint32_t>((std::max)(0.0f, y));

    uint8_t buf[32] = {0};
    buf[0] = 2; // SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT
    buf[1] = static_cast<uint8_t>(action); // 0=DOWN, 1=UP, 2=MOVE

    write64(buf + 2, POINTER_ID_MOUSE);
    write32(buf + 10, safe_x);
    write32(buf + 14, safe_y);
    write16(buf + 18, static_cast<uint16_t>(w));
    write16(buf + 20, static_cast<uint16_t>(h));
    write16(buf + 22, action == 1 ? 0 : 0xffff);
    write32(buf + 24, action == 0 || action == 1 ? AMOTION_EVENT_BUTTON_PRIMARY : 0);
    write32(buf + 28, action == 1 ? 0 : AMOTION_EVENT_BUTTON_PRIMARY);

    send_control(buf, sizeof(buf));
}

void ScrcpyClient::inject_keycode(int action, int keycode) {
    uint8_t buf[14] = {0};
    buf[0] = 0; // SC_CONTROL_MSG_TYPE_INJECT_KEYCODE
    buf[1] = static_cast<uint8_t>(action); // 0=DOWN, 1=UP
    write32(buf + 2, static_cast<uint32_t>(keycode));
    // repeat BE = 0 (buf[6..9]), metaState BE = 0 (buf[10..13])

    send_control(buf, sizeof(buf));
}

void ScrcpyClient::inject_scroll(float x, float y, int w, int h, float hscroll, float vscroll) {
    const auto buf = wire::build_inject_scroll(
        x, y, static_cast<uint16_t>(w), static_cast<uint16_t>(h), hscroll, vscroll);

    send_control(buf.data(), buf.size());
}

void ScrcpyClient::inject_text(const std::string& text) {
    if (text.empty()) return;

    // Injects a UTF-8 string as text, used when the UHID keyboard is off.
    const uint32_t len = static_cast<uint32_t>(text.size());
    std::vector<uint8_t> buf(5 + len);
    buf[0] = 1; // SC_CONTROL_MSG_TYPE_INJECT_TEXT
    write32(buf.data() + 1, len);
    std::memcpy(buf.data() + 5, text.data(), len);

    send_control(buf.data(), buf.size());
}

void ScrcpyClient::inject_set_clipboard(const std::string& text) {
    if (text.empty()) return;

    const uint32_t len = static_cast<uint32_t>(text.size());
    std::vector<uint8_t> buf(14 + len, 0);
    buf[0] = 9; // SC_CONTROL_MSG_TYPE_SET_CLIPBOARD
    // buf[1..8] = sequence 0, buf[9] = paste false
    write32(buf.data() + 10, len);
    std::memcpy(buf.data() + 14, text.data(), len);

    send_control(buf.data(), buf.size());
}

void ScrcpyClient::inject_expand_notification_panel() {
    uint8_t buf[1] = {5}; // SC_CONTROL_MSG_TYPE_EXPAND_NOTIFICATION_PANEL
    send_control(buf, sizeof(buf));
}

void ScrcpyClient::inject_expand_settings_panel() {
    uint8_t buf[1] = {6}; // SC_CONTROL_MSG_TYPE_EXPAND_SETTINGS_PANEL
    send_control(buf, sizeof(buf));
}

void ScrcpyClient::inject_collapse_panels() {
    uint8_t buf[1] = {7}; // SC_CONTROL_MSG_TYPE_COLLAPSE_PANELS
    send_control(buf, sizeof(buf));
}

void ScrcpyClient::inject_screen_power_mode(bool on) {
    const std::vector<uint8_t> buf = wire::build_set_display_power(on);
    // Only record the panel as forced off once the message really went out — a message
    // that never arrived changed nothing on the device.
    if (send_control(buf.data(), buf.size())) {
        screen_forced_off_.store(!on);
        log_stream_event(on ? "[Scrcpy] Device screen power back to normal"
                            : "[Scrcpy] Device screen power turned off");
    } else if (!on) {
        log_stream_event("[Scrcpy] Could not turn device screen off — control socket silent");
    }
}

void ScrcpyClient::inject_get_clipboard(uint8_t copy_key) {
    // Asks the device to send its current clipboard contents back.
    uint8_t buf[2];
    buf[0] = 8; // SC_CONTROL_MSG_TYPE_GET_CLIPBOARD
    buf[1] = copy_key; // 0 = NONE, 1 = COPY, 2 = CUT

    send_control(buf, sizeof(buf));
}

bool ScrcpyClient::device_supports_uhid() {
    const std::string device_id = config_.device_id;
    if (device_id.empty()) {
        return false;
    }

    // The same device always gives the same answer, and asking costs a whole adb child
    // process — once per connect, and again after every quality restart. Cache the
    // result per device id instead.
    {
        std::lock_guard<std::mutex> guard(uhid_probe_mutex_);
        const auto known = uhid_probe_cache_.find(device_id);
        if (known != uhid_probe_cache_.end()) {
            return known->second;
        }
    }

    pm::adb::AdbClient adb;
    const std::string answer = adb.execute_shell_command(device_id, UHID_PROBE_COMMAND);
    const bool allowed = answer.find("PM_UHID_OK") != std::string::npos;

    {
        std::lock_guard<std::mutex> guard(uhid_probe_mutex_);
        uhid_probe_cache_[device_id] = allowed;
    }

    log_stream_event(allowed
        ? "[Scrcpy] Phone allows /dev/uhid, virtual USB keyboard is possible"
        : "[Scrcpy] Phone refuses /dev/uhid, staying on text injection");
    return allowed;
}

bool ScrcpyClient::uhid_create(uint16_t id, const std::string& name,
                               const uint8_t* report_desc, size_t desc_size) {
    const std::vector<uint8_t> buf = wire::build_uhid_create(id, name, report_desc, desc_size);
    if (buf.empty()) {
        return false;
    }

    if (!send_control(buf.data(), buf.size())) {
        log_stream_event("[Scrcpy] Could not send UHID_CREATE, keyboard stays virtual-less");
        return false;
    }

    std::ostringstream oss;
    oss << "[Scrcpy] Virtual USB keyboard created (id=" << id
        << ", report_desc=" << desc_size << " bytes)";
    log_stream_event(oss.str());
    return true;
}

void ScrcpyClient::uhid_input(uint16_t id, const uint8_t* data, size_t size) {
    // Every key press and release goes through here, so this path must not allocate.
    uint8_t buf[wire::UHID_INPUT_HEADER_SIZE + UHID_MAX_REPORT_SIZE];
    const size_t length = wire::build_uhid_input(buf, sizeof(buf), id, data, size);
    if (length == 0) {
        return;
    }

    send_control(buf, length);
}

void ScrcpyClient::uhid_destroy(uint16_t id) {
    const std::vector<uint8_t> buf = wire::build_uhid_destroy(id);

    send_control(buf.data(), buf.size());
}

void ScrcpyClient::open_hard_keyboard_settings() {
    const std::vector<uint8_t> buf = wire::build_open_hard_keyboard_settings();

    send_control(buf.data(), buf.size());
}

bool ScrcpyClient::start_app(const std::string& package_name) {
    const std::vector<uint8_t> buf = wire::build_start_app(package_name);
    if (buf.empty()) {
        return false;
    }

    if (!send_control(buf.data(), buf.size())) {
        log_stream_event("[Scrcpy] Could not ask phone to start app " + package_name);
        return false;
    }
    log_stream_event("[Scrcpy] Asked phone to start app " + package_name);
    return true;
}

} // namespace pm::stream
