#include "video_renderer.h"

#include <iostream>

extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace pm::stream {

namespace {
#ifdef _WIN32
constexpr UINT WM_VIDEO_RENDER = WM_APP + 2;
#endif
}

VideoRenderer::~VideoRenderer() {
    shutdown();
}

bool VideoRenderer::init(void* native_window_handle) {
    native_window_handle_ = native_window_handle;
    return native_window_handle_ != nullptr;
}

void VideoRenderer::render_frame(void* frame) {
    if (!native_window_handle_ || !frame) {
        return;
    }

    AVFrame* av_frame = static_cast<AVFrame*>(frame);
    const int width = av_frame->width;
    const int height = av_frame->height;
    if (width <= 0 || height <= 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        if (width != frame_width_ || height != frame_height_) {
            frame_width_ = width;
            frame_height_ = height;
            bgra_buffer_.assign(static_cast<size_t>(width) * height * 4, 0);
            if (sws_ctx_) {
                sws_freeContext(sws_ctx_);
                sws_ctx_ = nullptr;
            }
        }

        sws_ctx_ = sws_getCachedContext(
            sws_ctx_,
            width,
            height,
            static_cast<AVPixelFormat>(av_frame->format),
            width,
            height,
            AV_PIX_FMT_BGRA,
            SWS_LANCZOS,
            nullptr,
            nullptr,
            nullptr
        );
        if (!sws_ctx_) {
            std::cerr << "[Renderer] Could not create scaler" << std::endl;
            return;
        }

        uint8_t* dst_data[4] = { bgra_buffer_.data(), nullptr, nullptr, nullptr };
        int dst_linesize[4] = { width * 4, 0, 0, 0 };
        sws_scale(
            sws_ctx_,
            av_frame->data,
            av_frame->linesize,
            0,
            height,
            dst_data,
            dst_linesize
        );
        has_frame_ = true;
    }

    request_render();
}

void VideoRenderer::paint(void* hdc, int x, int y, int width, int height) {
#ifdef _WIN32
    HDC target = static_cast<HDC>(hdc);
    if (!target || width <= 0 || height <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (!has_frame_ || bgra_buffer_.empty() || frame_width_ <= 0 || frame_height_ <= 0) {
        return;
    }

    int old_mode = SetStretchBltMode(target, HALFTONE);
    POINT old_origin{};
    SetBrushOrgEx(target, 0, 0, &old_origin);

    const uint8_t* pixels = bgra_buffer_.data();
    int source_w = frame_width_;
    int source_h = frame_height_;

    if (width != frame_width_ || height != frame_height_) {
        if (width != scaled_width_ || height != scaled_height_) {
            scaled_width_ = width;
            scaled_height_ = height;
            scaled_buffer_.assign(static_cast<size_t>(width) * height * 4, 0);
            if (paint_sws_ctx_) {
                sws_freeContext(paint_sws_ctx_);
                paint_sws_ctx_ = nullptr;
            }
        }

        paint_sws_ctx_ = sws_getCachedContext(
            paint_sws_ctx_,
            frame_width_,
            frame_height_,
            AV_PIX_FMT_BGRA,
            width,
            height,
            AV_PIX_FMT_BGRA,
            SWS_LANCZOS,
            nullptr,
            nullptr,
            nullptr
        );
        if (paint_sws_ctx_ && !scaled_buffer_.empty()) {
            const uint8_t* src_data[4] = { bgra_buffer_.data(), nullptr, nullptr, nullptr };
            int src_linesize[4] = { frame_width_ * 4, 0, 0, 0 };
            uint8_t* dst_data[4] = { scaled_buffer_.data(), nullptr, nullptr, nullptr };
            int dst_linesize[4] = { width * 4, 0, 0, 0 };
            sws_scale(paint_sws_ctx_, src_data, src_linesize, 0, frame_height_, dst_data, dst_linesize);
            pixels = scaled_buffer_.data();
            source_w = width;
            source_h = height;
        }
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = source_w;
    bmi.bmiHeader.biHeight = -source_h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    StretchDIBits(
        target,
        x,
        y,
        width,
        height,
        0,
        0,
        source_w,
        source_h,
        pixels,
        &bmi,
        DIB_RGB_COLORS,
        SRCCOPY
    );
    SetBrushOrgEx(target, old_origin.x, old_origin.y, nullptr);
    SetStretchBltMode(target, old_mode);
#else
    (void)hdc;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
#endif
}

void VideoRenderer::update_viewport(int x, int y, int width, int height) {
    viewport_x_ = x;
    viewport_y_ = y;
    viewport_width_ = width;
    viewport_height_ = height;
}

void VideoRenderer::shutdown() {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    if (sws_ctx_) {
        sws_freeContext(sws_ctx_);
        sws_ctx_ = nullptr;
    }
    if (paint_sws_ctx_) {
        sws_freeContext(paint_sws_ctx_);
        paint_sws_ctx_ = nullptr;
    }
    bgra_buffer_.clear();
    scaled_buffer_.clear();
    frame_width_ = 0;
    frame_height_ = 0;
    scaled_width_ = 0;
    scaled_height_ = 0;
    has_frame_ = false;
    native_window_handle_ = nullptr;
}

void VideoRenderer::request_render() {
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(native_window_handle_);
    if (hwnd) {
        PostMessage(hwnd, WM_VIDEO_RENDER, 0, 0);
    }
#endif
}

} // namespace pm::stream
