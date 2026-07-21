#include "capture_controller.h"

#include "../settings.h"

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

namespace pm::stream {

namespace {
constexpr AVRational CAPTURE_FPS = {60, 1};

std::filesystem::path capture_directory() {
    std::filesystem::path path = pm::get_settings_path().parent_path() / "captures";
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return ec ? std::filesystem::current_path() : path;
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
    if (!m_last_frame) return std::nullopt;

    const AVCodec* png = avcodec_find_encoder(AV_CODEC_ID_PNG);
    if (!png) return std::nullopt;

    AVCodecContext* codec = avcodec_alloc_context3(png);
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
    codec->pix_fmt = AV_PIX_FMT_RGB24;
    codec->time_base = {1, 1};
    rgb->format = codec->pix_fmt;
    rgb->width = codec->width;
    rgb->height = codec->height;

    std::optional<std::filesystem::path> result;
    if (avcodec_open2(codec, png, nullptr) >= 0 && av_frame_get_buffer(rgb, 1) >= 0) {
        SwsContext* scaler = sws_getContext(
            m_last_frame->width, m_last_frame->height, static_cast<AVPixelFormat>(m_last_frame->format),
            rgb->width, rgb->height, AV_PIX_FMT_RGB24, SWS_POINT, nullptr, nullptr, nullptr);
        if (scaler && sws_scale(scaler, m_last_frame->data, m_last_frame->linesize, 0,
                m_last_frame->height, rgb->data, rgb->linesize) > 0 &&
            avcodec_send_frame(codec, rgb) >= 0 && avcodec_receive_packet(codec, packet) >= 0) {
            const auto path = make_capture_path("Screenshot", ".png");
            std::ofstream output(path, std::ios::binary);
            if (output) {
                output.write(reinterpret_cast<const char*>(packet->data), packet->size);
                if (output.good()) result = path;
            }
        }
        sws_freeContext(scaler);
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
    if (avformat_alloc_output_context2(&m_format_context, nullptr, "mp4", m_video_path.string().c_str()) < 0 ||
        !m_format_context) {
        close_video_locked();
        return false;
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    m_video_stream = avformat_new_stream(m_format_context, encoder);
    m_video_codec = encoder ? avcodec_alloc_context3(encoder) : nullptr;
    if (!m_video_stream || !m_video_codec) {
        close_video_locked();
        return false;
    }

    m_video_codec->codec_id = AV_CODEC_ID_H264;
    m_video_codec->codec_type = AVMEDIA_TYPE_VIDEO;
    m_video_codec->width = frame->width;
    m_video_codec->height = frame->height;
    m_video_codec->pix_fmt = AV_PIX_FMT_YUV420P;
    m_video_codec->time_base = av_inv_q(CAPTURE_FPS);
    m_video_codec->framerate = CAPTURE_FPS;
    m_video_codec->bit_rate = 16'000'000;
    m_video_codec->gop_size = 60;
    m_video_codec->max_b_frames = 0;
    if (m_format_context->oformat->flags & AVFMT_GLOBALHEADER) {
        m_video_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    av_opt_set(m_video_codec->priv_data, "preset", "ultrafast", 0);
    av_opt_set(m_video_codec->priv_data, "tune", "zerolatency", 0);
    if (avcodec_open2(m_video_codec, encoder, nullptr) < 0 ||
        avcodec_parameters_from_context(m_video_stream->codecpar, m_video_codec) < 0) {
        close_video_locked();
        return false;
    }
    m_video_stream->time_base = m_video_codec->time_base;
    if (!(m_format_context->oformat->flags & AVFMT_NOFILE) &&
        avio_open(&m_format_context->pb, m_video_path.string().c_str(), AVIO_FLAG_WRITE) < 0) {
        close_video_locked();
        return false;
    }
    if (avformat_write_header(m_format_context, nullptr) < 0) {
        close_video_locked();
        return false;
    }

    m_next_pts = 0;
    m_video_has_frames = false;
    m_recording = true;
    return true;
}

bool CaptureController::encode_video_frame_locked(const AVFrame* frame) {
    if (!m_video_codec || !m_format_context || !m_video_stream ||
        frame->width != m_video_codec->width || frame->height != m_video_codec->height) return false;

    AVFrame* input = av_frame_alloc();
    if (!input) return false;
    input->format = AV_PIX_FMT_YUV420P;
    input->width = frame->width;
    input->height = frame->height;
    if (av_frame_get_buffer(input, 32) < 0) {
        av_frame_free(&input);
        return false;
    }
    m_video_scaler = sws_getCachedContext(m_video_scaler,
        frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        input->width, input->height, AV_PIX_FMT_YUV420P, SWS_POINT, nullptr, nullptr, nullptr);
    const bool scaled = m_video_scaler && sws_scale(m_video_scaler, frame->data, frame->linesize, 0,
        frame->height, input->data, input->linesize) > 0;
    input->pts = m_next_pts++;
    bool success = scaled && avcodec_send_frame(m_video_codec, input) >= 0;
    av_frame_free(&input);
    if (!success) return false;

    AVPacket* packet = av_packet_alloc();
    while (packet && avcodec_receive_packet(m_video_codec, packet) == 0) {
        av_packet_rescale_ts(packet, m_video_codec->time_base, m_video_stream->time_base);
        packet->stream_index = m_video_stream->index;
        if (av_interleaved_write_frame(m_format_context, packet) < 0) success = false;
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    m_video_has_frames = m_video_has_frames || success;
    return success;
}

void CaptureController::close_video_locked() {
    if (m_format_context && m_recording) av_write_trailer(m_format_context);
    if (m_video_scaler) sws_freeContext(m_video_scaler);
    m_video_scaler = nullptr;
    if (m_video_codec) avcodec_free_context(&m_video_codec);
    if (m_format_context) {
        if (!(m_format_context->oformat->flags & AVFMT_NOFILE) && m_format_context->pb) avio_closep(&m_format_context->pb);
        avformat_free_context(m_format_context);
        m_format_context = nullptr;
    }
    m_video_stream = nullptr;
    m_recording = false;
    m_video_has_frames = false;
    m_next_pts = 0;
}

} // namespace pm::stream
