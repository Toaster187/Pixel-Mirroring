#pragma once

#include <string>
#include <vector>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>

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

    // Largest HID report this client will send. A keyboard report is 8 bytes; the cap
    // only exists so the hot send path can use a stack buffer instead of allocating on
    // every keystroke.
    static constexpr size_t UHID_MAX_REPORT_SIZE = 64;

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
    using ControlLostCallback = std::function<void()>;
    void set_frame_callback(FrameCallback cb);
    void set_disconnect_callback(DisconnectCallback cb);
    void set_device_clipboard_callback(ClipboardCallback cb);

    // Fires when the server's control thread dies. Video keeps running but nothing
    // listens any more — no keys, no mouse, no clipboard.
    void set_control_lost_callback(ControlLostCallback cb);

    // Input Injection
    void inject_touch(int action, float x, float y, int w, int h);
    void inject_keycode(int action, int keycode);
    void inject_scroll(float x, float y, int w, int h, float hscroll, float vscroll);
    void inject_text(const std::string& text);
    void inject_set_clipboard(const std::string& text);
    void inject_get_clipboard(uint8_t copy_key = 0);

    // Opens and closes the phone's pull-down panels. Server 2.7 message types 5, 6
    // and 7 — a single byte each.
    void inject_expand_notification_panel();
    void inject_expand_settings_panel();
    void inject_collapse_panels();

    // Type 10: the panel power mode itself, not just brightness. false = panel off,
    // true = back to normal. The phone stays awake and controllable either way.
    void inject_screen_power_mode(bool on);

    // True while WE are holding the panel off. Stays true across stop() if the "panel
    // back on" message never reached the phone, so a later session can still fix it.
    bool screen_forced_off() const { return screen_forced_off_.load(); }

    // Virtual USB devices (scrcpy UHID protocol).
    //
    // PROBE FIRST. If the device refuses /dev/uhid and UHID_CREATE is sent anyway, the
    // exception escapes the server's handleEvent() and takes its entire control thread
    // down — keys, mouse, touch and clipboard all go silent while video keeps running.
    // device_supports_uhid() exists to prevent exactly that.
    bool device_supports_uhid();
    bool uhid_create(uint16_t id, const std::string& name,
                     const uint8_t* report_desc, size_t desc_size);
    void uhid_input(uint16_t id, const uint8_t* data, size_t size);
    void uhid_destroy(uint16_t id);

    // Opens the phone's physical-keyboard settings, where the German layout has to be
    // assigned to the virtual keyboard.
    void open_hard_keyboard_settings();

private:
    bool setup_tunnel();
    bool start_server_process();
    bool connect_sockets();
    bool read_metadata();

    // The UI thread and the screen-poll thread both send on the control socket, so
    // every message goes through here: serialised by a mutex, and looping until the
    // whole message has actually been written.
    bool send_control(const uint8_t* data, size_t length);

    // Reads exactly len bytes, tolerating recv timeouts, and gives up when the session
    // ends or the socket really dies. Shared by all three stream loops.
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

    // The "adb shell app_process ..." that hosts the server. Killed in stop().
    pm::adb::ShellProcess server_process_;

    std::thread video_thread_;
    std::thread audio_thread_;
    std::thread control_thread_;
    std::atomic<bool> running_{false};

    // Deliberately NOT reset in start(): if the panel is still off from a session
    // whose socket died, the next session has to know and turn it back on.
    std::atomic<bool> screen_forced_off_{false};

    // Cached /dev/uhid probe result per device. A device does not change its answer
    // while the app is running, so the adb child only has to run once per device.
    // Written from the connection thread, read from the UI thread on a menu click.
    std::mutex uhid_probe_mutex_;
    std::unordered_map<std::string, bool> uhid_probe_cache_;

    // Turns false when the device reports it cannot capture audio. Video must not care.
    bool audio_available_{false};
    AudioPlayer audio_player_;

    FrameCallback frame_cb_;
    DisconnectCallback disconnect_cb_;
    ClipboardCallback clipboard_cb_;
    ControlLostCallback control_lost_cb_;

    // Device Info
    std::string device_name_;
    uint32_t video_codec_id_{0};
    uint32_t initial_width_{0};
    uint32_t initial_height_{0};
    
    int local_port_{27183};
    
    VideoDecoder decoder_;
};

} // namespace pm::stream
