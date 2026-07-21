#pragma once

#include <filesystem>
#include <mutex>
#include <optional>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVStream;
struct SwsContext;

namespace pm::stream {

// Cave man keep copy of stream picture, make photo and moving picture on PC.
class CaptureController {
public:
    CaptureController() = default;
    ~CaptureController();

    void on_frame(const AVFrame* frame);
    std::optional<std::filesystem::path> take_screenshot();
    bool start_recording();
    std::optional<std::filesystem::path> stop_recording();
    bool is_recording() const;

private:
    bool open_video_locked(const AVFrame* frame);
    bool encode_video_frame_locked(const AVFrame* frame);
    void close_video_locked();
    std::filesystem::path make_capture_path(const char* prefix, const char* extension) const;

    mutable std::mutex m_mutex;
    AVFrame* m_last_frame{nullptr};
    AVFormatContext* m_format_context{nullptr};
    AVCodecContext* m_video_codec{nullptr};
    AVStream* m_video_stream{nullptr};
    SwsContext* m_video_scaler{nullptr};
    std::filesystem::path m_video_path;
    int64_t m_start_time_us{0};
    int64_t m_next_pts{0};
    bool m_recording{false};
    bool m_video_has_frames{false};
};

} // namespace pm::stream
