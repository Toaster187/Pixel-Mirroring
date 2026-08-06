#pragma once
#include <cstdint>
#include <mutex>
#include <vector>
#include <memory>
#include <atomic>
#include "capture_controller.h"

struct SDL_Texture;
struct SDL_Renderer;
struct AVFrame;

namespace pm::stream {

// Renders decoded frames on the GPU — no CPU scaling, no extra colour conversion.
class VideoRenderer {
public:
    VideoRenderer() = default;
    ~VideoRenderer();

    bool init(void* native_window_handle);
    void render_frame(void* frame);
    void paint(SDL_Renderer* renderer, int x, int y, int width, int height);
    void update_viewport(int x, int y, int width, int height);
    std::optional<std::filesystem::path> take_screenshot() { return m_capture.take_screenshot(); }
    bool start_recording() { return m_capture.start_recording(); }
    std::optional<std::filesystem::path> stop_recording() { return m_capture.stop_recording(); }
    bool is_recording() const { return m_capture.is_recording(); }
    void shutdown();

private:
    void request_render();

    void* m_native_window_handle{nullptr};
    SDL_Texture* m_texture{nullptr};
    SDL_Renderer* m_cached_renderer{nullptr};
    std::mutex m_frame_mutex;

    // A reference to the decoded frame, not a copy. Copying meant three plane memcpys
    // per frame, roughly 180 MB/s at 1080p60; av_frame_ref lets SDL upload straight
    // from FFmpeg's own buffers.
    AVFrame* m_frame{nullptr};

    int m_frame_width{0};
    int m_frame_height{0};
    int m_viewport_x{0};
    int m_viewport_y{0};
    int m_viewport_width{0};
    int m_viewport_height{0};
    std::atomic<bool> m_has_frame{false};
    std::atomic<bool> m_render_requested{false};
    CaptureController m_capture;
};

} // namespace pm::stream
