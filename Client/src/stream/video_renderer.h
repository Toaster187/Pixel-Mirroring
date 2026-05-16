#pragma once
#include <cstdint>
#include <mutex>
#include <vector>
#include <memory>
#include <atomic>

struct SDL_Texture;
struct SDL_Renderer;

namespace pm::stream {

// Cave man render frame on GPU — no CPU scaling, no double convert
class VideoRenderer {
public:
    VideoRenderer() = default;
    ~VideoRenderer();

    bool init(void* native_window_handle);
    void render_frame(void* frame);
    void paint(SDL_Renderer* renderer, int x, int y, int width, int height);
    void update_viewport(int x, int y, int width, int height);
    void shutdown();

private:
    void request_render();

    void* m_native_window_handle{nullptr};
    SDL_Texture* m_texture{nullptr};
    SDL_Renderer* m_cached_renderer{nullptr};
    std::mutex m_frame_mutex;

    // YUV plane data — cave man copy planes, GPU do rest
    std::vector<uint8_t> m_y_plane;
    std::vector<uint8_t> m_u_plane;
    std::vector<uint8_t> m_v_plane;
    int m_y_linesize{0};
    int m_u_linesize{0};
    int m_v_linesize{0};

    int m_frame_width{0};
    int m_frame_height{0};
    int m_viewport_x{0};
    int m_viewport_y{0};
    int m_viewport_width{0};
    int m_viewport_height{0};
    std::atomic<bool> m_has_frame{false};
};

} // namespace pm::stream
