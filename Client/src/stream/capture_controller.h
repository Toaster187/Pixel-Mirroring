#pragma once

#include <filesystem>
#include <mutex>
#include <optional>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;
struct SwsContext;

namespace pm::stream {

// Keeps a copy of the current stream frame so screenshots and recordings can be
// produced on the PC side.
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

    // One reused frame for the whole recording. Allocating a fresh one per frame meant
    // ~3 MB of allocation 60 times a second.
    AVFrame* m_encode_frame{nullptr};
    AVPacket* m_encode_packet{nullptr};
    // The pixel format the chosen encoder wants: YUV420P for software, usually NV12
    // for hardware. Stored so the scaler converts into the right one.
    int m_encode_pixel_format{0}; // AVPixelFormat

    std::filesystem::path m_video_path;
    int64_t m_start_time_us{0};
    bool m_recording{false};
    bool m_video_has_frames{false};
};

} // namespace pm::stream
