// EKF-lite fusion of ARKit head pose (phone UDP 48011) with Kiyo mono VO.
//
// Orientation: complementary filter with weight ~0.95 on ARKit; Kiyo's
// rotation integral only corrects YAW drift (the axis ARKit cannot anchor).
// The yaw error is the ARKit-vs-Kiyo rotation-integral delta, low-passed.
// Position: ARKit position + velocity extrapolation capped at 1.5 frames.
// Graceful degradation: no Kiyo -> pure ARKit passthrough.
#ifndef NUOS_FUSION_HPP
#define NUOS_FUSION_HPP

#include <opencv2/core.hpp>
#include <opencv2/core/quaternion.hpp>
#include <cstdint>

namespace nuos {

struct FusedState {
    double pos[3]     = {0,0,0};
    double quat[4]    = {1,0,0,0};   // w x y z
    double vel[3]     = {0,0,0};
    double ang_vel[3] = {0,0,0};
    uint8_t tracking  = 0;
    uint8_t kiyo_ok   = 0;
    uint8_t arkit_ok  = 0;
    uint64_t t_mono_ns = 0;
};

class Fusion {
public:
    Fusion();

    // ARKit pose packet arrival (phone monotonic t_ns is used only for
    // ordering; board CLOCK_MONOTONIC stamps the output).
    void on_arkit(const double pos[3], const double quat_wxyz[4],
                  const double vel[3], const double ang_vel[3],
                  uint8_t tracking, uint64_t board_mono_ns);

    // Kiyo relative VO result (axis-angle rotation, unit translation dir).
    void on_vo(const double rvec[3], const double t_dir[3], bool ok,
               uint64_t board_mono_ns);

    // Produce the fused state for output (with bounded extrapolation).
    FusedState sample(uint64_t board_mono_ns) const;

    void set_kiyo_alive(bool alive) { kiyo_alive_ = alive; }
    bool arkit_alive() const { return arkit_alive_; }

    static const double kArkitWeight;      // 0.95
    static const double kYawLpTau;         // low-pass time constant, seconds
    static const double kExtrapMaxFrames;  // 1.5

private:
    static cv::Quatd slerp(const cv::Quatd& a, const cv::Quatd& b, double t);

    // mutable because sample() nudges the yaw-correction state
    mutable cv::Quatd arkit_q_{1,0,0,0};
    mutable cv::Quatd fused_q_{1,0,0,0};
    mutable double yaw_corr_ = 0;      // accumulated Kiyo yaw correction, rad
    mutable uint64_t last_arkit_ns_ = 0;
    uint64_t last_vo_ns_ = 0;
    double pos_[3] = {0,0,0};
    double vel_[3] = {0,0,0};
    double ang_vel_[3] = {0,0,0};
    uint8_t tracking_ = 0;
    bool have_arkit_ = false;
    bool kiyo_alive_ = false;
    mutable bool arkit_alive_ = false;
    uint64_t stamp_now() const;
};

} // namespace nuos
#endif
