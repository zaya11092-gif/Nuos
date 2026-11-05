// Synthetic-motion VO test: generates a feature-rich video with known affine
// motion (cv::VideoWriter) if the fixture is missing, runs the KLT VO path,
// and asserts the recovered trajectory is finite, smooth, low-jitter.
//
// Fixture: $NUOS_TEST_FIXTURE_DIR/nuos_vo_fixture.mp4 (written when missing).
#include "v4l2_source.hpp"
#include "vo_klt.hpp"

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>

using namespace nuos;

static std::string fixture_dir() {
    const char* d = getenv("NUOS_TEST_FIXTURE_DIR");
    return d ? d : ".";
}

// Known motion: pure horizontal pan, 2 px/frame at 320x180 (≈8 px/frame at
// source scale). Yaw axis-angle should be small and roughly constant sign.
static void make_fixture(const std::string& path) {
    // 3D scene: pseudo-random point cloud rendered under perspective while the
    // camera translates laterally (planar scenes make E degenerate).
    const double fx = 0.9 * 320, cx = 160.0, cy = 90.0;
    cv::RNG rng(20260101);
    std::vector<cv::Point3d> pts3d;
    std::vector<cv::Scalar> cols;
    std::vector<int> radii;
    for (int i = 0; i < 400; ++i) {
        pts3d.emplace_back(rng.uniform(-1.2, 1.2), rng.uniform(-0.7, 0.7),
                           rng.uniform(1.5, 6.0));
        cols.emplace_back(rng.uniform(40, 220), rng.uniform(40, 220),
                          rng.uniform(40, 220));
        radii.push_back(rng.uniform(2, 4));
    }
    cv::VideoWriter w(path, cv::VideoWriter::fourcc('m','p','4','v'), 60,
                      cv::Size(320, 180));
    for (int f = 0; f < 120; ++f) {
        cv::Mat img(180, 320, CV_8UC3, cv::Vec3b(90, 90, 90));
        double camx = -0.03 * f; // lateral camera motion
        for (size_t i = 0; i < pts3d.size(); ++i) {
            double u = cx + fx * (pts3d[i].x - camx) / pts3d[i].z;
            double v = cy + fx * pts3d[i].y / pts3d[i].z;
            if (u < 2 || u > 318 || v < 2 || v > 178) continue;
            cv::circle(img, cv::Point((int)u, (int)v), radii[i], cols[i], -1,
                       cv::LINE_AA);
        }
        cv::Mat blurred;
        cv::GaussianBlur(img, blurred, cv::Size(0, 0), 0.5);
        w.write(blurred);
    }
    w.release();
    fprintf(stderr, "fixture written: %s\n", path.c_str());
}

int main() {
    std::string path = fixture_dir() + "/nuos_vo_fixture.mp4";
    {
        cv::VideoWriter probe(path, cv::VideoWriter::fourcc('m','p','4','v'), 60,
                              cv::Size(320, 180));
        if (!probe.isOpened()) return 0; // no video backend in CI: skip
        probe.release();
        remove(path.c_str()); // probe succeeded; regenerate real fixture
        make_fixture(path);
    }

    SourceConfig cfg;
    cfg.device_or_file = path;
    cfg.replay = true;
    CameraSource cam;
    if (!cam.open(cfg)) return 0; // backend-less CI: skip softly

    VoKlt vo;
    cv::Mat gray;
    int frames = 0, ok_frames = 0;
    double yaw_sum = 0, yaw_sq = 0, prev_yaw = 0, jitter_sum = 0;
    const double max_step = 0.15; // rad per frame — low-jitter bound

    while (cam.read_gray(gray)) {
        ++frames;
        VoFrameResult r = vo.process(gray);
        if (!r.ok) continue;
        double yaw = r.rvec[2]; // dominant component for a pan
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(r.rvec[i]) || !std::isfinite(r.t_dir[i])) {
                fprintf(stderr, "non-finite VO output\n");
                return 1;
            }
        }
        ++ok_frames;
        yaw_sum += yaw;
        yaw_sq += yaw * yaw;
        if (ok_frames > 1) jitter_sum += std::fabs(yaw - prev_yaw);
        prev_yaw = yaw;
        if (ok_frames > 3 && std::fabs(yaw - prev_yaw) > max_step) {
            fprintf(stderr, "jitter too high: %f rad jump\n", yaw - prev_yaw);
            return 1;
        }
    }

    if (frames < 50) {
        fprintf(stderr, "too few frames decoded (%d) — skip\n", frames);
        return 0; // codec availability varies; do not hard-fail CI
    }
    if (ok_frames < frames / 4) {
        fprintf(stderr, "VO failed on most frames (%d/%d)\n", ok_frames, frames);
        return 1;
    }
    double mean = yaw_sum / ok_frames;
    double var = yaw_sq / ok_frames - mean * mean;
    double jitter_mean = jitter_sum / (ok_frames - 1);
    fprintf(stderr, "frames=%d ok=%d mean_yaw=%.4f var=%.2e jitter=%.4f\n",
            frames, ok_frames, mean, var, jitter_mean);
    if (!std::isfinite(mean) || !std::isfinite(var)) return 1;
    if (jitter_mean > 0.05) {
        fprintf(stderr, "mean jitter %.4f exceeds 0.05 rad\n", jitter_mean);
        return 1;
    }
    printf("replay_tracker_test: PASS\n");
    return 0;
}
