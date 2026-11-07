// VAAPI HEVC encoder — implemented via libavcodec hevc_vaapi.
//
// The hand-rolled VAEntrypointEncSliceLP buffer flow produced empty coded
// buffers on iHD (missing driver-internal SPS/PPS generation details), while
// ffmpeg's hevc_vaapi encodes fine on the exact same hardware/driver. This
// implementation drives libavcodec directly:
//   BGRA (ring) -> swscale NV12 (CPU) -> av_hwframe upload -> hevc_vaapi
#include <atomic>
#include <mutex>
#include <vector>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>

#include "va_encoder.hpp"

struct VaEncoder::Impl {
    SwsContext* sws = nullptr;
    uint8_t* nv12 = nullptr;
    int nv12_size = 0;

    AVBufferRef* hw_device = nullptr;
    AVBufferRef* hw_frames = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* hw_frame = nullptr;
    AVFrame* sw_frame = nullptr;
    AVPacket* pkt = nullptr;

    std::mutex m;
    std::atomic<bool> force_idr_{false};
};

VaEncoder::~VaEncoder() { close(); }

bool VaEncoder::ok() const { return ready_; }
void VaEncoder::force_idr() { impl_->force_idr_ = true; }
void VaEncoder::set_bitrate_kbps(uint32_t) { /* fixed at open; v2 */ }
void VaEncoder::set_fps(uint32_t) { /* fixed at open; v2 */ }

void VaEncoder::close() {
    if (!impl_) return;
    std::lock_guard<std::mutex> lk(impl_->m);
    ready_ = false;
    if (impl_->pkt) av_packet_free(&impl_->pkt);
    if (impl_->sw_frame) av_frame_free(&impl_->sw_frame);
    if (impl_->hw_frame) av_frame_free(&impl_->hw_frame);
    if (impl_->ctx) avcodec_free_context(&impl_->ctx);
    if (impl_->hw_frames) av_buffer_unref(&impl_->hw_frames);
    if (impl_->hw_device) av_buffer_unref(&impl_->hw_device);
    if (impl_->sws) sws_freeContext(impl_->sws);
    if (impl_->nv12) av_free(impl_->nv12);
    delete impl_;
    impl_ = nullptr;
}

bool VaEncoder::init(const EncoderConfig& cfg, std::string* err) {
    cfg_ = cfg;
    impl_ = new Impl;
    impl_->nv12_size = av_image_get_buffer_size(AV_PIX_FMT_NV12, cfg_.width,
                                                cfg_.height, 1);
    impl_->nv12 = (uint8_t*)av_malloc((size_t)impl_->nv12_size);
    impl_->sws = sws_getContext(cfg_.width, cfg_.height, AV_PIX_FMT_BGRA,
                                cfg_.width, cfg_.height, AV_PIX_FMT_NV12,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->sws || !impl_->nv12) {
        if (err) *err = "swscale setup failed";
        return false;
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("hevc_vaapi");
    if (!codec) codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        if (err) *err = "hevc encoder not found in libavcodec";
        return false;
    }

    impl_->ctx = avcodec_alloc_context3(codec);
    if (!impl_->ctx) {
        if (err) *err = "avcodec_alloc_context3 failed";
        return false;
    }
    impl_->ctx->width = cfg_.width;
    impl_->ctx->height = cfg_.height;
    impl_->ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    impl_->ctx->time_base = AVRational{1, (int)cfg_.fps};
    impl_->ctx->framerate = AVRational{(int)cfg_.fps, 1};
    impl_->ctx->bit_rate = (int64_t)cfg_.bitrate_kbps * 1000;
    impl_->ctx->rc_max_rate = impl_->ctx->bit_rate;
    impl_->ctx->rc_buffer_size = (int)impl_->ctx->bit_rate; /* 1s HRD */
    impl_->ctx->gop_size = (int)cfg_.gop;
    impl_->ctx->max_b_frames = 0;
    av_opt_set(impl_->ctx->priv_data, "rc_mode", "CBR", 0);

    if (av_hwdevice_ctx_create(&impl_->hw_device, AV_HWDEVICE_TYPE_VAAPI,
                               "/dev/dri/renderD128", nullptr, 0) < 0) {
        if (err) *err = "av_hwdevice_ctx_create(VAAPI) failed";
        return false;
    }
    impl_->ctx->hw_device_ctx = av_buffer_ref(impl_->hw_device);

    impl_->hw_frames = av_hwframe_ctx_alloc(impl_->hw_device);
    AVHWFramesConstraints* cons =
        av_hwdevice_get_hwframe_constraints(impl_->hw_device, nullptr);
    auto* fctx = (AVHWFramesContext*)impl_->hw_frames->data;
    fctx->format = AV_PIX_FMT_VAAPI;
    fctx->sw_format = AV_PIX_FMT_NV12;
    fctx->width = cfg_.width;
    fctx->height = cfg_.height;
    if (cons) av_hwframe_constraints_free(&cons);
    if (av_hwframe_ctx_init(impl_->hw_frames) < 0) {
        if (err) *err = "hw frames ctx init failed";
        return false;
    }
    impl_->ctx->hw_frames_ctx = av_buffer_ref(impl_->hw_frames);

    if (avcodec_open2(impl_->ctx, codec, nullptr) < 0) {
        if (err) *err = "avcodec_open2(hevc_vaapi) failed";
        return false;
    }

    impl_->sw_frame = av_frame_alloc();
    impl_->sw_frame->format = AV_PIX_FMT_NV12;
    impl_->sw_frame->width = cfg_.width;
    impl_->sw_frame->height = cfg_.height;
    impl_->hw_frame = av_frame_alloc();
    impl_->hw_frame->format = AV_PIX_FMT_VAAPI;
    impl_->hw_frame->width = cfg_.width;
    impl_->hw_frame->height = cfg_.height;
    impl_->hw_frame->hw_frames_ctx = av_buffer_ref(impl_->hw_frames);
    impl_->pkt = av_packet_alloc();

    ready_ = true;
    return true;
}

bool VaEncoder::encode_bgra(const uint8_t* bgra, int stride, EncodedFrame& out) {
    out.data.clear();
    out.idr = false;
    if (!ready_) return false;
    std::lock_guard<std::mutex> lk(impl_->m);

    uint8_t* dst_slice[2] = { impl_->nv12,
                              impl_->nv12 + (size_t)cfg_.width * cfg_.height };
    const uint8_t* src_slice[1] = { bgra };
    int src_stride[1] = { stride };
    int dst_stride[2] = { cfg_.width, cfg_.width };
    sws_scale(impl_->sws, src_slice, src_stride, 0, cfg_.height,
              dst_slice, dst_stride);

    if (av_frame_get_buffer(impl_->sw_frame, 0) < 0) return false;
    if (av_image_fill_arrays(impl_->sw_frame->data, impl_->sw_frame->linesize,
                             impl_->nv12, AV_PIX_FMT_NV12, cfg_.width,
                             cfg_.height, 1) < 0)
        return false;

    if (impl_->force_idr_.exchange(false))
        impl_->sw_frame->pict_type = AV_PICTURE_TYPE_I;

    if (av_frame_make_writable(impl_->hw_frame) < 0) return false;
    if (av_hwframe_transfer_data(impl_->hw_frame, impl_->sw_frame, 0) < 0)
        return false;
    impl_->hw_frame->pict_type = impl_->sw_frame->pict_type;
    impl_->hw_frame->pts = (int64_t)impl_->frame_num++;

    if (avcodec_send_frame(impl_->ctx, impl_->hw_frame) < 0) return false;
    bool got = false;
    while (avcodec_receive_packet(impl_->ctx, impl_->pkt) == 0) {
        out.data.assign(impl_->pkt->data, impl_->pkt->data + impl_->pkt->size);
        out.idr = (impl_->pkt->flags & AV_PKT_FLAG_KEY) != 0;
        got = !out.data.empty();
        av_packet_unref(impl_->pkt);
        if (got) break; /* one access unit per ring frame (PROTOCOL.md) */
    }
    return got;
}
