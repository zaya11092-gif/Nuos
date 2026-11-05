#include "v4l2_source.hpp"

#include <cstring>
#include <cstdio>

namespace nuos {

bool CameraSource::open(const SourceConfig& cfg) {
    cfg_ = cfg;
    cap_.release();
    if (cfg.replay) {
        if (!cap_.open(cfg.device_or_file, cv::CAP_FFMPEG)) return false;
    } else {
        // MJPG @ 60fps 1280x720 — Razer Kiyo native streaming format.
        cap_.open(cfg.device_or_file, cv::CAP_V4L2);
        if (!cap_.isOpened()) return false;
        cap_.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M','J','P','G'));
        cap_.set(cv::CAP_PROP_FRAME_WIDTH,  cfg.want_width);
        cap_.set(cv::CAP_PROP_FRAME_HEIGHT, cfg.want_height);
        cap_.set(cv::CAP_PROP_FPS, cfg.want_fps);
    }
    fps_ = cap_.get(cv::CAP_PROP_FPS);
    return cap_.isOpened();
}

void CameraSource::close() { cap_.release(); }

bool CameraSource::read_gray(cv::Mat& gray) {
    if (!cap_.isOpened()) return false;
    cv::Mat frame;
    if (!cap_.read(frame) || frame.empty()) return false;
    if (frame.channels() == 3)
        cv::cvtColor(frame, frame, cv::COLOR_BGR2GRAY);
    else if (frame.channels() == 4)
        cv::cvtColor(frame, frame, cv::COLOR_BGRA2GRAY);
    cv::resize(frame, work_, cv::Size(cfg_.vo_width, cfg_.vo_height), 0, 0,
               cv::INTER_AREA);
    gray = work_;
    return true;
}

} // namespace nuos
