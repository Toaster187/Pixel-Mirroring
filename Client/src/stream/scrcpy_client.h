#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#else
#define SOCKET int
#endif

#include "../adb/adb_client.h"
#include "audio_player.h"
#include "video_decoder.h"

// Forward declarations for FFmpeg/SDL (they will be included in the .cpp files)
struct AVFrame;

namespace pm::stream {

class ScrcpyClient {
public:
    struct Config {
        std::string device_id;
        int max_size = 0;
        int video_bit_rate = 20000000;
        int max_fps = 60;
        bool audio = false; 
        bool control = true;
        bool tunnel_forward = false;
        bool lowest_brightness = true;
    };

    ScrcpyClient();
    ~ScrcpyClient();

    // Startet den ADB-Tunnel, pusht den Server und verbindet die Sockets
    bool start(const Config& config);
    void stop();
    bool is_running() const;
    int video_width() const { return static_cast<int>(initial_width_); }
    int video_height() const { return static_cast<int>(initial_height_); }
    const std::string& get_device_id() const { return config_.device_id; }

    // Callbacks
    using FrameCallback = std::function<void(AVFrame* frame)>;
    using DisconnectCallback = std::function<void()>;
    using ClipboardCallback = std::function<void(const std::string& text)>;
    void set_frame_callback(FrameCallback cb);
    void set_disconnect_callback(DisconnectCallback cb);
    void set_device_clipboard_callback(ClipboardCallback cb);

    // Input Injection
    void inject_touch(int action, float x, float y, int w, int h);
    void inject_keycode(int action, int keycode);
    void inject_scroll(float x, float y, int w, int h, float hscroll, float vscroll);
    void inject_text(const std::string& text);
    void inject_set_clipboard(const std::string& text);
    void inject_get_clipboard(uint8_t copy_key = 0);

private:
    bool setup_tunnel();
    bool start_server_process();
    bool connect_sockets();
    bool read_metadata();

    // Ugg! Two hunters used to shout into the same control hole at once and the
    // words got mixed. Now every message goes through here, one at a time, and
    // keeps pushing until the whole message is out.
    bool send_control(const uint8_t* data, size_t length);

    // Reads exactly len bytes, riding out the recv timeouts, and gives up when the
    // session ends or the socket really dies. Shared by all three stream loops.
    bool recv_all(SOCKET socket_handle, char* buffer, int length);

    // Tears down whatever start() managed to build before it hit a wall.
    void abort_start();
    void remove_tunnel();

    void video_thread_loop();
    void audio_thread_loop();
    void control_thread_loop();

    Config config_;
    std::string scid_;

    SOCKET video_socket_;
    SOCKET audio_socket_;
    SOCKET control_socket_;
    std::mutex control_mutex_;

    // The "adb shell app_process ..." that carries the server. Killed in stop().
    pm::adb::ShellProcess server_process_;

    std::thread video_thread_;
    std::thread audio_thread_;
    std::thread control_thread_;
    std::atomic<bool> running_{false};

    // Turns false when the phone says it cannot capture sound. Video must not care.
    bool audio_available_{false};
    AudioPlayer audio_player_;

    FrameCallback frame_cb_;
    DisconnectCallback disconnect_cb_;
    ClipboardCallback clipboard_cb_;

    // Device Info
    std::string device_name_;
    uint32_t video_codec_id_{0};
    uint32_t initial_width_{0};
    uint32_t initial_height_{0};
    
    int local_port_{27183};
    
    VideoDecoder decoder_;
};

} // namespace pm::stream
