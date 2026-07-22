#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>

#ifdef _WIN32
#include <winsock2.h>
#else
#define SOCKET int
#endif

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
    bool push_server();
    bool launch_server_process();
    bool connect_sockets();
    bool read_metadata();

    void video_thread_loop();
    void control_thread_loop();

    Config config_;
    std::string scid_;
    
    SOCKET video_socket_;
    SOCKET control_socket_;
    
    std::thread video_thread_;
    std::thread control_thread_;
    std::atomic<bool> running_{false};

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
