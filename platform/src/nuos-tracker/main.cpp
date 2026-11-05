// nuos-tracker — fuses ARKit head pose (UDP 48011) with Kiyo mono VO and
// publishes fused state (shm seqlock + localhost UDP), ~90 Hz.
//
// Kiyo visual odometry is MONOCULAR: translation is scale-ambiguous. VO
// contributes (a) yaw-drift correction to ARKit orientation and (b) a unit
// translation direction used only for motion gating — never metric position.
// Without a Kiyo (no camera / no features), the tracker degrades to ARKit
// passthrough transparently.
#include <getopt.h>
#include <sched.h>
#include <pthread.h>
#include <csignal>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <thread>

#include "nuos_proto.h"
#include "v4l2_source.hpp"
#include "vo_klt.hpp"
#include "fusion.hpp"
#include "telem_recv.hpp"
#include "state_pub.hpp"

using namespace nuos;

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int) { g_stop = 1; }

static void apply_affinity(int core) {
    if (core < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof set, &set) != 0)
        fprintf(stderr, "nuos-tracker: affinity to core %d failed\n", core);
}

static void apply_rt() {
    sched_param sp{};
    sp.sched_priority = 20;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "nuos-tracker: SCHED_FIFO not granted (run as root or setcap)\n");
}


static uint64_t mono_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(int argc, char** argv) {
    std::string device = "/dev/video0";
    bool replay = false;
    std::string shm_path = "/dev/shm"; // informational; shm name from nuos_shm.h
    int core = -1;
    bool rt = false;

    static option longopts[] = {
        {"device",  required_argument, nullptr, 'd'},
        {"replay",  required_argument, nullptr, 'r'},
        {"out-shm", required_argument, nullptr, 's'},
        {"core",    required_argument, nullptr, 'c'},
        {"rt",      no_argument,       nullptr, 't'},
        {nullptr, 0, nullptr, 0},
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "d:r:s:c:t", longopts, nullptr)) != -1) {
        switch (ch) {
        case 'd': device = optarg; replay = false; break;
        case 'r': device = optarg; replay = true; break;
        case 's': shm_path = optarg; break; // default per nuos_shm.h; kept for CLI parity
        case 'c': core = atoi(optarg); break;
        case 't': rt = true; break;
        default: return 2;
        }
    }
    (void)shm_path; // fixed contract: /dev/shm/nuos_pose via NUOS_SHM_POSE_NAME

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    apply_affinity(core);
    if (rt) apply_rt();

    SourceConfig scfg;
    scfg.device_or_file = device;
    scfg.replay = replay;

    CameraSource cam;
    bool kiyo = cam.open(scfg);
    if (!kiyo)
        fprintf(stderr, "nuos-tracker: Kiyo source '%s' unavailable — ARKit passthrough mode\n",
                device.c_str());
    VoKlt vo;
    Fusion fusion;
    fusion.set_kiyo_alive(kiyo);

    TelemRecv telem;
    if (!telem.start()) {
        fprintf(stderr, "nuos-tracker: cannot bind UDP %d\n", NUOS_PORT_TELEMETRY);
        return 1;
    }

    StatePub pub;
    if (!pub.open()) {
        fprintf(stderr, "nuos-tracker: cannot open %s\n", NUOS_SHM_POSE_NAME);
        return 1;
    }

    TelemHandlers handlers;
    handlers.on_pose = [&](const nuos_pose_t& p) {
        double pos[3]     = { p.pos[0],     p.pos[1],     p.pos[2] };
        double quat[4]    = { p.quat[0],    p.quat[1],    p.quat[2],    p.quat[3] };
        double vel[3]     = { p.vel[0],     p.vel[1],     p.vel[2] };
        double ang_vel[3] = { p.ang_vel[0], p.ang_vel[1], p.ang_vel[2] };
        uint64_t now = mono_ns();
        fusion.on_arkit(pos, quat, vel, ang_vel, p.tracking, now);
    };
    handlers.on_hands = [&](const uint8_t* raw, size_t len, const nuos_hands_t&) {
        static uint16_t hseq = 0;
        pub.forward_hands(raw, len, ++hseq);
    };
    handlers.on_stat = [](const nuos_stat_t&) {
        // last_stat() is read on the publish loop for telemetry merge
    };

    fprintf(stderr, "nuos-tracker: running (kiyo=%s, out=%s)\n",
            kiyo ? "yes" : "no", NUOS_SHM_POSE_NAME);

    const auto tick = std::chrono::microseconds(11111); // ~90 Hz
    auto next = std::chrono::steady_clock::now();
    while (!g_stop) {
        next += tick;

        // 1) drain telemetry socket (non-blocking, 1 ms rcv timeout)
        for (int i = 0; i < 8 && telem.poll(handlers); ++i) {}

        // 2) Kiyo VO step
        if (kiyo) {
            cv::Mat gray;
            if (cam.read_gray(gray)) {
                VoFrameResult r = vo.process(gray);
                fusion.on_vo(r.rvec, r.t_dir, r.ok, mono_ns());
            }
            fusion.set_kiyo_alive(vo.tracked_points() >= vo.min_points());
        }

        // 3) publish fused state (~90 Hz) + 10 Hz localhost packet
        uint64_t now = mono_ns();
        FusedState fs = fusion.sample(now);
        pub.publish_pose(fs);
        pub.send_state_10hz(fs, now);

        std::this_thread::sleep_until(next);
    }

    telem.stop();
    cam.close();
    fprintf(stderr, "nuos-tracker: stopped\n");
    return 0;
}
