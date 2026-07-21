#include <iostream>
extern "C" {
#include <libavcodec/avcodec.h>
}
int main() {
    const AVCodec* png = avcodec_find_encoder(AV_CODEC_ID_PNG);
    std::cout << "PNG encoder: " << (png ? "found" : "not found") << "\n";
    const AVCodec* h264 = avcodec_find_encoder(AV_CODEC_ID_H264);
    std::cout << "H264 encoder: " << (h264 ? "found" : "not found") << "\n";
    return 0;
}
