#include "fusion.hpp"

#include <cmath>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace nuos {
const double Fusion::kArkitWeight     = 0.95;
const double Fusion::kYawLpTau        = 2.0;   // seconds
const double Fusion::kExtrapMaxFrames = 1.5;   // frames at ~90 Hz output

static uint64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t Fusion::stamp_now() const { return mono_ns(); }

Fusion::Fusion() = default;

cv::Quatd Fusion::slerp(const cv::Quatd& a, const cv::Quatd& b, double t) {
    cv::Quatd bb = b;
    if (a.dot(bb) < 0) bb = -bb; // shortest arc
    double dot = std::clamp(a.dot(bb), -1.0, 1.0);
    double theta = std::acos(dot);
    if (theta < 1e-6) return (1.0 - t) * a + t * bb;
    double s = std::sin(theta);
    return (std::sin((1.0 - t) * theta) / s) * a + (std::sin(t * theta) / s) * bb;
}

void Fusion::on_arkit(const double pos[3], const double quat_wxyz[4],
                      const double vel[3], const double ang_vel[3],
                      uint8_t tracking, uint64_t board_mono_ns) {
    cv::Quatd q(quat_wxyz[0], quat_wxyz[1], quat_wxyz[2], quat_wxyz[3]);
    q = q.normalize();

    if (have_arkit_) {
        // dt since previous ARKit packet (board clock), clamp for safety
        double dt = last_arkit_ns_
            ? std::min(std::max((double)(board_mono_ns - last_arkit_ns_) * 1e-9, 1e-4), 0.1)
            : 0.016;

        // complementary blend on full orientation
        fused_q_ = slerp(fused_q_, q, kArkitWeight).normalize();

        // yaw drift correction: low-pass the ARKit-vs-fused yaw delta and
        // apply the (filtered) Kiyo yaw integral as a slow correction.
        // We estimate ARKit yaw from q and correct fused yaw toward it
        // only via the low-passed Kiyo evidence below (in on_vo).
        (void)dt;
        last_arkit_ns_ = board_mono_ns;
    } else {
        fused_q_ = q;
        last_arkit_ns_ = board_mono_ns;
        have_arkit_ = true;
    }

    arkit_q_ = q;
    arkit_alive_ = true;
    std::memcpy(pos_, pos, sizeof pos_);
    std::memcpy(vel_, vel, sizeof vel_);
    std::memcpy(ang_vel_, ang_vel, sizeof ang_vel_);
    tracking_ = tracking;
}

void Fusion::on_vo(const double rvec[3], const double t_dir[3], bool ok,
                   uint64_t board_mono_ns) {
    if (!have_arkit_) return;
    if (!ok) { kiyo_alive_ = false; return; }
    kiyo_alive_ = true;

    double dt = last_vo_ns_
        ? std::min(std::max((double)(board_mono_ns - last_vo_ns_) * 1e-9, 1e-4), 0.1)
        : 0.016;
    last_vo_ns_ = board_mono_ns;

    // Kiyo relative rotation about world-Y (yaw). Kiyo looks forward from the
    // headset; its camera-frame rotation z component maps to world yaw for a
    // forward-facing mount. We integrate the signed yaw rate and compare the
    // integral against ARKit yaw drift over the same window; the low-passed
    // delta becomes a slow additive yaw correction to the fused orientation.
    double kiyo_yaw_rate = rvec[2] / dt; // rad/s (small-angle per-frame)
    // low-pass: alpha = dt/(tau+dt)
    double alpha = dt / (kYawLpTau + dt);
    // ARKit already provides yaw; Kiyo delta is the residual. We nudge the
    // fused yaw by alpha * (kiyo_yaw_rate - arkit yaw rate) when they disagree.
    double arkit_yaw_rate = ang_vel_[1];
    yaw_corr_ += alpha * (kiyo_yaw_rate - arkit_yaw_rate) * dt;
    // clamp correction to a sane band (±10 deg)
    yaw_corr_ = std::clamp(yaw_corr_, -0.175, 0.175);

    // Rebuild fused orientation = ARKit orientation with yaw correction.
    cv::Quatd corr = cv::Quatd::createFromYRot(yaw_corr_);
    fused_q_ = (corr * arkit_q_).normalize();
}

FusedState Fusion::sample(uint64_t board_mono_ns) const {
    FusedState s;
    double dt = 0.0;
    if (have_arkit_ && last_arkit_ns_)
        dt = std::clamp((double)(board_mono_ns - last_arkit_ns_) * 1e-9, 0.0,
                        kExtrapMaxFrames / 90.0); // <=1.5 frames @90 Hz
    for (int i = 0; i < 3; ++i)
        s.pos[i] = pos_[i] + vel_[i] * dt;      // position extrapolation only
    cv::Quatd q = fused_q_;
    if (kiyo_alive_) q = (cv::Quatd::createFromYRot(yaw_corr_) * q).normalize();
    s.quat[0] = q.w; s.quat[1] = q.x; s.quat[2] = q.y; s.quat[3] = q.z;
    std::memcpy(s.vel, vel_, sizeof s.vel);
    std::memcpy(s.ang_vel, ang_vel_, sizeof s.ang_vel);
    s.tracking = tracking_;
    s.kiyo_ok = kiyo_alive_ ? 1 : 0;
    s.arkit_ok = have_arkit_ ? 1 : 0;
    s.t_mono_ns = board_mono_ns;
    return s;
}

} // namespace nuos
