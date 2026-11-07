// nuos-stream — reads /dev/shm/nuos_frames, encodes BGRA->NV12->VAAPI HEVC
// low-power, fragments with nuos_video_t headers + FEC, streams UDP to the
// phone (IP learned from the :48012 TCP control peer).
#include <getopt.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sched.h>
#include <pthread.h>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <thread>

#include <sys/socket.h>
#include <netinet/in.h>

#include "nuos_proto.h"
#include "nuos_shm.h"
#include "ring_reader.hpp"
#include "va_encoder.hpp"
#include "packetizer.hpp"
#include "udp_send.hpp"
#include "control_server.hpp"

using namespace nuos;

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

static uint64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// Render pose = latest fused head pose from /dev/shm/nuos_pose (seqlock).
static bool read_render_pose(float pos[3], float quat[4], uint64_t& t_ns,
                             uint8_t& tracking) {
    int fd = shm_open(NUOS_SHM_POSE_NAME, O_RDONLY, 0);
    if (fd < 0) return false;
    void* m = mmap(nullptr, sizeof(nuos_pose_shm_t), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return false;
    auto* p = (nuos_pose_shm_t*)m;
    bool ok = false;
    for (int tries = 0; tries < 8; ++tries) {
        uint32_t s1 = nuos_seqlock_begin_read(&p->seq);
        if (p->magic != NUOS_SHM_MAGIC_POSE) break;
        memcpy(pos, p->pos, sizeof(float) * 3);
        memcpy(quat, p->quat, sizeof(float) * 4);
        t_ns = p->t_mono_ns;
        tracking = p->tracking;
        ok = nuos_seqlock_end_read(&p->seq, s1) == 0;
        if (ok) break;
    }
    munmap(m, sizeof(nuos_pose_shm_t));
    return ok;
}

int main(int argc, char** argv) {
    int core = -1;
    bool rt = false;
    static option longopts[] = {
        {"core", required_argument, nullptr, 'c'},
        {"rt",   no_argument,       nullptr, 't'},
        {nullptr, 0, nullptr, 0},
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "c:t", longopts, nullptr)) != -1) {
        if (ch == 'c') core = atoi(optarg);
        if (ch == 't') rt = true;
    }
    (void)core; (void)rt; // same pinning helpers as tracker if desired

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    if (core >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        pthread_setaffinity_np(pthread_self(), sizeof set, &set);
    }
    if (rt) {
        sched_param sp{};
        sp.sched_priority = 15;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }

    RingReader ring;
    bool ring_ok = ring.open(/*create_if_missing=*/false);
    if (!ring_ok) {
        fprintf(stderr, "nuos-stream: %s not open yet (waiting for xr-home)\n",
                NUOS_SHM_RING_NAME);
    }

    VaEncoder enc;
    EncoderConfig ecfg;
    if (ring_ok) {
        // Encode the ring frame AS-IS (side-by-side stereo, whatever dims
        // the home renders) — the phone displays the decoded frame full
        // screen. Session width/height from the phone is ignored for dims.
        if (auto* h = ring.header()) {
            if (h->width > 0 && h->height > 0) {
                ecfg.width = (int)h->width;
                ecfg.height = (int)h->height;
                fprintf(stderr, "nuos-stream: encoding ring %dx%d\n", ecfg.width, ecfg.height);
            }
        }
    }
    std::string err;
    if (!enc.init(ecfg, &err)) {
        fprintf(stderr, "nuos-stream: VAAPI encoder unavailable: %s\n", err.c_str());
        fprintf(stderr, "  (board needs i965/iHD driver; stream will idle)\n");
    }

    Packetizer pkt;
    UdpSend udp;
    udp.open();
    udp.set_target("127.0.0.1", NUOS_PORT_VIDEO); // default until control peer

    ControlServer ctl;
    if (!ctl.start()) {
        fprintf(stderr, "nuos-stream: cannot listen on TCP %d\n", NUOS_PORT_CONTROL);
        return 1;
    }
    ctl.set_on_session([&](const SessionParams& s) {
        enc.set_bitrate_kbps(s.bitrate_kbps);
        enc.set_fps(s.fps);
        if (ctl.peer_connected()) udp.set_target(ctl.peer_ip(), NUOS_PORT_VIDEO);
        enc.force_idr();
    });
    ctl.set_on_stop([&]() {
        udp.set_target("127.0.0.1", NUOS_PORT_VIDEO); // stop streaming to peer
    });

    uint32_t frame_seq = 0;
    uint64_t last_slot_ns = 0;
    uint32_t loss_ppm_est = 0;

    fprintf(stderr, "nuos-stream: running\n");
    const auto tick = std::chrono::microseconds(16667); // ~60 fps
    auto next = std::chrono::steady_clock::now();

    while (!g_stop) {
        next += tick;
        ctl.poll(); // non-blocking accept / frame read

        if (!enc.ok() || !ring_ok) { std::this_thread::sleep_until(next); continue; }

        RgbaView view{nullptr, 0, 0, 0};
        uint64_t slot_ns = 0;
        if (!ring.latest(view, slot_ns) || slot_ns == last_slot_ns) {
            std::this_thread::sleep_until(next);
            continue; // no new frame
        }
        last_slot_ns = slot_ns;

        EncodedFrame ef;
        if (!enc.encode_bgra(view.data, (int)view.stride, ef) || ef.data.empty()) {
            static int nfails = 0;
            if ((nfails++ & 31) == 0)
                fprintf(stderr, "nuos-stream: encode_bgra failed (frame %d)
", nfails);
            std::this_thread::sleep_until(next);
            continue;
        }
        if (nfails > 0 && ef.data.size() > 0) {
            fprintf(stderr, "nuos-stream: encode RECOVERED, au=%zu bytes
", ef.data.size());
            nfails = 0;
        }

        float rpos[3] = {0,0,0}, rquat[4] = {1,0,0,0};
        uint64_t pt_ns = mono_ns();
        uint8_t tracking = 0;
        read_render_pose(rpos, rquat, pt_ns, tracking);

        // Env-gated bitstream dump (first 30 AUs) for offline decode.
        if (const char* dump_path = getenv("NUOS_DUMP")) {
            static FILE* df = fopen(dump_path, "wb");
            static unsigned dumped = 0;
            if (df && dumped < 30) {
                fwrite(ef.data.data(), 1, ef.data.size(), df);
                if (++dumped == 30) { fclose(df); df = nullptr;
                    fprintf(stderr, "nuos-stream: dumped 30 AUs
"); }
            }
        }

        std::vector<VideoPacket> frags;
        std::vector<FecPacket> fec;
        pkt.frame_packets(mono_ns(), frame_seq, ef.data.data(), ef.data.size(),
                          ef.idr, rpos, rquat, frags, fec);

        // send back-to-back, no pacing within a frame (PROTOCOL.md rule 3)
        for (auto& f : frags) udp.send(f.bytes.data(), f.bytes.size());
        for (auto& f : fec)   udp.send(f.bytes.data(), f.bytes.size());

        ++frame_seq;
        std::this_thread::sleep_until(next);
    }

    ctl.stop();
    enc.close();
    fprintf(stderr, "nuos-stream: stopped\n");
    return 0;
}
