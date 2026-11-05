// VAAPI HEVC low-power encoder (VAEntrypointEncSliceLP), VADisplay from DRM
// render node. CPU-side swscale BGRA->NV12 upload per frame.
#ifndef NUOS_VA_ENCODER_HPP
#define NUOS_VA_ENCODER_HPP

#include <cstdint>
#include <vector>
#include <string>

namespace nuos {

struct EncoderConfig {
    int width  = 1280;
    int height = 720;
    uint32_t bitrate_kbps = 15000;
    uint32_t fps = 60;
    int gop = 120;
};

struct EncodedFrame {
    std::vector<uint8_t> data;   // one HEVC access unit
    bool idr = false;
};

class VaEncoder {
public:
    ~VaEncoder();

    bool init(const EncoderConfig& cfg, std::string* err = nullptr);
    void close();
    bool ok() const { return ready_; }

    // Encodes one BGRA frame; appends the access unit to out.
    bool encode_bgra(const uint8_t* bgra, int stride, EncodedFrame& out);

    void set_bitrate_kbps(uint32_t kbps);
    void set_fps(uint32_t fps);
    void force_idr();

private:
    bool setup_surface_encoding(std::string* err);
    bool encode_nv12(const uint8_t* y, const uint8_t* uv, int y_stride,
                     int uv_stride, EncodedFrame& out);

    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;
    EncoderConfig cfg_;
    bool want_idr_ = true;
};

} // namespace nuos
#endif
