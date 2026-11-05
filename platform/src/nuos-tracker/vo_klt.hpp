// Sparse KLT visual odometry on 320x180 grayscale Kiyo frames.
//
// MONO VO IS SCALE-AMBIGUOUS: a single camera cannot observe absolute
// translation magnitude. recoverPose() yields a UNIT translation direction
// only. The fusion layer therefore uses rotation absolutely (correcting ARKit
// yaw drift) and translation direction qualitatively (sanity/displacement
// gating), never as a metric position source.
#ifndef NUOS_VO_KLT_HPP
#define NUOS_VO_KLT_HPP

#include <opencv2/core.hpp>
#include <vector>

namespace nuos {

struct VoFrameResult {
    bool   ok = false;             // pose recovered this frame
    double rvec[3] = {0,0,0};      // relative rotation, axis-angle, frame N-1 -> N
    double t_dir[3] = {0,0,0};     // UNIT translation direction (camera frame)
    int    inliers = 0;
};

class VoKlt {
public:
    VoKlt();

    // Feed the next grayscale frame; produces relative motion vs. previous.
    VoFrameResult process(const cv::Mat& gray);

    void reset();

    int  tracked_points() const { return last_tracked_; }
    int  min_points() const { return min_pts_; }

private:
    cv::Mat prev_gray_;
    std::vector<cv::Point2f> prev_pts_;
    cv::Mat K_;
    int last_tracked_ = 0;
    const int min_pts_ = 40;
    const int max_pts_ = 300;
    const double quality_ = 0.01;
    const double min_dist_ = 8.0;
};

} // namespace nuos
#endif
