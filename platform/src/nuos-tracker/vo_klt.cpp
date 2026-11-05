#include "vo_klt.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/video.hpp>
#include <opencv2/calib3d.hpp>
#include <cmath>

namespace nuos {

// Approximate Kiyo intrinsics at 320x180 (FOV ~90deg horizontal, derived by
// scaling the 1280x720 factory assumption; refined calibration can replace K_).
static cv::Mat default_intrinsics(int w, int h) {
    double cx = w / 2.0, cy = h / 2.0;
    double fx = 0.9 * w;   // ~focal in pixels for 90deg HFOV
    double fy = fx;
    return (cv::Mat_<double>(3,3) <<
        fx, 0, cx,
        0, fy, cy,
        0, 0, 1);
}

VoKlt::VoKlt() {
    K_ = default_intrinsics(320, 180);
}

void VoKlt::reset() {
    prev_gray_.release();
    prev_pts_.clear();
    last_tracked_ = 0;
}

VoFrameResult VoKlt::process(const cv::Mat& gray) {
    VoFrameResult res;
    if (gray.empty()) return res;

    if (prev_gray_.empty()) {
        cv::goodFeaturesToTrack(gray, prev_pts_, max_pts_, quality_, min_dist_);
        gray.copyTo(prev_gray_);
        last_tracked_ = (int)prev_pts_.size();
        return res; // no previous frame -> no relative motion yet
    }

    if (prev_pts_.size() < (size_t)min_pts_) {
        cv::goodFeaturesToTrack(prev_gray_, prev_pts_, max_pts_, quality_, min_dist_);
        if (prev_pts_.size() < (size_t)min_pts_) {
            reset();
            cv::goodFeaturesToTrack(gray, prev_pts_, max_pts_, quality_, min_dist_);
            gray.copyTo(prev_gray_);
            return res;
        }
    }

    std::vector<cv::Point2f> next_pts;
    std::vector<uchar> status;
    std::vector<float> err;
    cv::calcOpticalFlowPyrLK(prev_gray_, gray, prev_pts_, next_pts,
                             status, err,
                             cv::Size(21,21), 3,
                             cv::TermCriteria(cv::TermCriteria::COUNT+cv::TermCriteria::EPS, 30, 0.01));

    std::vector<cv::Point2f> p1, p2;
    p1.reserve(next_pts.size()); p2.reserve(next_pts.size());
    for (size_t i = 0; i < next_pts.size(); ++i)
        if (status[i]) { p1.push_back(prev_pts_[i]); p2.push_back(next_pts[i]); }
    last_tracked_ = (int)p1.size();

    if (p1.size() >= (size_t)min_pts_) {
        cv::Mat R, t;
        cv::Mat inlier_mask;
        cv::Mat E = cv::findEssentialMat(p1, p2, K_, cv::RANSAC, 0.999, 1.0, inlier_mask);
        if (!E.empty() &&
            cv::recoverPose(E, p1, p2, K_, R, t, inlier_mask) > (size_t)min_pts_/2) {
            cv::Mat rvec;
            cv::Rodrigues(R, rvec);
            for (int i = 0; i < 3; ++i) res.rvec[i] = rvec.at<double>(i);
            double n = cv::norm(t);
            if (n > 1e-9) {
                for (int i = 0; i < 3; ++i) res.t_dir[i] = t.at<double>(i) / n;
            }
            res.inliers = (int)cv::countNonZero(inlier_mask);
            res.ok = true;
        }
    }

    // reseed features for the next frame
    std::vector<cv::Point2f> seeds;
    cv::goodFeaturesToTrack(gray, seeds, max_pts_, quality_, min_dist_);
    if (!seeds.empty() && res.ok) {
        // keep flow endpoints plus fresh corners for coverage
        prev_pts_ = p2;
        for (auto& s : seeds) prev_pts_.push_back(s);
        if (prev_pts_.size() > (size_t)max_pts_ * 2)
            prev_pts_.assign(prev_pts_.end() - max_pts_, prev_pts_.end());
    } else {
        prev_pts_ = seeds;
    }
    gray.copyTo(prev_gray_);
    return res;
}

} // namespace nuos
