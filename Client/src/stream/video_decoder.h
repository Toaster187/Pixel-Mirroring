#pragma once
#include <cstdint>
#include <cstddef>
namespace pm::stream {

class VideoDecoder {
public:
    VideoDecoder() = default;
    ~VideoDecoder() = default;

    bool init(uint32_t codec_id);
    bool decode(const uint8_t* data, size_t size, bool is_config);
    
    // Placeholder - FFmpeg AVFrame* equivalent
    void* get_frame() { return nullptr; }
    
    bool has_resolution_changed(int* out_w, int* out_h) { return false; }
    void shutdown() {}
};

} // namespace pm::stream
