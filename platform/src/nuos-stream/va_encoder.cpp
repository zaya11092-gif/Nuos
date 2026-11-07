#include "va_encoder.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

extern "C" {
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_enc_hevc.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace nuos {

struct VaEncoder::Impl {
    int drm_fd = -1;
    VADisplay dpy = nullptr;
    VASurfaceID surface = VA_INVALID_ID;
    VAConfigID config = VA_INVALID_ID;
    VAContextID context = VA_INVALID_ID;
    VABufferID coded_buf = VA_INVALID_ID;
    SwsContext* sws = nullptr;
    // NV12 staging (CPU), then upload via vaPutImage
    uint8_t* nv12 = nullptr;
    int nv12_size = 0;
    unsigned frame_num = 0;
    VAImage img{};
    bool img_ok = false;
};

VaEncoder::~VaEncoder() { close(); }

bool VaEncoder::init(const EncoderConfig& cfg, std::string* err) {
    cfg_ = cfg;
    impl_ = new Impl;
    impl_->drm_fd = open("/dev/dri/renderD128", O_RDWR);
    if (impl_->drm_fd < 0) {
        if (err) *err = "cannot open /dev/dri/renderD128";
        return false;
    }
    impl_->dpy = vaGetDisplayDRM(impl_->drm_fd);
    if (!impl_->dpy) {
        if (err) *err = "vaGetDisplayDRM failed";
        return false;
    }
    int major, minor;
    VAStatus st = vaInitialize(impl_->dpy, &major, &minor);
    if (st != VA_STATUS_SUCCESS) {
        if (err) *err = std::string("vaInitialize: ") + vaErrorStr(st);
        return false;
    }
    if (!setup_surface_encoding(err)) return false;

    // Persistent NV12 image: fill via vaMapBuffer each frame, upload with
    // vaPutImage (per-frame vaDeriveImage/vaDestroyImage segfaults on iHD).
    VAImageFormat fmt{};
    fmt.fourcc = VA_FOURCC_NV12;
    fmt.byte_order = VA_LSB_FIRST;
    fmt.bits_per_pixel = 12;
    if (vaCreateImage(impl_->dpy, &fmt, cfg_.width, cfg_.height, &impl_->img)
        == VA_STATUS_SUCCESS) {
        impl_->img_ok = true;
    }
    ready_ = true;
    return true;
}

bool VaEncoder::setup_surface_encoding(std::string* err) {
    VAStatus st;

    // Look for HEVC LowPower entrypoint on any supported profile.
    VAEntrypoint want = VAEntrypointEncSliceLP;
    VAProfile profile = VAProfileHEVCMain;
    int num = vaMaxNumEntrypoints(impl_->dpy);
    std::vector<VAEntrypoint> eps(num);
    st = vaQueryConfigEntrypoints(impl_->dpy, profile, eps.data(), &num);
    if (st != VA_STATUS_SUCCESS) {
        if (err) *err = "HEVCMain not supported by VA driver";
        return false;
    }
    bool have = false;
    for (int i = 0; i < num; ++i) if (eps[i] == want) { have = true; break; }
    if (!have) {
        if (err) *err = "VAEntrypointEncSliceLP unavailable for HEVCMain";
        return false;
    }

    VAConfigAttrib attrib[2];
    attrib[0].type = VAConfigAttribRTFormat;
    attrib[1].type = VAConfigAttribRateControl;
    vaGetConfigAttributes(impl_->dpy, profile, want, attrib, 2);
    if (!(attrib[0].value & VA_RT_FORMAT_YUV420)) {
        if (err) *err = "NV12 RT format unsupported";
        return false;
    }
    attrib[1].value = VA_RC_CBR;

    st = vaCreateConfig(impl_->dpy, profile, want, attrib, 2, &impl_->config);
    if (st != VA_STATUS_SUCCESS) {
        if (err) *err = std::string("vaCreateConfig: ") + vaErrorStr(st);
        return false;
    }

    st = vaCreateSurfaces(impl_->dpy, VA_RT_FORMAT_YUV420,
                          cfg_.width, cfg_.height, &impl_->surface, 1,
                          nullptr, 0);
    if (st != VA_STATUS_SUCCESS) {
        if (err) *err = std::string("vaCreateSurfaces: ") + vaErrorStr(st);
        return false;
    }

    st = vaCreateContext(impl_->dpy, impl_->config, cfg_.width, cfg_.height,
                         VA_PROGRESSIVE, &impl_->surface, 1, &impl_->context);
    if (st != VA_STATUS_SUCCESS) {
        if (err) *err = std::string("vaCreateContext: ") + vaErrorStr(st);
        return false;
    }

    // coded buffer, generously sized for the bitrate
    size_t cb = cfg_.bitrate_kbps * 1000 / 8 / cfg_.fps * 4 + 65536;
    st = vaCreateBuffer(impl_->dpy, impl_->context, VAEncCodedBufferType,
                        cb, 1, nullptr, &impl_->coded_buf);
    if (st != VA_STATUS_SUCCESS) {
        if (err) *err = std::string("vaCreateBuffer(coded): ") + vaErrorStr(st);
        return false;
    }

    // CPU NV12 staging + swscale BGRA -> NV12
    impl_->nv12_size = av_image_get_buffer_size(AV_PIX_FMT_NV12,
                                                cfg_.width, cfg_.height, 1);
    impl_->nv12 = (uint8_t*)av_malloc(impl_->nv12_size);
    impl_->sws = sws_getContext(cfg_.width, cfg_.height, AV_PIX_FMT_BGRA,
                                cfg_.width, cfg_.height, AV_PIX_FMT_NV12,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl_->sws || !impl_->nv12) {
        if (err) *err = "swscale setup failed";
        return false;
    }
    return true;
}

void VaEncoder::close() {
    ready_ = false;
    if (!impl_) return;
    if (impl_->coded_buf != VA_INVALID_ID)
        vaDestroyBuffer(impl_->dpy, impl_->coded_buf);
    if (impl_->context != VA_INVALID_ID)
        vaDestroyContext(impl_->dpy, impl_->context);
    if (impl_->surface != VA_INVALID_ID)
        vaDestroySurfaces(impl_->dpy, &impl_->surface, 1);
    if (impl_->config != VA_INVALID_ID)
        vaDestroyConfig(impl_->dpy, impl_->config);
    if (impl_->img_ok) vaDestroyImage(impl_->dpy, impl_->img.buf);
    if (impl_->dpy) vaTerminate(impl_->dpy);
    if (impl_->sws) sws_freeContext(impl_->sws);
    if (impl_->nv12) av_free(impl_->nv12);
    if (impl_->drm_fd >= 0) ::close(impl_->drm_fd);
    delete impl_;
    impl_ = nullptr;
}

void VaEncoder::set_bitrate_kbps(uint32_t kbps) { cfg_.bitrate_kbps = kbps; }
void VaEncoder::set_fps(uint32_t fps) { cfg_.fps = fps; }
void VaEncoder::force_idr() { want_idr_ = true; }

bool VaEncoder::encode_bgra(const uint8_t* bgra, int stride, EncodedFrame& out) {
    if (!ready_) return false;
    uint8_t* dst_slice[2] = { impl_->nv12,
                              impl_->nv12 + (size_t)cfg_.width * cfg_.height };
    const uint8_t* src_slice[1] = { bgra };
    int src_stride[1] = { stride };
    int dst_stride[2] = { cfg_.width, cfg_.width };
    sws_scale(impl_->sws, src_slice, src_stride, 0, cfg_.height,
              dst_slice, dst_stride);
    return encode_nv12(impl_->nv12,
                       impl_->nv12 + (size_t)cfg_.width * cfg_.height,
                       cfg_.width, cfg_.width, out);
}

bool VaEncoder::encode_nv12(const uint8_t* y, const uint8_t* uv, int y_stride,
                            int uv_stride, EncodedFrame& out) {
    VAStatus st;
    VAEncPictureParameterBufferHEVC pic{};
    pic.slice_pic_parameter_set_id = 0;
    pic.nal_unit_type = want_idr_ ? 19 : 1; /* IDR_W_RADL / TRAIL_R */
    pic.pic_fields.bits.idr_pic_flag = want_idr_ ? 1 : 0;
    pic.pic_fields.bits.coding_type = want_idr_ ? 1 : 2; // I / P
    pic.pic_fields.bits.reference_pic_flag = 1;
    pic.last_picture = 0;
    pic.collocated_ref_pic_index = 0xFF;
    pic.decoded_curr_pic.picture_id = impl_->surface;
    pic.coded_buf = impl_->coded_buf;
    for (int i = 0; i < 15; ++i) pic.reference_frames[i].picture_id = VA_INVALID_ID;

    VABufferID pic_buf;
    st = vaCreateBuffer(impl_->dpy, impl_->context,
                        VAEncPictureParameterBufferType,
                        sizeof pic, 1, &pic, &pic_buf);
    if (st != VA_STATUS_SUCCESS) return false;

    VAEncSliceParameterBufferHEVC slice{};
    slice.slice_type = want_idr_ ? 2 : 1;
    slice.num_ctu_in_slice =
        ((cfg_.width + 63) / 64) * ((cfg_.height + 63) / 64);
    VABufferID slice_buf;
    st = vaCreateBuffer(impl_->dpy, impl_->context,
                        VAEncSliceParameterBufferType,
                        sizeof slice, 1, &slice, &slice_buf);
    if (st != VA_STATUS_SUCCESS) { vaDestroyBuffer(impl_->dpy, pic_buf); return false; }

    VABufferID hrd_buf = VA_INVALID_ID;
    {
        VAEncMiscParameterBuffer misc{};
        misc.type = VAEncMiscParameterTypeHRD;
        auto* hrd = (VAEncMiscParameterHRD*)misc.data;
        hrd->initial_buffer_fullness = cfg_.bitrate_kbps * 1000 / 2;
        hrd->buffer_size = cfg_.bitrate_kbps * 1000;
        vaCreateBuffer(impl_->dpy, impl_->context, VAEncMiscParameterBufferType,
                       sizeof(misc) + sizeof(*hrd), 1, &misc, &hrd_buf);
    }

    // upload pixels into the surface
    if (!impl_->img_ok) return false;
    void* pmem = nullptr;
    if (vaMapBuffer(impl_->dpy, impl_->img.buf, &pmem) == VA_STATUS_SUCCESS
        && pmem) {
        uint8_t* base = (uint8_t*)pmem;
        for (int r = 0; r < cfg_.height; ++r)
            memcpy(base + (size_t)r * impl_->img.pitches[0],
                   y + (size_t)r * y_stride, cfg_.width);
        uint8_t* cbase = base + (size_t)impl_->img.offsets[1];
        for (int r = 0; r < cfg_.height / 2; ++r)
            memcpy(cbase + (size_t)r * impl_->img.pitches[1],
                   uv + (size_t)r * uv_stride, cfg_.width);
        vaUnmapBuffer(impl_->dpy, impl_->img.buf);
    }
    vaPutImage(impl_->dpy, impl_->surface, impl_->img.buf,
               0, 0, (unsigned)cfg_.width, (unsigned)cfg_.height,
               0, 0, (unsigned)cfg_.width, (unsigned)cfg_.height);

    st = vaBeginPicture(impl_->dpy, impl_->context, impl_->surface);
    if (st == VA_STATUS_SUCCESS) {
        vaRenderPicture(impl_->dpy, impl_->context, &pic_buf, 1);
        vaRenderPicture(impl_->dpy, impl_->context, &slice_buf, 1);
        if (hrd_buf != VA_INVALID_ID)
            vaRenderPicture(impl_->dpy, impl_->context, &hrd_buf, 1);
        vaEndPicture(impl_->dpy, impl_->context);
    }

    // read coded data
    VACodedBufferSegment* seg = nullptr;
    bool got = false;
    if (vaMapBuffer(impl_->dpy, impl_->coded_buf, (void**)&seg) == VA_STATUS_SUCCESS
        && seg && seg->size > 0 && seg->buf) {
        out.data.assign((uint8_t*)seg->buf, (uint8_t*)seg->buf + seg->size);
        out.idr = want_idr_;
        got = true;
        vaUnmapBuffer(impl_->dpy, impl_->coded_buf);
    } else if (seg) {
        vaUnmapBuffer(impl_->dpy, impl_->coded_buf);
    }

    vaDestroyBuffer(impl_->dpy, pic_buf);
    vaDestroyBuffer(impl_->dpy, slice_buf);
    if (hrd_buf != VA_INVALID_ID) vaDestroyBuffer(impl_->dpy, hrd_buf);

    want_idr_ = false;
    ++impl_->frame_num;
    return got;
}

} // namespace nuos
