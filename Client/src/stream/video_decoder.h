#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace pm::stream {

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder();

    bool init(uint32_t codec_id);
    bool decode(const uint8_t* data, size_t size, bool is_config);

    void* get_frame() { return frame_; }

    bool has_resolution_changed(int* out_w, int* out_h);
    void shutdown();

private:
    AVCodecContext* codec_ctx_{nullptr};
    AVFrame* frame_{nullptr};
    AVPacket* packet_{nullptr};
    // Reused packet payload, so no fresh allocation per decoded frame.
    std::vector<uint8_t> packet_buffer_;
    int last_width_{0};
    int last_height_{0};
    bool resolution_changed_{false};
};

} // namespace pm::stream
