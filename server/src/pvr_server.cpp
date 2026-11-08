// NuOS v2 PVR server — speaks the PhoneVR link protocol to the phone client.
//
// Protocol (clean-room from the documented PhoneVR wire format):
//   phone: UDP broadcast {'p','v','r',0x00, uint32 version} :33333 (~2 Hz)
//   board: TCP connect to <phone_ip>:33333, sends
//          "pvr" + msgType + uint16 payloadLen (LE):
//            PAIR_ACCEPT(3) [no payload]
//            HEADER_NALS(5) [Annex-B VPS/SPS/PPS]
//   video: board listens TCP :15243; phone connects; board writes per frame
//          64-byte header:
//            i64 pts_us | f32 quat[4] (w,x,y,z) | u32 nal_size
//            f32 fps[5] | f32 delay_ms[2] | i64 wall_ms
//          followed by nal_size bytes of one HEVC Annex-B AU (no param sets)
//   pose:  phone -> board UDP :51423, 36 B: f32 quat[4] | f32 accel[3] | i64 ns
//   ctrl:  on the control TCP, phone->board TLVs (NuOS v2 extension):
//            0x10 LIST_WINDOWS_REQ      -> ADDITIONAL_DATA(4) JSON list
//            0x11 LAUNCH  [len][cmd]    -> spawns command, ACKs ADDITIONAL_DATA
//            0x12 STREAM_WINDOW [u32]   -> capture+stream that window id
//
// Build: see CMakeLists.txt. Deps: libavcodec/libavutil/libswscale, X11.
#include "win_capture.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <poll.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

// ---------------------------------------------------------------- constants
static constexpr uint16_t kPairPort = 33333;      // UDP announce; TCP to phone
static constexpr uint16_t kVideoPort = 15243;     // TCP, phone connects to us
static constexpr uint16_t kPosePort = 51423;      // UDP, phone -> us
static constexpr uint32_t kMinClientVersion = 0;  // accept any for now

static constexpr uint8_t MSG_PAIR_HMD = 0, MSG_PAIR_ACCEPT = 3,
                         MSG_ADDITIONAL_DATA = 4, MSG_HEADER_NALS = 5;
static constexpr uint8_t TLV_LIST_WINDOWS_REQ = 0x10, TLV_LAUNCH = 0x11,
                         TLV_STREAM_WINDOW = 0x12;

// ---------------------------------------------------------------- globals
static std::atomic<bool> g_stop{false};
static std::mutex g_phone_mtx;          // guards phone socket + header state
static int g_phone_ctl = -1;            // TCP socket to phone control
static std::string g_phone_ip;
static std::vector<uint8_t> g_header_nals;  // Annex-B VPS/SPS/PPS
static std::atomic<bool> g_have_headers{false};
static std::atomic<uint32_t> g_stream_window{0};  // X11 window id to stream
static float g_pose_quat[4] = {0, 0, 0, 1};       // latest phone pose
static std::atomic<uint64_t> g_pose_ns{0};

static uint64_t mono_us() {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
}

// ---------------------------------------------------------------- tiny log
#define LOGI(...) do { fprintf(stderr, "[pvr] "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

// ---------------------------------------------------------------- TCP framing
static bool send_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    while (n) {
        ssize_t r = send(fd, p, n, MSG_NOSIGNAL);
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

static bool send_msg(int fd, uint8_t type, const void* payload, uint16_t len) {
    uint8_t hdr[6] = {'p', 'v', 'r', type, (uint8_t)(len & 0xff),
                      (uint8_t)(len >> 8)};
    if (!send_all(fd, hdr, 6)) return false;
    if (len) return send_all(fd, payload, len);
    return true;
}

// read one TLV from the control stream (NuOS v2 extension, byte-framed)
// returns false on socket error
static bool read_tlv(int fd, uint8_t& type, std::vector<uint8_t>& payload) {
    uint8_t h[5];
    size_t got = 0;
    while (got < sizeof h) {
        ssize_t r = recv(fd, h + got, sizeof h - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    type = h[0];
    uint32_t len = (uint32_t)h[1] | ((uint32_t)h[2] << 8) |
                   ((uint32_t)h[3] << 16) | ((uint32_t)h[4] << 24);
    if (len > 1u << 20) return false;
    payload.assign(len, 0);
    got = 0;
    while (got < len) {
        ssize_t r = recv(fd, payload.data() + got, len - got, 0);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

// ---------------------------------------------------------------- encoder
// libavcodec hevc_vaapi — the proven pattern on this board (legacy notes:
// extern-C ffmpeg 8 headers, pooled dst via av_hwframe_get_buffer).
class HevcEncoder {
public:
    ~HevcEncoder() { close(); }

    bool init(int w, int h, uint32_t bitrate_kbps, std::string* err) {
        w_ = w; h_ = h;
        nv12_size_ = av_image_get_buffer_size(AV_PIX_FMT_NV12, w, h, 1);
        nv12_ = (uint8_t*)av_malloc((size_t)nv12_size_);
        sws_ = sws_getContext(w, h, AV_PIX_FMT_BGRA, w, h, AV_PIX_FMT_NV12,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_ || !nv12_) { if (err) *err = "swscale init failed"; return false; }

        const AVCodec* codec = avcodec_find_encoder_by_name("hevc_vaapi");
        if (!codec) { if (err) *err = "hevc_vaapi not found"; return false; }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) { if (err) *err = "alloc ctx failed"; return false; }
        ctx_->width = w; ctx_->height = h;
        ctx_->pix_fmt = AV_PIX_FMT_VAAPI;
        ctx_->time_base = AVRational{1, 60};
        ctx_->framerate = AVRational{60, 1};
        ctx_->bit_rate = (int64_t)bitrate_kbps * 1000;
        ctx_->rc_max_rate = ctx_->bit_rate;
        ctx_->rc_buffer_size = (int)ctx_->bit_rate;
        ctx_->gop_size = 120;
        ctx_->max_b_frames = 0;
        av_opt_set(ctx_->priv_data, "rc_mode", "CBR", 0);

        if (av_hwdevice_ctx_create(&hw_dev_, AV_HWDEVICE_TYPE_VAAPI,
                                   "/dev/dri/renderD128", nullptr, 0) < 0) {
            if (err) *err = "vaapi device open failed";
            return false;
        }
        ctx_->hw_device_ctx = av_buffer_ref(hw_dev_);
        hw_frames_ = av_hwframe_ctx_alloc(hw_dev_);
        auto* fc = (AVHWFramesContext*)hw_frames_->data;
        fc->format = AV_PIX_FMT_VAAPI;
        fc->sw_format = AV_PIX_FMT_NV12;
        fc->width = w; fc->height = h;
        fc->initial_pool_size = 20;
        if (av_hwframe_ctx_init(hw_frames_) < 0) {
            if (err) *err = "hw frames init failed";
            return false;
        }
        ctx_->hw_frames_ctx = av_buffer_ref(hw_frames_);
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            if (err) *err = "avcodec_open2(hevc_vaapi) failed";
            return false;
        }
        sw_ = av_frame_alloc();
        sw_->format = AV_PIX_FMT_NV12;
        sw_->width = w; sw_->height = h;
        hw_ = av_frame_alloc();
        pkt_ = av_packet_alloc();

        // Encode one gray frame NOW so VPS/SPS/PPS exist before any phone
        // pairs (phones expect HEADER_NALS right after PAIR_ACCEPT).
        std::vector<uint8_t> bgra((size_t)w_ * h_ * 4, 0x80);
        std::vector<uint8_t> au;
        for (int i = 0; i < 12 && au.empty(); ++i) {
            encode_bgra(bgra.data(), w_ * 4, au);
        }
        if (!au.empty()) {
            LOGI("warm-up encoded, header NALs cached (%zu bytes)",
                 header_nals_.size());
        } else {
            LOGI("warm-up produced no bitstream (will retry on stream)");
        }
        frame_ = 0;
        frame_ = 0;
        LOGI("encoder ready %dx%d @ %u kbps (hevc_vaapi)", w, h, bitrate_kbps);
        return true;
    }

    // Encode one BGRA frame. Returns true and fills `au` (Annex-B) on success.
    // On the FIRST successful packet, strips VPS/SPS/PPS into *header_nals
    // and the AU becomes the remaining NALs.
    bool encode_bgra(const uint8_t* bgra, int stride, std::vector<uint8_t>& au) {
        if (!ready()) return false;
        uint8_t* dst[2] = { nv12_, nv12_ + (size_t)w_ * h_ };
        const uint8_t* src[1] = { bgra };
        int sstr[1] = { stride };
        int dstr[2] = { w_, w_ };
        sws_scale(sws_, src, sstr, 0, h_, dst, dstr);

        if (av_image_fill_arrays(sw_->data, sw_->linesize, nv12_,
                                 AV_PIX_FMT_NV12, w_, h_, 1) < 0) {
            LOGI("encode: fill_arrays failed");
            return false;
        }
        av_frame_unref(hw_);
        hw_->format = AV_PIX_FMT_VAAPI;
        hw_->width = w_; hw_->height = h_;
        if (av_hwframe_get_buffer(hw_frames_, hw_, 0) < 0) {
            LOGI("encode: get_buffer failed");
            return false;
        }
        int tr = av_hwframe_transfer_data(hw_, sw_, 0);
        if (tr < 0) {
            LOGI("encode: transfer failed %d", tr);
            return false;
        }
        hw_->pts = (int64_t)frame_++;
        hw_->pts = (int64_t)frame_++;

        if (avcodec_send_frame(ctx_, hw_) < 0) {
            LOGI("encode: send_frame failed");
            return false;
        }
        bool got = false;
        while (avcodec_receive_packet(ctx_, pkt_) == 0) {
            if (!got && !header_sent_) {
                split_header_and_au(pkt_->data, pkt_->size, au);
                header_sent_ = true;
                {
                    std::lock_guard<std::mutex> lk(g_phone_mtx);
                    g_header_nals = header_nals_;
                }
                g_have_headers = true;
                LOGI("header NALs cached (%zu bytes)", header_nals_.size());
            } else {
                au.assign(pkt_->data, pkt_->data + pkt_->size);
            }
            got = !au.empty();
            av_packet_unref(pkt_);
            if (got) break;
        }
        if (!got) LOGI("encode: no packet from driver");
        return got;
    }

    const std::vector<uint8_t>& header_nals() const { return header_nals_; }

    void close() {
        if (pkt_) av_packet_free(&pkt_);
        if (sw_) av_frame_free(&sw_);
        if (hw_) av_frame_free(&hw_);
        if (ctx_) avcodec_free_context(&ctx_);
        if (hw_frames_) av_buffer_unref(&hw_frames_);
        if (hw_dev_) av_buffer_unref(&hw_dev_);
        if (sws_) sws_freeContext(sws_);
        if (nv12_) av_free(nv12_);
        nv12_ = nullptr;
    }

private:
    bool ready() const { return ctx_ != nullptr; }

    // Split an AU into leading parameter-set NALs (32/33/34) and the rest.
    void split_header_and_au(const uint8_t* d, size_t n, std::vector<uint8_t>& au) {
        size_t i = 0;
        size_t au_start = n;
        while (i + 4 <= n) {
            if (!(d[i] == 0 && d[i+1] == 0 && d[i+2] == 0 && d[i+3] == 1)) break;
            size_t nal = i + 4;
            uint8_t type = (d[nal] >> 1) & 0x3f;
            // find next start code
            size_t j = nal + 1;
            while (j + 3 <= n &&
                   !(d[j] == 0 && d[j+1] == 0 && d[j+2] == 0 && d[j+3] == 1))
                ++j;
            bool is_param = (type == 32 || type == 33 || type == 34);
            if (!is_param) { au_start = i; break; }
            header_nals_.insert(header_nals_.end(), d + i, d + j);
            i = j;
            if (j >= n) { au_start = n; break; }
        }
        if (au_start < n) au.assign(d + au_start, d + n);
    }

    int w_ = 0, h_ = 0;
    SwsContext* sws_ = nullptr;
    uint8_t* nv12_ = nullptr;
    int nv12_size_ = 0;
    AVBufferRef* hw_dev_ = nullptr;
    AVBufferRef* hw_frames_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* sw_ = nullptr;
    AVFrame* hw_ = nullptr;
    AVPacket* pkt_ = nullptr;
    uint64_t frame_ = 0;
    bool header_sent_ = false;
    std::vector<uint8_t> header_nals_;
};

// ---------------------------------------------------------------- control
static void handle_tlv(int fd, uint8_t type, const std::vector<uint8_t>& pl) {
    switch (type) {
    case TLV_LIST_WINDOWS_REQ: {
        std::string json = nuos_list_windows_json();
        send_msg(fd, MSG_ADDITIONAL_DATA, json.data(),
                 (uint16_t)std::min<size_t>(json.size(), 0xffff));
        break;
    }
    case TLV_LAUNCH: {
        std::string cmd((const char*)pl.data(), pl.size());
        LOGI("launch request: %s", cmd.c_str());
        std::string full = "DISPLAY=:0 nohup " + cmd +
                           " >/dev/null 2>&1 &";
        int rc = system(full.c_str());
        (void)rc;
        const char* ok = "{\"launched\":true}";
        send_msg(fd, MSG_ADDITIONAL_DATA, ok, (uint16_t)strlen(ok));
        break;
    }
    case TLV_STREAM_WINDOW: {
        uint32_t id = pl.size() >= 4
            ? (uint32_t)pl[0] | ((uint32_t)pl[1] << 8) |
              ((uint32_t)pl[2] << 16) | ((uint32_t)pl[3] << 24)
            : 0;
        g_stream_window = id;
        LOGI("stream window -> %u", id);
        break;
    }
    default:
        LOGI("unknown TLV 0x%02x (%zu bytes)", type, pl.size());
    }
}

static void control_reader(int fd) {
    for (;;) {
        uint8_t type; std::vector<uint8_t> pl;
        if (!read_tlv(fd, type, pl)) break;
        handle_tlv(fd, type, pl);
    }
    LOGI("control reader ended");
}

// ---------------------------------------------------------------- pairing
static void pair_with_phone(const std::string& ip) {
    std::lock_guard<std::mutex> lk(g_phone_mtx);
    // Same phone announcing again: keep the live connection, re-pair only if
    // it is dead (phone restarted) or the announcing IP changed.
    if (g_phone_ctl >= 0 && g_phone_ip == ip) {
        pollfd p{g_phone_ctl, POLLIN | POLLRDHUP, 0};
        int r = poll(&p, 1, 0);
        if (r >= 0 && !(p.revents & (POLLRDHUP | POLLERR | POLLHUP)))
            return;  // still alive
        LOGI("control socket dead, re-pairing");
    }
    if (g_phone_ctl >= 0) {
        close(g_phone_ctl);
        g_phone_ctl = -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPairPort);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    if (connect(fd, (sockaddr*)&a, sizeof a) < 0) {
        LOGI("TCP connect to %s:%u failed: %s", ip.c_str(), kPairPort,
             strerror(errno));
        close(fd);
        return;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (!send_msg(fd, MSG_PAIR_ACCEPT, nullptr, 0)) {
        close(fd); return;
    }
    g_phone_ctl = fd;
    g_phone_ip = ip;
    LOGI("paired with %s (control)", ip.c_str());
    if (g_have_headers) {
        // NOTE: g_phone_mtx is already held by this function.
        send_msg(fd, MSG_HEADER_NALS, g_header_nals.data(),
                 (uint16_t)std::min<size_t>(g_header_nals.size(), 0xffff));
        LOGI("sent HEADER_NALS (%zu bytes)", g_header_nals.size());
    }
    std::thread(control_reader, fd).detach();
}

static void* announce_listener(void*) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPairPort);
    a.sin_addr.s_addr = INADDR_ANY;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (sockaddr*)&a, sizeof a) < 0) {
        LOGI("announce bind failed: %s", strerror(errno));
        return nullptr;
    }
    LOGI("listening for phone announces on UDP %u", kPairPort);
    char buf[64];
    sockaddr_in src{};
    socklen_t sl = sizeof src;
    while (!g_stop) {
        ssize_t r = recvfrom(fd, buf, sizeof buf, 0, (sockaddr*)&src, &sl);
        if (r < 8) continue;
        if (memcmp(buf, "pvr", 3) != 0 || buf[3] != MSG_PAIR_HMD) continue;
        uint32_t ver;
        memcpy(&ver, buf + 4, 4);
        if (ver < kMinClientVersion) continue;
        char ip[64];
        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof ip);
        pair_with_phone(ip);
    }
    return nullptr;
}

static void* pose_listener(void*) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kPosePort);
    a.sin_addr.s_addr = INADDR_ANY;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    bind(fd, (sockaddr*)&a, sizeof a);
    LOGI("listening for pose on UDP %u", kPosePort);
    uint8_t buf[64];
    while (!g_stop) {
        ssize_t r = recvfrom(fd, buf, sizeof buf, 0, nullptr, nullptr);
        if (r < 28) continue;
        float q[4], acc[3];
        memcpy(q, buf, 16);
        memcpy(acc, buf + 16, 12);
        for (int i = 0; i < 4; ++i) g_pose_quat[i] = q[i];
        g_pose_ns = mono_us();
    }
    return nullptr;
}

// ---------------------------------------------------------------- video out
static void* video_server(void* arg) {
    HevcEncoder* enc = (HevcEncoder*)arg;
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(kVideoPort);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(lfd, (sockaddr*)&a, sizeof a) < 0 || listen(lfd, 1) < 0) {
        LOGI("video listen failed: %s", strerror(errno));
        return nullptr;
    }
    LOGI("video server on TCP %u", kVideoPort);
    while (!g_stop) {
        int vfd = accept(lfd, nullptr, nullptr);
        if (vfd < 0) continue;
        int nd = 1;
        setsockopt(vfd, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof nd);
        LOGI("video client connected");
        uint64_t frames = 0, last_log = mono_us();
        while (!g_stop) {
            uint32_t win = g_stream_window;
            if (!win) { usleep(16000); continue; }
            WinImage img;
            if (!nuos_capture_window(win, img)) { usleep(100000); continue; }
            std::vector<uint8_t> au;
            if (!enc->encode_bgra(img.data.data(), (int)img.stride, au) ||
                au.empty()) {
                usleep(16000);
                continue;
            }
            // 64-byte header: pts i64, quat f32x4, nal u32, fps f32x5,
            //                 delays f32x2, wall i64
            uint8_t hdr[64];
            memset(hdr, 0, sizeof hdr);
            uint64_t pts = mono_us();
            memcpy(hdr, &pts, 8);
            memcpy(hdr + 8, g_pose_quat, 16);
            uint32_t nalsz = (uint32_t)au.size();
            memcpy(hdr + 24, &nalsz, 4);
            uint64_t wall = (uint64_t)time(nullptr) * 1000ull;
            memcpy(hdr + 56, &wall, 8);
            if (!send_all(vfd, hdr, 64) || !send_all(vfd, au.data(), au.size())) {
                LOGI("video client gone");
                break;
            }
            if (++frames % 120 == 0) {
                uint64_t now = mono_us();
                LOGI("streamed %llu AUs (%.1f fps)", (unsigned long long)frames,
                     120000.0 / (double)(now - last_log));
                last_log = now;
            }
        }
        close(vfd);
    }
    return nullptr;
}

// ---------------------------------------------------------------- main
int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    int capture_w = 1280, capture_h = 720;
    uint32_t bitrate = 15000;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--width") && i + 1 < argc) capture_w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) capture_h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) bitrate = (uint32_t)atoi(argv[++i]);
    }
    LOGI("nuospvr-server starting (capture %dx%d, %u kbps)", capture_w, capture_h,
         bitrate);

    if (!nuos_x11_init()) {
        LOGI("cannot open X11 display (is the desktop session up?)");
        return 1;
    }

    HevcEncoder enc;
    std::string err;
    if (!enc.init(capture_w, capture_h, bitrate, &err)) {
        LOGI("encoder init failed: %s", err.c_str());
        return 1;
    }

    pthread_t t1, t2, t3;
    pthread_create(&t1, nullptr, announce_listener, nullptr);
    pthread_create(&t2, nullptr, pose_listener, nullptr);
    pthread_create(&t3, nullptr, video_server, &enc);

    while (!g_stop) sleep(1);
    return 0;
}
