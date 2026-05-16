#pragma once
#include <cstdint>
#include <mutex>
#include <vector>

struct SwsContext;

namespace pm::stream {

class VideoRenderer {
public:
    VideoRenderer() = default;
    ~VideoRenderer();

    bool init(void* native_window_handle);
    void render_frame(void* frame);
    void paint(void* hdc, int x, int y, int width, int height);
    void update_viewport(int x, int y, int width, int height);
    void shutdown();

private:
    void request_render();

    void* native_window_handle_{nullptr};
    SwsContext* sws_ctx_{nullptr};
    SwsContext* paint_sws_ctx_{nullptr};
    std::mutex frame_mutex_;
    std::vector<uint8_t> bgra_buffer_;
    std::vector<uint8_t> scaled_buffer_;
    int frame_width_{0};
    int frame_height_{0};
    int scaled_width_{0};
    int scaled_height_{0};
    int viewport_x_{0};
    int viewport_y_{0};
    int viewport_width_{0};
    int viewport_height_{0};
    bool has_frame_{false};
};

} // namespace pm::stream
