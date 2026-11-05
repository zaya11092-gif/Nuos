// NuOS Kiyo camera source: MJPG 1280x720@60 via V4L2/OpenCV, downscaled to
// grayscale 320x180 for KLT visual odometry.
#ifndef NUOS_V4L2_SOURCE_HPP
#define NUOS_V4L2_SOURCE_HPP

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>
#include <string>

namespace nuos {

struct SourceConfig {
    std::string device_or_file;   // "/dev/video0" or path to a video file
    bool replay = false;          // true = file replay
    int  want_width  = 1280;
    int  want_height = 720;
    int  want_fps    = 60;
    int  vo_width    = 320;       // grayscale working resolution
    int  vo_height   = 180;
};

class CameraSource {
public:
    // Opens the capture; returns false on failure (graceful degradation:
    // tracker runs in ARKit passthrough when this returns false).
    bool open(const SourceConfig& cfg);
    void close();
    bool is_open() const { return cap_.isOpened(); }

    // Grabs the next frame and returns the 1-channel grayscale VO image.
    // Returns false when no frame is available this tick (timeout) or EOF.
    bool read_gray(cv::Mat& gray);

    double reported_fps() const { return fps_; }

private:
    cv::VideoCapture cap_;
    cv::Mat work_;
    double fps_ = 0;
    SourceConfig cfg_;
};

} // namespace nuos
#endif
