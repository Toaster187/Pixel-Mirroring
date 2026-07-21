#include "video_renderer.h"

#include <iostream>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
}

#include <SDL2/SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace pm::stream {

namespace {
#ifdef _WIN32
// Cave man signal main thread: new frame ready, paint now
constexpr UINT WM_VIDEO_RENDER = WM_APP + 2;
#endif
}

VideoRenderer::~VideoRenderer() {
    shutdown();
}

bool VideoRenderer::init(void* native_window_handle) {
    m_native_window_handle = native_window_handle;
    return m_native_window_handle != nullptr;
}

void VideoRenderer::render_frame(void* frame) {
    if (!m_native_window_handle || !frame) {
        return;
    }

    AVFrame* av_frame = static_cast<AVFrame*>(frame);
    const int width = av_frame->width;
    const int height = av_frame->height;
    if (width <= 0 || height <= 0) {
        return;
    }

    // Cave man only handle YUV420P or YUVJ420P — that what scrcpy send
    // Other formats = no good, skip
    if (av_frame->format != 0 /* AV_PIX_FMT_YUV420P */ && av_frame->format != 12 /* AV_PIX_FMT_YUVJ420P */) {
        std::cerr << "[Renderer] Unsupported pixel format: " << av_frame->format << std::endl;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_frame_mutex);

        // Resize if frame size changed
        if (width != m_frame_width || height != m_frame_height) {
            m_frame_width = width;
            m_frame_height = height;
            // Destroy old texture — size wrong now
            if (m_texture) {
                SDL_DestroyTexture(m_texture);
                m_texture = nullptr;
            }
        }

        // Copy Y plane
        int y_size = av_frame->linesize[0] * height;
        m_y_linesize = av_frame->linesize[0];
        m_y_plane.resize(y_size);
        std::memcpy(m_y_plane.data(), av_frame->data[0], y_size);

        // Copy U plane (half height)
        int uv_height = (height + 1) / 2;
        int u_size = av_frame->linesize[1] * uv_height;
        m_u_linesize = av_frame->linesize[1];
        m_u_plane.resize(u_size);
        std::memcpy(m_u_plane.data(), av_frame->data[1], u_size);

        // Copy V plane (half height)
        int v_size = av_frame->linesize[2] * uv_height;
        m_v_linesize = av_frame->linesize[2];
        m_v_plane.resize(v_size);
        std::memcpy(m_v_plane.data(), av_frame->data[2], v_size);

        m_has_frame = true;
    }

    request_render();
}

void VideoRenderer::paint(SDL_Renderer* renderer, int x, int y, int width, int height) {
    // Cave man UI is ready now. New frame after this must wake it again.
    m_render_requested.store(false, std::memory_order_release);

    if (!renderer || width <= 0 || height <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_frame_mutex);
    if (!m_has_frame || m_frame_width <= 0 || m_frame_height <= 0) {
        return;
    }

    // Create YUV texture if needed — GPU handle color conversion
    if (!m_texture || m_cached_renderer != renderer) {
        if (m_texture) {
            SDL_DestroyTexture(m_texture);
        }
        // Cave man set best scaling quality before make texture
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "best");
        m_texture = SDL_CreateTexture(
            renderer,
            SDL_PIXELFORMAT_IYUV,
            SDL_TEXTUREACCESS_STREAMING,
            m_frame_width,
            m_frame_height
        );
        m_cached_renderer = renderer;
        if (!m_texture) {
            std::cerr << "[Renderer] Could not create YUV texture: " << SDL_GetError() << std::endl;
            return;
        }
        // Cave man set scale mode to best so big screen look nice
        SDL_SetTextureScaleMode(m_texture, SDL_ScaleModeBest);
    }

    // Upload YUV planes directly — no CPU conversion needed
    SDL_UpdateYUVTexture(
        m_texture,
        nullptr,
        m_y_plane.data(), m_y_linesize,
        m_u_plane.data(), m_u_linesize,
        m_v_plane.data(), m_v_linesize
    );

    // Calculate aspect-ratio-preserving destination rect
    double src_aspect = static_cast<double>(m_frame_width) / m_frame_height;
    double dst_aspect = static_cast<double>(width) / height;

    int target_w = width;
    int target_h = height;

    if (src_aspect > dst_aspect) {
        target_h = static_cast<int>(width / src_aspect);
    } else {
        target_w = static_cast<int>(height * src_aspect);
    }

    int offset_x = x + (width - target_w) / 2;
    int offset_y = y + (height - target_h) / 2;

    SDL_Rect dst_rect = { offset_x, offset_y, target_w, target_h };

    // GPU do scaling and YUV->RGB — cave man just copy and blit
    SDL_RenderCopy(renderer, m_texture, nullptr, &dst_rect);
}

void VideoRenderer::update_viewport(int x, int y, int width, int height) {
    m_viewport_x = x;
    m_viewport_y = y;
    m_viewport_width = width;
    m_viewport_height = height;
}

void VideoRenderer::shutdown() {
    std::lock_guard<std::mutex> lock(m_frame_mutex);
    if (m_texture) {
        SDL_DestroyTexture(m_texture);
        m_texture = nullptr;
    }
    m_cached_renderer = nullptr;
    m_y_plane.clear();
    m_u_plane.clear();
    m_v_plane.clear();
    m_frame_width = 0;
    m_frame_height = 0;
    m_has_frame = false;
    m_render_requested = false;
    m_native_window_handle = nullptr;
}

void VideoRenderer::request_render() {
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(m_native_window_handle);
    // Cave man keep only one render knock queued. At 60 FPS, slow UI no make
    // 60 stale full-frame uploads per second; it paints newest frame once.
    if (hwnd && !m_render_requested.exchange(true, std::memory_order_acq_rel)) {
        if (!PostMessage(hwnd, WM_VIDEO_RENDER, 0, 0)) {
            m_render_requested.store(false, std::memory_order_release);
        }
    }
#endif
}

} // namespace pm::stream
