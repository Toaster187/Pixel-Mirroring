#include "capture_controller.h"

#include "../settings.h"
#include "../util/encoding.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include <shlobj.h>
#endif

namespace pm::stream {

namespace {
constexpr AVRational CAPTURE_FPS = {60, 1};

std::filesystem::path capture_directory() {
#ifdef _WIN32
    // Wide, not narrow: a picture cave under a home cave the tribe's old letter-table
    // cannot spell came back full of "?", and then the recording was neither saved
    // nor thrown to the phone. See the letter-table rules in CLAUDE.md.
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, 0, path))) {
        std::filesystem::path full_path = std::filesystem::path(path) / "PixelMirroring";
        std::error_code ec;
        std::filesystem::create_directories(full_path, ec);
        return ec ? std::filesystem::current_path() : full_path;
    }
#else
    const char* home = std::getenv("HOME");
    if (home) {
        std::filesystem::path full_path = std::filesystem::path(home) / "Pictures" / "PixelMirroring";
        std::error_code ec;
        std::filesystem::create_directories(full_path, ec);
        return ec ? std::filesystem::current_path() : full_path;
    }
#endif
    std::filesystem::path fallback = pm::get_settings_path().parent_path() / "captures";
    std::error_code ec;
    std::filesystem::create_directories(fallback, ec);
    return ec ? std::filesystem::current_path() : fallback;
}

// Ugg! Cave man used to grind every recorded picture by hand on the CPU (libx264).
// Modern fire-boxes have a carving stone built into the graphics card; Media
// Foundation on Windows hands us that one. If the machine has none, or the driver
// refuses, we quietly fall back to the old hand-grinding — a recording must never
// fail just because the fancy path is missing.
const AVCodec* find_h264_encoder(bool* out_is_hardware) {
    const char* hardware_names[] = {
        "h264_mf",    // Windows Media Foundation (Intel QSV / NVENC / AMD VCE under the hood)
        "h264_nvenc", // NVIDIA, if this FFmpeg build has it
        "h264_qsv",   // Intel QuickSync
        "h264_amf",   // AMD
    };

    for (const char* name : hardware_names) {
        if (const AVCodec* codec = avcodec_find_encoder_by_name(name)) {
            if (out_is_hardware) *out_is_hardware = true;
            return codec;
        }
    }

    if (out_is_hardware) *out_is_hardware = false;
    return avcodec_find_encoder(AV_CODEC_ID_H264);
}

// Picks a pixel shape the encoder actually understands. Hardware encoders usually
// want NV12, the software one wants YUV420P — asking for the wrong one makes
// avcodec_open2 refuse.
AVPixelFormat pick_encoder_pixel_format(const AVCodec* codec) {
    const AVPixelFormat* formats = nullptr;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                     reinterpret_cast<const void**>(&formats), nullptr) < 0) {
        formats = nullptr;
    }
#else
    formats = codec->pix_fmts;
#endif

    if (!formats) {
        return AV_PIX_FMT_YUV420P;
    }

    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
    }
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f) {
        if (*f == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
    }
    return formats[0] != AV_PIX_FMT_NONE ? formats[0] : AV_PIX_FMT_YUV420P;
}

std::string capture_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local_time{};
#ifdef _WIN32
    localtime_s(&local_time, &time);
#else
    localtime_r(&time, &local_time);
#endif
    std::ostringstream out;
    out << std::put_time(&local_time, "%Y-%m-%d_%H-%M-%S") << "_"
        << std::setw(3) << std::setfill('0') << milliseconds;
    return out.str();
}
}

CaptureController::~CaptureController() {
    std::lock_guard<std::mutex> lock(m_mutex);
    close_video_locked();
    if (m_last_frame) {
        av_frame_free(&m_last_frame);
    }
}

std::filesystem::path CaptureController::make_capture_path(const char* prefix, const char* extension) const {
    return capture_directory() / (std::string(prefix) + "_" + capture_timestamp() + extension);
}

int64_t get_time_us() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

void CaptureController::on_frame(const AVFrame* frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0) return;

    std::lock_guard<std::mutex> lock(m_mutex);
    AVFrame* copied = av_frame_clone(frame);
    if (!copied) return;
    if (m_last_frame) av_frame_free(&m_last_frame);
    m_last_frame = copied;

    if (m_recording) {
        encode_video_frame_locked(frame);
    }
}

std::optional<std::filesystem::path> CaptureController::take_screenshot() {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_last_frame) {
        return std::nullopt;
    }

    const AVCodec* bmp = avcodec_find_encoder(AV_CODEC_ID_BMP);
    if (!bmp) {
        return std::nullopt;
    }

    AVCodecContext* codec = avcodec_alloc_context3(bmp);
    AVFrame* rgb = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    if (!codec || !rgb || !packet) {
        avcodec_free_context(&codec);
        av_frame_free(&rgb);
        av_packet_free(&packet);
        return std::nullopt;
    }

    codec->width = m_last_frame->width;
    codec->height = m_last_frame->height;
    codec->pix_fmt = AV_PIX_FMT_BGR24;
    codec->time_base = {1, 1};
    rgb->format = codec->pix_fmt;
    rgb->width = codec->width;
    rgb->height = codec->height;

    std::optional<std::filesystem::path> result;

    if (avcodec_open2(codec, bmp, nullptr) >= 0) {
        if (av_frame_get_buffer(rgb, 1) >= 0) {
            SwsContext* scaler = sws_getContext(
                m_last_frame->width, m_last_frame->height, static_cast<AVPixelFormat>(m_last_frame->format),
                rgb->width, rgb->height, AV_PIX_FMT_BGR24, SWS_POINT, nullptr, nullptr, nullptr);
            if (scaler) {
                if (sws_scale(scaler, m_last_frame->data, m_last_frame->linesize, 0,
                    m_last_frame->height, rgb->data, rgb->linesize) > 0) {
                    if (avcodec_send_frame(codec, rgb) >= 0) {
                        if (avcodec_receive_packet(codec, packet) >= 0) {
                            const auto path = make_capture_path("Screenshot", ".bmp");
                            std::ofstream output(path, std::ios::binary);
                            if (output) {
                                output.write(reinterpret_cast<const char*>(packet->data), packet->size);
                                if (output.good()) {
                                    result = path;
                                }
                            }
                        }
                    }
                }
                sws_freeContext(scaler);
            }
        }
    }

    av_packet_free(&packet);
    av_frame_free(&rgb);
    avcodec_free_context(&codec);
    return result;
}

bool CaptureController::start_recording() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_recording || !m_last_frame) return false;
    return open_video_locked(m_last_frame);
}

std::optional<std::filesystem::path> CaptureController::stop_recording() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_recording) return std::nullopt;

    if (m_video_codec) {
        avcodec_send_frame(m_video_codec, nullptr);
        AVPacket* packet = av_packet_alloc();
        while (packet && avcodec_receive_packet(m_video_codec, packet) == 0) {
            av_packet_rescale_ts(packet, m_video_codec->time_base, m_video_stream->time_base);
            packet->stream_index = m_video_stream->index;
            av_interleaved_write_frame(m_format_context, packet);
            av_packet_unref(packet);
        }
        av_packet_free(&packet);
    }

    const bool completed = m_video_has_frames;
    const auto path = m_video_path;
    close_video_locked();
    return completed ? std::optional<std::filesystem::path>(path) : std::nullopt;
}

bool CaptureController::is_recording() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_recording;
}

bool CaptureController::open_video_locked(const AVFrame* frame) {
    m_video_path = make_capture_path("Aufnahme", ".mp4");
    // FFmpeg's own file-opener turns the name back into wide stones with CP_UTF8, so
    // it wants UTF-8 — path::string() would hand it the tribe's old table instead.
    const std::string video_path_utf8 = pm::util::path_to_utf8(m_video_path);
    if (avformat_alloc_output_context2(&m_format_context, nullptr, "mp4", video_path_utf8.c_str()) < 0 ||
        !m_format_context) {
        close_video_locked();
        return false;
    }

    bool is_hardware = false;
    const AVCodec* encoder = find_h264_encoder(&is_hardware);
    const AVCodec* fallback = is_hardware ? avcodec_find_encoder(AV_CODEC_ID_H264) : nullptr;

    // Try the hardware carving stone first, then the software one.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt == 1) {
            if (!fallback || fallback == encoder) break;
            // Hardware said no — throw away the half-built state and grind by hand.
            if (m_video_codec) avcodec_free_context(&m_video_codec);
            encoder = fallback;
            is_hardware = false;
        }
        if (!encoder) break;

        if (!m_video_stream) {
            m_video_stream = avformat_new_stream(m_format_context, encoder);
        }
        m_video_codec = avcodec_alloc_context3(encoder);
        if (!m_video_stream || !m_video_codec) break;

        m_encode_pixel_format = pick_encoder_pixel_format(encoder);

        m_video_codec->codec_id = AV_CODEC_ID_H264;
        m_video_codec->codec_type = AVMEDIA_TYPE_VIDEO;
        m_video_codec->width = frame->width;
        m_video_codec->height = frame->height;
        m_video_codec->pix_fmt = static_cast<AVPixelFormat>(m_encode_pixel_format);
        m_video_codec->time_base = {1, 1000};
        m_video_codec->framerate = {60, 1};
        m_video_codec->bit_rate = 16'000'000;
        m_video_codec->gop_size = 60;
        m_video_codec->max_b_frames = 0;
        if (m_format_context->oformat->flags & AVFMT_GLOBALHEADER) {
            m_video_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        // Only the software encoder knows these; hardware encoders ignore them.
        if (!is_hardware) {
            av_opt_set(m_video_codec->priv_data, "preset", "ultrafast", 0);
            av_opt_set(m_video_codec->priv_data, "tune", "zerolatency", 0);
        }

        if (avcodec_open2(m_video_codec, encoder, nullptr) >= 0 &&
            avcodec_parameters_from_context(m_video_stream->codecpar, m_video_codec) >= 0) {
            break; // this one works
        }

        if (attempt == 1 || !fallback || fallback == encoder) {
            close_video_locked();
            return false;
        }
    }

    if (!m_video_codec || !m_video_stream) {
        close_video_locked();
        return false;
    }
    m_video_stream->time_base = m_video_codec->time_base;
    if (!(m_format_context->oformat->flags & AVFMT_NOFILE) &&
        avio_open(&m_format_context->pb, video_path_utf8.c_str(), AVIO_FLAG_WRITE) < 0) {
        close_video_locked();
        return false;
    }
    if (avformat_write_header(m_format_context, nullptr) < 0) {
        close_video_locked();
        return false;
    }

    // One reusable frame + packet for the whole recording.
    m_encode_frame = av_frame_alloc();
    m_encode_packet = av_packet_alloc();
    if (!m_encode_frame || !m_encode_packet) {
        close_video_locked();
        return false;
    }
    m_encode_frame->format = m_encode_pixel_format;
    m_encode_frame->width = m_video_codec->width;
    m_encode_frame->height = m_video_codec->height;
    if (av_frame_get_buffer(m_encode_frame, 32) < 0) {
        close_video_locked();
        return false;
    }

    m_start_time_us = get_time_us();
    m_video_has_frames = false;
    m_recording = true;
    return true;
}

bool CaptureController::encode_video_frame_locked(const AVFrame* frame) {
    if (!m_video_codec || !m_format_context || !m_video_stream || !m_encode_frame || !m_encode_packet ||
        frame->width != m_video_codec->width || frame->height != m_video_codec->height) return false;

    // Cave man makes the tablet writable again instead of carving a new one.
    if (av_frame_make_writable(m_encode_frame) < 0) return false;

    m_video_scaler = sws_getCachedContext(m_video_scaler,
        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        m_encode_frame->width, m_encode_frame->height,
        static_cast<AVPixelFormat>(m_encode_pixel_format), SWS_POINT, nullptr, nullptr, nullptr);
    const bool scaled = m_video_scaler && sws_scale(m_video_scaler, frame->data, frame->linesize, 0,
        frame->height, m_encode_frame->data, m_encode_frame->linesize) > 0;
    if (!scaled) return false;

    int64_t duration_us = get_time_us() - m_start_time_us;
    if (duration_us < 0) duration_us = 0;

    // Scale from microseconds to milliseconds (our time_base is 1/1000)
    m_encode_frame->pts = av_rescale_q(duration_us, {1, 1000000}, m_video_codec->time_base);

    if (avcodec_send_frame(m_video_codec, m_encode_frame) < 0) return false;

    bool success = true;
    while (avcodec_receive_packet(m_video_codec, m_encode_packet) == 0) {
        av_packet_rescale_ts(m_encode_packet, m_video_codec->time_base, m_video_stream->time_base);
        m_encode_packet->stream_index = m_video_stream->index;
        if (av_interleaved_write_frame(m_format_context, m_encode_packet) < 0) success = false;
        av_packet_unref(m_encode_packet);
    }

    m_video_has_frames = m_video_has_frames || success;
    return success;
}

void CaptureController::close_video_locked() {
    if (m_format_context && m_recording) av_write_trailer(m_format_context);
    if (m_video_scaler) sws_freeContext(m_video_scaler);
    m_video_scaler = nullptr;
    if (m_encode_frame) av_frame_free(&m_encode_frame);
    if (m_encode_packet) av_packet_free(&m_encode_packet);
    if (m_video_codec) avcodec_free_context(&m_video_codec);
    if (m_format_context) {
        if (!(m_format_context->oformat->flags & AVFMT_NOFILE) && m_format_context->pb) avio_closep(&m_format_context->pb);
        avformat_free_context(m_format_context);
        m_format_context = nullptr;
    }
    m_video_stream = nullptr;
    m_recording = false;
    m_video_has_frames = false;
}

} // namespace pm::stream
