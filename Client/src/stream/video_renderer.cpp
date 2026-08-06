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
// Signals the UI thread that a new frame is ready to be painted.
constexpr UINT WM_VIDEO_RENDER = WM_APP + 2;
#endif
}

VideoRenderer::~VideoRenderer() {
    shutdown();
}

bool VideoRenderer::init(void* native_window_handle) {
    m_native_window_handle = native_window_handle;
    if (!m_native_window_handle) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_frame_mutex);
    if (!m_frame) {
        m_frame = av_frame_alloc();
        if (!m_frame) {
            return false;
        }
    }
    // Drop the last frame on reconnect, or a stale phone screen flashes up first.
    av_frame_unref(m_frame);
    m_has_frame = false;
    m_frame_width = 0;
    m_frame_height = 0;
    return true;
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

    // scrcpy only ever sends YUV420P or YUVJ420P; anything else is skipped.
    if (av_frame->format != 0 /* AV_PIX_FMT_YUV420P */ && av_frame->format != 12 /* AV_PIX_FMT_YUVJ420P */) {
        std::cerr << "[Renderer] Unsupported pixel format: " << av_frame->format << std::endl;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_frame_mutex);
        if (!m_frame) {
            return;
        }

        // Rebuild the texture when the frame size changed (rotation, quality switch).
        if (width != m_frame_width || height != m_frame_height) {
            m_frame_width = width;
            m_frame_height = height;
            // The old texture has the wrong size now.
            if (m_texture) {
                SDL_DestroyTexture(m_texture);
                m_texture = nullptr;
            }
        }

        // Reference the frame rather than copying it. FFmpeg refcounts its buffers, so
        // the decoder cannot reuse this one while the GPU is still reading from it.
        av_frame_unref(m_frame);
        if (av_frame_ref(m_frame, av_frame) < 0) {
            m_has_frame = false;
            return;
        }

        m_has_frame = true;
    }

    // Screenshots come from the decoded stream frame, not from a screenshot
    // command run on the phone.
    m_capture.on_frame(av_frame);

    request_render();
}

void VideoRenderer::paint(SDL_Renderer* renderer, int x, int y, int width, int height) {
    // The UI has caught up; the next frame has to signal it again.
    m_render_requested.store(false, std::memory_order_release);

    if (!renderer || width <= 0 || height <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_frame_mutex);
    if (!m_has_frame || !m_frame || !m_frame->data[0] || m_frame_width <= 0 || m_frame_height <= 0) {
        return;
    }

    // Create the YUV texture on demand — the GPU does the colour conversion.
    if (!m_texture || m_cached_renderer != renderer) {
        if (m_texture) {
            SDL_DestroyTexture(m_texture);
        }
        // The scaling hint has to be set before the texture is created.
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
        // Best scaling mode, so an upscaled picture stays smooth.
        SDL_SetTextureScaleMode(m_texture, SDL_ScaleModeBest);
    }

    // Upload the YUV planes straight from the decoded frame — no CPU copy, no conversion.
    SDL_UpdateYUVTexture(
        m_texture,
        nullptr,
        m_frame->data[0], m_frame->linesize[0],
        m_frame->data[1], m_frame->linesize[1],
        m_frame->data[2], m_frame->linesize[2]
    );

    // Destination rectangle that preserves the source aspect ratio.
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

    // The GPU does the scaling and the YUV->RGB conversion.
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
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    m_frame_width = 0;
    m_frame_height = 0;
    m_has_frame = false;
    m_render_requested = false;
    m_native_window_handle = nullptr;
}

void VideoRenderer::request_render() {
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(m_native_window_handle);
    // Keep at most one repaint request queued. Otherwise a UI thread that falls behind
    // at 60 FPS would work through 60 stale full-frame uploads a second instead of
    // painting the newest frame once.
    if (hwnd && !m_render_requested.exchange(true, std::memory_order_acq_rel)) {
        if (!PostMessage(hwnd, WM_VIDEO_RENDER, 0, 0)) {
            m_render_requested.store(false, std::memory_order_release);
        }
    }
#endif
}

} // namespace pm::stream
