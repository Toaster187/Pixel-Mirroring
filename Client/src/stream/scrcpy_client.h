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

        // EXPERIMENT (needs server 3.x): the phone does not hand over ITS picture but
        // grows a SECOND display that exists only for this PC. The phone's own panel
        // stays dark and locked and a second human can keep using it meanwhile.
        bool new_display = false;
        int new_display_width = 0;   // 0 x 0 = same shape as the phone's own display
        int new_display_height = 0;
        int new_display_dpi = 0;     // 0 = scaled from the phone's own density
    };

    // Biggest HID report this client will carry to the phone. A keyboard report
    // is 8 bytes; the cap only exists so the hot send path can use a stone from
    // the stack instead of digging in the heap on every keystroke.
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

    // True while this session paints into a display of its own instead of into the
    // phone's. Everything that pokes the phone's OWN panel — brightness, unlocking,
    // the screen watchdog — has to keep its hands still while this is true.
    bool uses_new_display() const { return config_.new_display; }

    // Callbacks
    using FrameCallback = std::function<void(AVFrame* frame)>;
    using DisconnectCallback = std::function<void()>;
    using ClipboardCallback = std::function<void(const std::string& text)>;
    using ControlLostCallback = std::function<void()>;
    // Fires from the video thread when the phone changes the picture size mid-stream
    // (rotation, or a resize we asked for). Server 4.x announces this in the stream;
    // before that the size was fixed for the whole session.
    using ResolutionCallback = std::function<void(int width, int height)>;
    void set_frame_callback(FrameCallback cb);
    void set_disconnect_callback(DisconnectCallback cb);
    void set_device_clipboard_callback(ClipboardCallback cb);
    void set_resolution_callback(ResolutionCallback cb);

    // Fires when the phone's control thread falls over. The picture keeps running
    // but nothing listens any more — no keys, no mouse, no clipboard.
    void set_control_lost_callback(ControlLostCallback cb);

    // Input Injection
    void inject_touch(int action, float x, float y, int w, int h);
    void inject_keycode(int action, int keycode);
    void inject_scroll(float x, float y, int w, int h, float hscroll, float vscroll);
    void inject_text(const std::string& text);
    void inject_set_clipboard(const std::string& text);
    void inject_get_clipboard(uint8_t copy_key = 0);

    // Cave man pulls the phone's top curtains down or shoves them back up.
    // Message types 5, 6 and 7 — one bare byte each.
    void inject_expand_notification_panel();
    void inject_expand_settings_panel();
    void inject_collapse_panels();

    // Type 10 (SET_DISPLAY_POWER since server 3.0): the phone's light itself, not just
    // its brightness. false = panel dark, true = panel back to normal. The phone stays
    // awake and steerable either way. Always aims at the phone's OWN panel, also while
    // the picture comes from a display of our own.
    void inject_screen_power_mode(bool on);

    // Type 16, server 3.x only: puts an app onto the display. Cave man only needs this
    // when the phone hangs no launcher into a second display — then the new display
    // would stay black forever and nothing could ever be started on it.
    void start_app(const std::string& package_name);

    // True while WE hold the phone's light down. Stays true across stop() when the
    // "light back on" word never reached the phone — then somebody else must fix it.
    bool screen_forced_off() const { return screen_forced_off_.load(); }

    // Virtual USB devices (scrcpy UHID protocol).
    //
    // Ugg! ASK FIRST. If the phone refuses /dev/uhid and we tell the server to
    // build a keyboard anyway, the server's whole control thread dies with it —
    // keys, mouse, touch and clipboard all go quiet while the picture happily
    // keeps running. device_supports_uhid() is what stops that from happening.
    bool device_supports_uhid();
    bool uhid_create(uint16_t id, const std::string& name,
                     const uint8_t* report_desc, size_t desc_size);
    void uhid_input(uint16_t id, const uint8_t* data, size_t size);
    void uhid_destroy(uint16_t id);

    // Opens the phone's physical-keyboard settings, where the German layout is
    // assigned to the virtual keyboard.
    void open_hard_keyboard_settings();

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

    // Deliberately NOT reset in start(): if the phone's light is still down from a
    // session whose socket died, the next session has to know and put it back.
    std::atomic<bool> screen_forced_off_{false};

    // What each phone once answered about /dev/uhid. A phone does not change its
    // mind while the cave is open, so the adb child only has to run once per phone.
    // Written from the connection thread, read from the UI thread on a menu click.
    std::mutex uhid_probe_mutex_;
    std::unordered_map<std::string, bool> uhid_probe_cache_;

    // Turns false when the phone says it cannot capture sound. Video must not care.
    bool audio_available_{false};
    AudioPlayer audio_player_;

    FrameCallback frame_cb_;
    DisconnectCallback disconnect_cb_;
    ClipboardCallback clipboard_cb_;
    ControlLostCallback control_lost_cb_;
    ResolutionCallback resolution_cb_;

    // Device Info
    std::string device_name_;
    uint32_t video_codec_id_{0};
    uint32_t initial_width_{0};
    uint32_t initial_height_{0};
    
    int local_port_{27183};
    
    VideoDecoder decoder_;
};

} // namespace pm::stream
