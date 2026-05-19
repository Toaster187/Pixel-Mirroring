#include "video_decoder.h"

#include <cstring>
#include <iostream>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
}

namespace pm::stream {

namespace {
AVCodecID codec_id_from_scrcpy(uint32_t codec_id) {
    switch (codec_id) {
        case 0x68323634: return AV_CODEC_ID_H264; // h264
        case 0x68323635: return AV_CODEC_ID_HEVC; // h265
        case 0x61763100: return AV_CODEC_ID_AV1;  // av1
        case 0x61763120: return AV_CODEC_ID_AV1;  // av1 + space
        default: return AV_CODEC_ID_NONE;
    }
}

void log_ffmpeg_error(const char* prefix, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(err, buf, sizeof(buf));
    std::cerr << prefix << ": " << buf << std::endl;
}
}

VideoDecoder::~VideoDecoder() {
    shutdown();
}

bool VideoDecoder::init(uint32_t codec_id) {
    shutdown();

    const AVCodecID av_codec_id = codec_id_from_scrcpy(codec_id);
    if (av_codec_id == AV_CODEC_ID_NONE) {
        std::cerr << "[Decoder] Unsupported codec id: " << codec_id << std::endl;
        return false;
    }

    const AVCodec* codec = avcodec_find_decoder(av_codec_id);
    if (!codec) {
        std::cerr << "[Decoder] FFmpeg decoder not found" << std::endl;
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!codec_ctx_ || !frame_ || !packet_) {
        std::cerr << "[Decoder] Could not allocate decoder state" << std::endl;
        shutdown();
        return false;
    }

    int ret = avcodec_open2(codec_ctx_, codec, nullptr);
    if (ret < 0) {
        log_ffmpeg_error("[Decoder] Could not open codec", ret);
        shutdown();
        return false;
    }

    return true;
}

bool VideoDecoder::decode(const uint8_t* data, size_t size, bool) {
    if (!codec_ctx_ || !frame_ || !packet_ || !data || size == 0) {
        return false;
    }

    av_packet_unref(packet_);
    int ret = av_new_packet(packet_, static_cast<int>(size));
    if (ret < 0) {
        log_ffmpeg_error("[Decoder] Could not allocate packet", ret);
        return false;
    }

    std::memcpy(packet_->data, data, size);
    ret = avcodec_send_packet(codec_ctx_, packet_);
    av_packet_unref(packet_);
    if (ret < 0) {
        log_ffmpeg_error("[Decoder] Could not send packet", ret);
        return false;
    }

    bool got_frame = false;
    while (true) {
        ret = avcodec_receive_frame(codec_ctx_, frame_);
        if (ret == 0) {
            // Cave man got frame! Break loop, keep frame in frame_ so next receive not kill it!
            if (frame_->width != last_width_ || frame_->height != last_height_) {
                last_width_ = frame_->width;
                last_height_ = frame_->height;
                resolution_changed_ = true;
            }
            got_frame = true;
            break;
        }
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            log_ffmpeg_error("[Decoder] Could not receive frame", ret);
            break;
        }
    }

    return got_frame;
}

bool VideoDecoder::has_resolution_changed(int* out_w, int* out_h) {
    if (!resolution_changed_) {
        return false;
    }
    if (out_w) *out_w = last_width_;
    if (out_h) *out_h = last_height_;
    resolution_changed_ = false;
    return true;
}

void VideoDecoder::shutdown() {
    if (packet_) {
        av_packet_free(&packet_);
    }
    if (frame_) {
        av_frame_free(&frame_);
    }
    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
    }
    last_width_ = 0;
    last_height_ = 0;
    resolution_changed_ = false;
}

} // namespace pm::stream
